/*
 ** Copyright 2016, The Android Open Source Project
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <fcntl.h>
#include <linux/unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <sstream>

#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/stringprintf.h>
#include <selinux/android.h>

#include <apexd.h>

#include "installd_constants.h"
#include "otapreopt_utils.h"

#ifndef LOG_TAG
#define LOG_TAG "otapreopt"
#endif

using android::base::StringPrintf;

namespace android {
namespace installd {

// Configuration for bind-mounted Bionic artifacts.

static constexpr const char* kLinkerMountPoint = "/bionic/bin/linker";
static constexpr const char* kRuntimeLinkerPath = "/apex/com.android.runtime/bin/linker";

static constexpr const char* kBionicLibsMountPointDir = "/bionic/lib/";
static constexpr const char* kRuntimeBionicLibsDir = "/apex/com.android.runtime/lib/bionic/";

static constexpr const char* kLinkerMountPoint64 = "/bionic/bin/linker64";
static constexpr const char* kRuntimeLinkerPath64 = "/apex/com.android.runtime/bin/linker64";

static constexpr const char* kBionicLibsMountPointDir64 = "/bionic/lib64/";
static constexpr const char* kRuntimeBionicLibsDir64 = "/apex/com.android.runtime/lib64/bionic/";

static const std::vector<std::string> kBionicLibFileNames = {"libc.so", "libm.so", "libdl.so"};


static void CloseDescriptor(int fd) {
    if (fd >= 0) {
        int result = close(fd);
        UNUSED(result);  // Ignore result. Printing to logcat will open a new descriptor
                         // that we do *not* want.
    }
}

static void CloseDescriptor(const char* descriptor_string) {
    int fd = -1;
    std::istringstream stream(descriptor_string);
    stream >> fd;
    if (!stream.fail()) {
        CloseDescriptor(fd);
    }
}

static std::vector<apex::ApexFile> ActivateApexPackages() {
    // The logic here is (partially) copied and adapted from
    // system/apex/apexd/apexd_main.cpp.
    //
    // Only scan the APEX directory under /system (within the chroot dir).
    apex::scanPackagesDirAndActivate(apex::kApexPackageSystemDir);
    return apex::getActivePackages();
}

static void DeactivateApexPackages(const std::vector<apex::ApexFile>& active_packages) {
    for (const apex::ApexFile& apex_file : active_packages) {
        const std::string& package_path = apex_file.GetPath();
        apex::Status status = apex::deactivatePackage(package_path);
        if (!status.Ok()) {
            LOG(ERROR) << "Failed to deactivate " << package_path << ": " << status.ErrorMessage();
        }
    }
}

// Copied from system/core/init/mount_namespace.cpp.
static bool BindMount(const std::string& source, const std::string& mount_point,
                      bool recursive = false) {
    unsigned long mountflags = MS_BIND;
    if (recursive) {
        mountflags |= MS_REC;
    }
    if (mount(source.c_str(), mount_point.c_str(), nullptr, mountflags, nullptr) == -1) {
        PLOG(ERROR) << "Could not bind-mount " << source << " to " << mount_point;
        return false;
    }
    return true;
}

// Copied from system/core/init/mount_namespace.cpp and and adjusted (bind
// mounts are not made private, as the /postinstall is already private (see
// `android::installd::otapreopt_chroot`).
static bool BindMountBionic(const std::string& linker_source, const std::string& lib_dir_source,
                            const std::string& linker_mount_point,
                            const std::string& lib_mount_dir) {
    if (access(linker_source.c_str(), F_OK) != 0) {
        PLOG(INFO) << linker_source << " does not exist. Skipping mounting Bionic there.";
        return true;
    }
    if (!BindMount(linker_source, linker_mount_point)) {
        return false;
    }
    for (const auto& libname : kBionicLibFileNames) {
        std::string mount_point = lib_mount_dir + libname;
        std::string source = lib_dir_source + libname;
        if (!BindMount(source, mount_point)) {
            return false;
        }
    }
    return true;
}

// Entry for otapreopt_chroot. Expected parameters are:
//   [cmd] [status-fd] [target-slot] "dexopt" [dexopt-params]
// The file descriptor denoted by status-fd will be closed. The rest of the parameters will
// be passed on to otapreopt in the chroot.
static int otapreopt_chroot(const int argc, char **arg) {
    // Validate arguments
    // We need the command, status channel and target slot, at a minimum.
    if(argc < 3) {
        PLOG(ERROR) << "Not enough arguments.";
        exit(208);
    }
    // Close all file descriptors. They are coming from the caller, we do not want to pass them
    // on across our fork/exec into a different domain.
    // 1) Default descriptors.
    CloseDescriptor(STDIN_FILENO);
    CloseDescriptor(STDOUT_FILENO);
    CloseDescriptor(STDERR_FILENO);
    // 2) The status channel.
    CloseDescriptor(arg[1]);

    // We need to run the otapreopt tool from the postinstall partition. As such, set up a
    // mount namespace and change root.

    // Create our own mount namespace.
    if (unshare(CLONE_NEWNS) != 0) {
        PLOG(ERROR) << "Failed to unshare() for otapreopt.";
        exit(200);
    }

    // Make postinstall private, so that our changes don't propagate.
    if (mount("", "/postinstall", nullptr, MS_PRIVATE, nullptr) != 0) {
        PLOG(ERROR) << "Failed to mount private.";
        exit(201);
    }

    // Bind mount necessary directories.
    constexpr const char* kBindMounts[] = {
            "/data", "/dev", "/proc", "/sys"
    };
    for (size_t i = 0; i < arraysize(kBindMounts); ++i) {
        std::string trg = StringPrintf("/postinstall%s", kBindMounts[i]);
        if (mount(kBindMounts[i], trg.c_str(), nullptr, MS_BIND, nullptr) != 0) {
            PLOG(ERROR) << "Failed to bind-mount " << kBindMounts[i];
            exit(202);
        }
    }

    // Try to mount the vendor partition. update_engine doesn't do this for us, but we
    // want it for vendor APKs.
    // Notes:
    //  1) We pretty much guess a name here and hope to find the partition by name.
    //     It is just as complicated and brittle to scan /proc/mounts. But this requires
    //     validating the target-slot so as not to try to mount some totally random path.
    //  2) We're in a mount namespace here, so when we die, this will be cleaned up.
    //  3) Ignore errors. Printing anything at this stage will open a file descriptor
    //     for logging.
    if (!ValidateTargetSlotSuffix(arg[2])) {
        LOG(ERROR) << "Target slot suffix not legal: " << arg[2];
        exit(207);
    }
    {
      std::string vendor_partition = StringPrintf("/dev/block/by-name/vendor%s",
                                                  arg[2]);
      int vendor_result = mount(vendor_partition.c_str(),
                                "/postinstall/vendor",
                                "ext4",
                                MS_RDONLY,
                                /* data */ nullptr);
      UNUSED(vendor_result);
    }

    // Try to mount the product partition. update_engine doesn't do this for us, but we
    // want it for product APKs. Same notes as vendor above.
    {
      std::string product_partition = StringPrintf("/dev/block/by-name/product%s",
                                                   arg[2]);
      int product_result = mount(product_partition.c_str(),
                                 "/postinstall/product",
                                 "ext4",
                                 MS_RDONLY,
                                 /* data */ nullptr);
      UNUSED(product_result);
    }

    // Setup APEX mount point and its security context.
    static constexpr const char* kPostinstallApexDir = "/postinstall/apex";
    // The following logic is similar to the one in system/core/rootdir/init.rc:
    //
    //   mount tmpfs tmpfs /apex nodev noexec nosuid
    //   chmod 0755 /apex
    //   chown root root /apex
    //   restorecon /apex
    //
    // except we perform the `restorecon` step just after mounting the tmpfs
    // filesystem in /postinstall/apex, so that this directory is correctly
    // labeled (with type `postinstall_apex_mnt_dir`) and may be manipulated in
    // following operations (`chmod`, `chown`, etc.) following policies
    // restricted to `postinstall_apex_mnt_dir`:
    //
    //   mount tmpfs tmpfs /postinstall/apex nodev noexec nosuid
    //   restorecon /postinstall/apex
    //   chmod 0755 /postinstall/apex
    //   chown root root /postinstall/apex
    //
    if (mount("tmpfs", kPostinstallApexDir, "tmpfs", MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr)
        != 0) {
        PLOG(ERROR) << "Failed to mount tmpfs in " << kPostinstallApexDir;
        exit(209);
    }
    if (selinux_android_restorecon(kPostinstallApexDir, 0) < 0) {
        PLOG(ERROR) << "Failed to restorecon " << kPostinstallApexDir;
        exit(214);
    }
    if (chmod(kPostinstallApexDir, 0755) != 0) {
        PLOG(ERROR) << "Failed to chmod " << kPostinstallApexDir << " to 0755";
        exit(210);
    }
    if (chown(kPostinstallApexDir, 0, 0) != 0) {
        PLOG(ERROR) << "Failed to chown " << kPostinstallApexDir << " to root:root";
        exit(211);
    }

    // Chdir into /postinstall.
    if (chdir("/postinstall") != 0) {
        PLOG(ERROR) << "Unable to chdir into /postinstall.";
        exit(203);
    }

    // Make /postinstall the root in our mount namespace.
    if (chroot(".")  != 0) {
        PLOG(ERROR) << "Failed to chroot";
        exit(204);
    }

    if (chdir("/") != 0) {
        PLOG(ERROR) << "Unable to chdir into /.";
        exit(205);
    }

    // Try to mount APEX packages in "/apex" in the chroot dir. We need at least
    // the Android Runtime APEX, as it is required by otapreopt to run dex2oat.
    std::vector<apex::ApexFile> active_packages = ActivateApexPackages();

    // Bind-mount Bionic artifacts from the Runtime APEX.
    // This logic is copied and adapted from system/core/init/mount_namespace.cpp.
    if (!BindMountBionic(kRuntimeLinkerPath, kRuntimeBionicLibsDir, kLinkerMountPoint,
                         kBionicLibsMountPointDir)) {
        LOG(ERROR) << "Failed to mount 32-bit Bionic artifacts from the Runtime APEX.";
        // Clean up and exit.
        DeactivateApexPackages(active_packages);
        exit(215);
    }
    if (!BindMountBionic(kRuntimeLinkerPath64, kRuntimeBionicLibsDir64, kLinkerMountPoint64,
                         kBionicLibsMountPointDir64)) {
        LOG(ERROR) << "Failed to mount 64-bit Bionic artifacts from the Runtime APEX.";
        // Clean up and exit.
        DeactivateApexPackages(active_packages);
        exit(216);
    }

    // Now go on and run otapreopt.

    // Incoming:  cmd + status-fd + target-slot + cmd...      | Incoming | = argc
    // Outgoing:  cmd             + target-slot + cmd...      | Outgoing | = argc - 1
    std::vector<std::string> cmd;
    cmd.reserve(argc);
    cmd.push_back("/system/bin/otapreopt");

    // The first parameter is the status file descriptor, skip.
    for (size_t i = 2; i < static_cast<size_t>(argc); ++i) {
        cmd.push_back(arg[i]);
    }

    // Fork and execute otapreopt in its own process.
    std::string error_msg;
    bool exec_result = Exec(cmd, &error_msg);
    if (!exec_result) {
        LOG(ERROR) << "Running otapreopt failed: " << error_msg;
    }

    // Tear down the work down by the apexd logic. (i.e. deactivate packages).
    DeactivateApexPackages(active_packages);

    if (!exec_result) {
        exit(213);
    }

    return 0;
}

}  // namespace installd
}  // namespace android

int main(const int argc, char *argv[]) {
    return android::installd::otapreopt_chroot(argc, argv);
}
