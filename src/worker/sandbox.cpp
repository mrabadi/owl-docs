#include "docxstudio/worker/sandbox.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#if defined(__linux__)
#include <seccomp.h>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace docxstudio::worker {
namespace {

void setError(
    SandboxError* error,
    SandboxStage stage,
    int native_error,
    std::string message) {
    if (error != nullptr) {
        *error = SandboxError{stage, native_error, std::move(message)};
    }
}

#if defined(__linux__)

rlim_t boundedLimit(std::uint64_t requested) {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<rlim_t>::max());
    return static_cast<rlim_t>(std::min(requested, maximum));
}

bool lowerLimit(int resource, std::uint64_t requested, const char* name, SandboxError* error) {
    struct rlimit current {};
    if (::getrlimit(resource, &current) != 0) {
        const int native_error = errno;
        setError(
            error,
            SandboxStage::resource_limits,
            native_error,
            std::string("getrlimit(") + name + ") failed: " + std::strerror(native_error));
        return false;
    }
    rlim_t value = boundedLimit(requested);
    if (current.rlim_max != RLIM_INFINITY) {
        value = std::min(value, current.rlim_max);
    }
    const struct rlimit restricted {value, value};
    if (::setrlimit(resource, &restricted) != 0) {
        const int native_error = errno;
        setError(
            error,
            SandboxStage::resource_limits,
            native_error,
            std::string("setrlimit(") + name + ") failed: " + std::strerror(native_error));
        return false;
    }
    return true;
}

bool installResourceLimits(const SandboxLimits& limits, SandboxError* error) {
    if (limits.address_space_bytes == 0 || limits.cpu_seconds == 0 ||
        limits.open_file_descriptors < 3) {
        setError(
            error,
            SandboxStage::resource_limits,
            EINVAL,
            "Address-space and CPU limits must be non-zero and the descriptor limit must be at least three");
        return false;
    }
    return lowerLimit(RLIMIT_AS, limits.address_space_bytes, "RLIMIT_AS", error) &&
           lowerLimit(RLIMIT_CPU, limits.cpu_seconds, "RLIMIT_CPU", error) &&
           lowerLimit(RLIMIT_FSIZE, limits.output_file_bytes, "RLIMIT_FSIZE", error) &&
           lowerLimit(RLIMIT_NOFILE, limits.open_file_descriptors, "RLIMIT_NOFILE", error) &&
           lowerLimit(RLIMIT_NPROC, limits.child_processes, "RLIMIT_NPROC", error) &&
           lowerLimit(RLIMIT_CORE, 0, "RLIMIT_CORE", error);
}

bool hardenProcess(SandboxError* error) {
    const pid_t parent = ::getppid();
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0) {
        const int native_error = errno;
        setError(
            error,
            SandboxStage::process_hardening,
            native_error,
            std::string("PR_SET_PDEATHSIG failed: ") + std::strerror(native_error));
        return false;
    }
    if (::getppid() != parent) {
        setError(
            error,
            SandboxStage::process_hardening,
            ESRCH,
            "Parser parent exited while the sandbox was being installed");
        return false;
    }
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        const int native_error = errno;
        setError(
            error,
            SandboxStage::process_hardening,
            native_error,
            std::string("PR_SET_DUMPABLE failed: ") + std::strerror(native_error));
        return false;
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        const int native_error = errno;
        setError(
            error,
            SandboxStage::process_hardening,
            native_error,
            std::string("PR_SET_NO_NEW_PRIVS failed: ") + std::strerror(native_error));
        return false;
    }
    return true;
}

bool addErrnoRules(
    scmp_filter_ctx filter,
    const char* const* names,
    std::size_t count,
    SandboxError* error) {
    for (std::size_t index = 0; index < count; ++index) {
        const int syscall_number = seccomp_syscall_resolve_name(names[index]);
        if (syscall_number == __NR_SCMP_ERROR) {
            continue;
        }
        const int result =
            seccomp_rule_add(filter, SCMP_ACT_ERRNO(static_cast<std::uint16_t>(EPERM)), syscall_number, 0);
        if (result < 0) {
            const int native_error = -result;
            setError(
                error,
                SandboxStage::seccomp_filter,
                native_error,
                std::string("Cannot deny syscall ") + names[index] + ": " +
                    std::strerror(native_error));
            return false;
        }
    }
    return true;
}

bool restrictPrctl(scmp_filter_ctx filter, SandboxError* error) {
    const int syscall_number = seccomp_syscall_resolve_name("prctl");
    if (syscall_number == __NR_SCMP_ERROR) {
        return true;
    }
    const int result = seccomp_rule_add(
        filter,
        SCMP_ACT_ERRNO(static_cast<std::uint16_t>(EPERM)),
        syscall_number,
        1,
        SCMP_A0(SCMP_CMP_NE, PR_GET_NO_NEW_PRIVS));
    if (result >= 0) {
        return true;
    }
    const int native_error = -result;
    setError(
        error,
        SandboxStage::seccomp_filter,
        native_error,
        std::string("Cannot restrict prctl: ") + std::strerror(native_error));
    return false;
}

bool installSeccompFilter(SandboxError* error) {
    scmp_filter_ctx filter = seccomp_init(SCMP_ACT_ALLOW);
    if (filter == nullptr) {
        setError(error, SandboxStage::seccomp_filter, ENOMEM, "seccomp_init failed");
        return false;
    }

    // No socket creation or socket configuration. Descriptor protocol I/O
    // uses read/write on a pipe supplied by the parent.
    static constexpr std::array network_syscalls{
        "socket",       "socketpair", "connect",    "bind",       "listen",
        "accept",       "accept4",    "sendto",     "sendmsg",    "sendmmsg",
        "recvfrom",     "recvmsg",    "recvmmsg",   "shutdown",   "setsockopt",
        "getsockopt",   "getsockname", "getpeername", "socketcall"};

    // The document and response descriptors must already exist. fstat, read,
    // pread, lseek, mmap and close remain available to libzip/pugixml.
    static constexpr std::array filesystem_syscalls{
        "open",          "openat",          "openat2",       "creat",
        "stat",          "lstat",           "newfstatat",    "statx",
        "access",        "faccessat",        "faccessat2",    "readlink",
        "readlinkat",    "getdents",         "getdents64",    "chdir",
        "fchdir",        "truncate",         "ftruncate",     "rename",
        "renameat",      "renameat2",        "unlink",        "unlinkat",
        "link",          "linkat",           "symlink",       "symlinkat",
        "mkdir",         "mkdirat",          "rmdir",         "mknod",
        "mknodat",       "chmod",            "fchmod",        "fchmodat",
        "chown",         "fchown",           "lchown",        "fchownat",
        "utime",         "utimes",           "utimensat",     "futimesat",
        "statfs",        "ustat",            "getxattr",      "lgetxattr",
        "listxattr",     "llistxattr",       "inotify_init",  "inotify_init1",
        "inotify_add_watch", "inotify_rm_watch", "fanotify_init", "fanotify_mark",
        "setxattr",      "lsetxattr",        "fsetxattr",     "removexattr",
        "lremovexattr",  "fremovexattr",     "mount",         "umount",
        "umount2",       "pivot_root",       "chroot",        "swapon",
        "swapoff",       "quotactl",         "name_to_handle_at",
        "open_by_handle_at", "open_tree",    "move_mount",    "fsopen",
        "fsconfig",      "fsmount",          "fspick",        "mount_setattr"};

    static constexpr std::array process_syscalls{
        "clone",          "clone3",          "fork",             "vfork",
        "execve",         "execveat",        "ptrace",           "process_vm_readv",
        "process_vm_writev", "kcmp",         "unshare",          "setns",
        "pidfd_open",     "pidfd_getfd",      "pidfd_send_signal", "kill",
        "tkill",          "tgkill",          "rt_sigqueueinfo",  "rt_tgsigqueueinfo",
        "setuid",         "setgid",          "setreuid",         "setregid",
        "setresuid",      "setresgid",       "setfsuid",         "setfsgid",
        "setgroups",      "capset",          "personality",      "setrlimit",
        "prlimit64",      "seccomp"};

    static constexpr std::array kernel_attack_surface_syscalls{
        "io_uring_setup", "io_uring_enter", "io_uring_register", "bpf",
        "perf_event_open", "userfaultfd",   "keyctl",            "add_key",
        "request_key",    "reboot",         "kexec_load",        "kexec_file_load",
        "init_module",    "finit_module",   "delete_module"};

    const bool rules_added =
        addErrnoRules(filter, network_syscalls.data(), network_syscalls.size(), error) &&
        addErrnoRules(filter, filesystem_syscalls.data(), filesystem_syscalls.size(), error) &&
        addErrnoRules(filter, process_syscalls.data(), process_syscalls.size(), error) &&
        addErrnoRules(
            filter,
            kernel_attack_surface_syscalls.data(),
            kernel_attack_surface_syscalls.size(),
            error) &&
        restrictPrctl(filter, error);
    if (!rules_added) {
        seccomp_release(filter);
        return false;
    }

    const int load_result = seccomp_load(filter);
    if (load_result < 0) {
        const int native_error = -load_result;
        setError(
            error,
            SandboxStage::seccomp_filter,
            native_error,
            std::string("seccomp_load failed: ") + std::strerror(native_error));
        seccomp_release(filter);
        return false;
    }
    seccomp_release(filter);
    return true;
}

#endif

}  // namespace

bool installParserSandbox(const SandboxLimits& limits, SandboxError* error) {
    if (error != nullptr) {
        *error = {};
    }
#if defined(__linux__)
    return installResourceLimits(limits, error) && hardenProcess(error) &&
           installSeccompFilter(error);
#else
    (void)limits;
    setError(
        error,
        SandboxStage::unsupported_platform,
        0,
        "The restricted parser worker currently requires Linux and libseccomp");
    return false;
#endif
}

}  // namespace docxstudio::worker
