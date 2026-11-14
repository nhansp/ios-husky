// SPDX-License-Identifier: MIT
// From UniXNU AdrenoMetal (MIT); the system-wide cache layer for the render server is dropped.
#pragma once
#include <string>
#include <pwd.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <vector>

// Processes of one user share immutable, version-keyed shader entries. A sandbox
// that cannot write this directory continues with its container's cache. The
// sandbox is asked first without a report: a refused probe file made the sandbox
// write a violation report (with a backtrace) at every launch of every sandboxed
// Metal process (iconservicesagent, widgets).
extern "C" int sandbox_check(pid_t pid, const char *operation, int type, ...);
inline std::string sharedShaderCacheDirectory() {
    static const std::string path = []() -> std::string {
        std::string root;
        if (const char *override = getenv("HUSKYVK_SHARED_CACHE_DIR")) {
            if (!*override) return {};
            root = override;
        } else {
            passwd user{}, *found=nullptr;
            std::vector<char> bytes(16384);
            if (getpwuid_r(getuid(), &user, bytes.data(), bytes.size(), &found) || !found || !user.pw_dir) return {};
            root = std::string(user.pw_dir) + "/Library/Caches/org.husky.vkmetal";
        }
        mkdir(root.c_str(), 0700);
        struct stat st = {};
        if (lstat(root.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0022)) return {};
        // A sandboxed process (sandbox_check with no operation answers 1) asks for the write first: SANDBOX_FILTER_PATH (1) |
        // SANDBOX_CHECK_NO_REPORT (0x40000000), nonzero is a refusal.
        if (sandbox_check(getpid(), nullptr, 0) == 1 &&
            sandbox_check(getpid(), "file-write-create", 1 | 0x40000000, (root + "/.write-probe").c_str()) != 0) return {};
        std::string probe = root + "/.write-XXXXXX";
        int fd = mkstemp(probe.data());
        if (fd < 0) return {};
        close(fd); unlink(probe.c_str());
        return root;
    }();
    return path;
}

// Call before the first Vulkan device is opened, including the optional smoke path.
inline void configureShaderDiskCache(const std::string &root) {
    if (root.empty()) return;
    const std::string mesa = root + "/mesa";   // panvk's own disk cache of compiled pipelines
    mkdir(mesa.c_str(), 0700);
    if (access(mesa.c_str(), W_OK) == 0) setenv("MESA_SHADER_CACHE_DIR", mesa.c_str(), 0);
}
