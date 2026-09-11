#pragma once

#include <cstdint>
#include <string>

namespace docxstudio::worker {

// These limits are intentionally suitable for a short-lived parser child,
// not for the GUI process. installParserSandbox() is irreversible.
struct SandboxLimits {
    std::uint64_t address_space_bytes{768ULL * 1024ULL * 1024ULL};
    std::uint64_t cpu_seconds{10};
    std::uint64_t output_file_bytes{0};
    std::uint64_t open_file_descriptors{16};
    std::uint64_t child_processes{0};
};

enum class SandboxStage {
    none,
    resource_limits,
    process_hardening,
    seccomp_filter,
    unsupported_platform,
};

struct SandboxError {
    SandboxStage stage{SandboxStage::none};
    int native_error{0};
    std::string message;
};

// Applies RLIMIT_AS/CPU/FSIZE/NOFILE/NPROC/CORE, disables dumpability, sets a
// parent-death signal and PR_SET_NO_NEW_PRIVS, then loads a libseccomp filter.
// The filter permits descriptor-based parsing but returns EPERM for socket
// creation/network setup, path-based filesystem access and mutation,
// process creation/exec/ptrace, namespaces/mounts, io_uring, BPF and related
// escape surfaces. Call this only in a dedicated child process.
[[nodiscard]] bool installParserSandbox(
    const SandboxLimits& limits = {},
    SandboxError* error = nullptr);

}  // namespace docxstudio::worker
