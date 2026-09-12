#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "docxstudio/worker/client.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace docxstudio::worker {
namespace {

constexpr int kChildInputDescriptor = 3;
constexpr int kChildOutputDescriptor = 4;
constexpr int kRelocatedDescriptorMinimum = 64;
constexpr auto kMaximumTimeout = std::chrono::hours(24);

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizerBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizerBuild = true;
#else
constexpr bool kAddressSanitizerBuild = false;
#endif
#else
constexpr bool kAddressSanitizerBuild = false;
#endif

class UniqueDescriptor {
public:
    UniqueDescriptor() = default;
    explicit UniqueDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    ~UniqueDescriptor() { reset(); }

    UniqueDescriptor(const UniqueDescriptor&) = delete;
    UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;

    UniqueDescriptor(UniqueDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.descriptor_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    void reset(int replacement = -1) noexcept {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
        descriptor_ = replacement;
    }

private:
    int descriptor_{-1};
};

class SpawnFileActions {
public:
    SpawnFileActions() : error_(::posix_spawn_file_actions_init(&actions_)) {}
    ~SpawnFileActions() {
        if (error_ == 0) {
            (void)::posix_spawn_file_actions_destroy(&actions_);
        }
    }

    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;

    [[nodiscard]] int error() const noexcept { return error_; }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept { return &actions_; }

private:
    posix_spawn_file_actions_t actions_{};
    int error_{0};
};

class SpawnAttributes {
public:
    SpawnAttributes() : error_(::posix_spawnattr_init(&attributes_)) {}
    ~SpawnAttributes() {
        if (error_ == 0) {
            (void)::posix_spawnattr_destroy(&attributes_);
        }
    }

    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;

    [[nodiscard]] int error() const noexcept { return error_; }
    [[nodiscard]] posix_spawnattr_t* get() noexcept { return &attributes_; }

private:
    posix_spawnattr_t attributes_{};
    int error_{0};
};

class SpawnedChild {
public:
    explicit SpawnedChild(pid_t process_id) noexcept : process_id_(process_id) {}
    ~SpawnedChild() {
        const int saved_error = errno;
        if (!reaped_) {
            int ignored = 0;
            (void)terminateAndReap(ignored);
        }
        errno = saved_error;
    }

    SpawnedChild(const SpawnedChild&) = delete;
    SpawnedChild& operator=(const SpawnedChild&) = delete;

    // Returns one when reaped, zero while running, and minus one on error.
    int reapWithoutBlocking(int& native_error) noexcept {
        if (reaped_) {
            return 1;
        }
        int status = 0;
        pid_t result = -1;
        do {
            result = ::waitpid(process_id_, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == process_id_) {
            wait_status_ = status;
            reaped_ = true;
            return 1;
        }
        if (result == 0) {
            return 0;
        }
        native_error = errno;
        return -1;
    }

    bool terminateAndReap(int& native_error) noexcept {
        if (reaped_) {
            return true;
        }

        // The child is placed in its own process group. Killing the group also
        // contains a compromised pre-sandbox helper that managed to fork.
        if (::kill(-process_id_, SIGKILL) != 0) {
            if (::kill(process_id_, SIGKILL) != 0 && errno != ESRCH) {
                native_error = errno;
            }
        }

        int status = 0;
        pid_t result = -1;
        do {
            result = ::waitpid(process_id_, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result != process_id_) {
            if (native_error == 0) {
                native_error = errno;
            }
            return false;
        }
        wait_status_ = status;
        reaped_ = true;
        return true;
    }

    [[nodiscard]] bool reaped() const noexcept { return reaped_; }
    [[nodiscard]] int waitStatus() const noexcept { return wait_status_; }

private:
    pid_t process_id_{-1};
    bool reaped_{false};
    int wait_status_{0};
};

std::string nativeErrorMessage(std::string_view prefix, int native_error) {
    return std::string(prefix) + ": " + std::strerror(native_error);
}

ParserClientResult invalidArgument(std::string message, int native_error = EINVAL) {
    ParserClientResult result;
    result.status = ParserClientStatus::invalid_argument;
    result.native_error = native_error;
    result.error = std::move(message);
    return result;
}

bool pathHasEmbeddedNull(const std::filesystem::path& path) {
    const std::string& native = path.native();
    return native.find('\0') != std::string::npos;
}

bool validateOptions(const ParserClientOptions& options, ParserClientResult& failure) {
    if (options.helper_executable.empty() || !options.helper_executable.is_absolute() ||
        pathHasEmbeddedNull(options.helper_executable)) {
        failure = invalidArgument("Parser helper must be specified by a non-empty absolute path");
        return false;
    }
    if (options.timeout <= std::chrono::milliseconds::zero() ||
        options.timeout > kMaximumTimeout) {
        failure = invalidArgument("Parser timeout must be greater than zero and at most 24 hours");
        return false;
    }
    const ParserLimits worker_limits;
    if (options.max_response_bytes < kParserResponseHeaderSize ||
        options.max_response_bytes > worker_limits.max_response_bytes ||
        options.max_response_bytes > std::numeric_limits<std::size_t>::max()) {
        failure = invalidArgument(
            "Parser response limit must fit the protocol header and not exceed the worker limit");
        return false;
    }
    return true;
}

bool validateInputDescriptor(int descriptor, ParserClientResult& failure) {
    if (descriptor < 0) {
        failure = invalidArgument("Parser input descriptor is invalid", EBADF);
        return false;
    }
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        const int native_error = errno;
        failure = invalidArgument(
            nativeErrorMessage("Cannot inspect parser input descriptor", native_error),
            native_error);
        return false;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        failure = invalidArgument("Parser input descriptor must have O_RDONLY access");
        return false;
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const int native_error = errno;
        failure = invalidArgument(
            nativeErrorMessage("Cannot stat parser input descriptor", native_error),
            native_error);
        return false;
    }
    if (!S_ISREG(status.st_mode)) {
        failure = invalidArgument("Parser input descriptor must refer to a regular file or memfd");
        return false;
    }
    return true;
}

int addSpawnFileActions(
    posix_spawn_file_actions_t* actions,
    int relocated_input,
    int relocated_output) {
    int result = ::posix_spawn_file_actions_adddup2(
        actions, relocated_input, kChildInputDescriptor);
    if (result == 0) {
        result = ::posix_spawn_file_actions_adddup2(
            actions, relocated_output, kChildOutputDescriptor);
    }
    if (result == 0) {
        // GNU libc 2.34+ performs this close in the child before exec. The
        // two protocol descriptors are below the cutoff and every relocated
        // source plus unrelated inherited descriptor is removed.
        result = ::posix_spawn_file_actions_addclosefrom_np(
            actions, kChildOutputDescriptor + 1);
    }
    return result;
}

int configureSpawnAttributes(posix_spawnattr_t* attributes) {
    sigset_t empty_mask;
    sigset_t default_signals;
    if (::sigemptyset(&empty_mask) != 0 || ::sigemptyset(&default_signals) != 0) {
        return errno;
    }
    constexpr std::array signals_to_reset{
        SIGPIPE, SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGXCPU, SIGXFSZ};
    for (const int signal_number : signals_to_reset) {
        if (::sigaddset(&default_signals, signal_number) != 0) {
            return errno;
        }
    }

    int result = ::posix_spawnattr_setsigmask(attributes, &empty_mask);
    if (result == 0) {
        result = ::posix_spawnattr_setsigdefault(attributes, &default_signals);
    }
    if (result == 0) {
        result = ::posix_spawnattr_setpgroup(attributes, 0);
    }
    if (result == 0) {
        constexpr short flags = static_cast<short>(
            POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
        result = ::posix_spawnattr_setflags(attributes, flags);
    }
    return result;
}

void recordExit(ParserClientResult& result, int wait_status) {
    result.worker_exit.available = true;
    if (WIFEXITED(wait_status)) {
        result.worker_exit.exited = true;
        result.worker_exit.exit_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        result.worker_exit.signaled = true;
        result.worker_exit.signal_number = WTERMSIG(wait_status);
    }
}

void stopAndRecord(SpawnedChild& child, ParserClientResult& result) {
    int native_error = 0;
    if (!child.terminateAndReap(native_error) && result.native_error == 0) {
        result.native_error = native_error;
    }
    if (child.reaped()) {
        recordExit(result, child.waitStatus());
    }
}

int deadlinePollMilliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = deadline - now;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) {
        milliseconds += std::chrono::milliseconds(1);
    }
    return static_cast<int>(std::min<std::int64_t>(milliseconds.count(), INT_MAX));
}

enum class ReceiveStatus { end_of_file, timed_out, io_error, too_large };

ReceiveStatus receiveFrame(
    int descriptor,
    std::uint64_t maximum_frame_bytes,
    std::chrono::steady_clock::time_point deadline,
    std::vector<std::uint8_t>& frame,
    int& native_error) {
    constexpr std::size_t chunk_size = 16 * 1024;
    std::array<std::uint8_t, chunk_size> chunk{};
    frame.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(maximum_frame_bytes, 64ULL * 1024ULL)));

    while (true) {
        const int timeout = deadlinePollMilliseconds(deadline);
        if (timeout == 0) {
            return ReceiveStatus::timed_out;
        }
        struct pollfd poll_descriptor {descriptor, static_cast<short>(POLLIN | POLLHUP), 0};
        int poll_result = -1;
        do {
            poll_result = ::poll(&poll_descriptor, 1, timeout);
        } while (poll_result < 0 && errno == EINTR &&
                 std::chrono::steady_clock::now() < deadline);
        if (poll_result == 0) {
            return ReceiveStatus::timed_out;
        }
        if (poll_result < 0) {
            native_error = errno;
            return ReceiveStatus::io_error;
        }
        if ((poll_descriptor.revents & POLLNVAL) != 0) {
            native_error = EBADF;
            return ReceiveStatus::io_error;
        }

        while (true) {
            const ssize_t count = ::read(descriptor, chunk.data(), chunk.size());
            if (count > 0) {
                const auto byte_count = static_cast<std::size_t>(count);
                if (frame.size() > maximum_frame_bytes ||
                    byte_count > maximum_frame_bytes - frame.size()) {
                    return ReceiveStatus::too_large;
                }
                frame.insert(
                    frame.end(),
                    chunk.begin(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(byte_count));
                continue;
            }
            if (count == 0) {
                return ReceiveStatus::end_of_file;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            native_error = errno;
            return ReceiveStatus::io_error;
        }

        if ((poll_descriptor.revents & POLLERR) != 0) {
            native_error = EIO;
            return ReceiveStatus::io_error;
        }
    }
}

enum class WaitStatus { reaped, timed_out, failed };

WaitStatus waitForChild(
    SpawnedChild& child,
    std::chrono::steady_clock::time_point deadline,
    int& native_error) {
    while (true) {
        const int state = child.reapWithoutBlocking(native_error);
        if (state > 0) {
            return WaitStatus::reaped;
        }
        if (state < 0) {
            return WaitStatus::failed;
        }
        const int remaining = deadlinePollMilliseconds(deadline);
        if (remaining == 0) {
            return WaitStatus::timed_out;
        }
        const int pause = std::min(remaining, 10);
        int poll_result = -1;
        do {
            poll_result = ::poll(nullptr, 0, pause);
        } while (poll_result < 0 && errno == EINTR &&
                 std::chrono::steady_clock::now() < deadline);
        if (poll_result < 0 && errno != EINTR) {
            native_error = errno;
            return WaitStatus::failed;
        }
    }
}

void classifyWorkerExit(ParserClientResult& result) {
    if (result.worker_exit.signaled) {
        result.status = ParserClientStatus::worker_signaled;
        result.error = "Parser worker terminated by signal " +
                       std::to_string(result.worker_exit.signal_number);
    } else if (result.worker_exit.exited && result.worker_exit.exit_code != 0) {
        result.status = ParserClientStatus::worker_exited;
        result.error =
            "Parser worker exited with status " + std::to_string(result.worker_exit.exit_code);
    } else {
        result.status = ParserClientStatus::ok;
    }
}

ParserClientResult parseValidatedDescriptor(
    int input_fd,
    const ParserClientOptions& options) {
    ParserClientResult result;

    UniqueDescriptor relocated_input(
        ::fcntl(input_fd, F_DUPFD_CLOEXEC, kRelocatedDescriptorMinimum));
    if (relocated_input.get() < 0) {
        result.status = ParserClientStatus::io_error;
        result.native_error = errno;
        result.error = nativeErrorMessage("Cannot duplicate parser input descriptor", errno);
        return result;
    }

    int response_descriptors[2]{-1, -1};
    if (::pipe2(response_descriptors, O_CLOEXEC) != 0) {
        result.status = ParserClientStatus::io_error;
        result.native_error = errno;
        result.error = nativeErrorMessage("Cannot create parser response pipe", errno);
        return result;
    }
    UniqueDescriptor response_reader(response_descriptors[0]);
    UniqueDescriptor response_writer(response_descriptors[1]);
    UniqueDescriptor relocated_output(
        ::fcntl(response_writer.get(), F_DUPFD_CLOEXEC, kRelocatedDescriptorMinimum));
    if (relocated_output.get() < 0) {
        result.status = ParserClientStatus::io_error;
        result.native_error = errno;
        result.error = nativeErrorMessage("Cannot relocate parser output descriptor", errno);
        return result;
    }
    response_writer.reset();

    SpawnFileActions actions;
    SpawnAttributes attributes;
    int spawn_setup_error = actions.error();
    if (spawn_setup_error == 0) {
        spawn_setup_error = attributes.error();
    }
    if (spawn_setup_error == 0) {
        spawn_setup_error = addSpawnFileActions(
            actions.get(), relocated_input.get(), relocated_output.get());
    }
    if (spawn_setup_error == 0) {
        spawn_setup_error = configureSpawnAttributes(attributes.get());
    }
    if (spawn_setup_error != 0) {
        result.status = ParserClientStatus::spawn_failed;
        result.native_error = spawn_setup_error;
        result.error = nativeErrorMessage("Cannot configure parser worker spawn", spawn_setup_error);
        return result;
    }

    std::string helper_path = options.helper_executable.native();
    std::string input_argument = "--input-fd=" + std::to_string(kChildInputDescriptor);
    std::string output_argument = "--output-fd=" + std::to_string(kChildOutputDescriptor);
    std::array<char*, 4> arguments{
        helper_path.data(), input_argument.data(), output_argument.data(), nullptr};
    std::array<char*, 1> empty_environment{nullptr};
    // LeakSanitizer cannot inspect a child after the worker deliberately sets
    // PR_SET_DUMPABLE to zero. Keep the production environment empty, while
    // giving instrumented test helpers one controlled option that disables
    // only the incompatible exit-time leak pass. ASan and UBSan remain active
    // while the parser handles the document inside its normal seccomp policy.
    char sanitizer_option[] = "ASAN_OPTIONS=detect_leaks=0";
    std::array<char*, 2> sanitizer_environment{sanitizer_option, nullptr};
    char* const* child_environment = kAddressSanitizerBuild
        ? sanitizer_environment.data()
        : empty_environment.data();
    pid_t process_id = -1;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    const int spawn_error = ::posix_spawn(
        &process_id,
        helper_path.c_str(),
        actions.get(),
        attributes.get(),
        arguments.data(),
        child_environment);
    if (spawn_error != 0) {
        result.status = ParserClientStatus::spawn_failed;
        result.native_error = spawn_error;
        result.error = nativeErrorMessage("Cannot spawn parser worker", spawn_error);
        return result;
    }

    SpawnedChild child(process_id);
    relocated_input.reset();
    relocated_output.reset();

    const int reader_flags = ::fcntl(response_reader.get(), F_GETFL);
    if (reader_flags < 0 || ::fcntl(response_reader.get(), F_SETFL, reader_flags | O_NONBLOCK) != 0) {
        result.status = ParserClientStatus::io_error;
        result.native_error = errno;
        result.error = nativeErrorMessage("Cannot configure parser response pipe", errno);
        stopAndRecord(child, result);
        return result;
    }

    std::vector<std::uint8_t> frame;
    int receive_error = 0;
    const ReceiveStatus receive_status = receiveFrame(
        response_reader.get(), options.max_response_bytes, deadline, frame, receive_error);
    response_reader.reset();
    if (receive_status == ReceiveStatus::timed_out) {
        result.status = ParserClientStatus::timed_out;
        result.error = "Parser worker exceeded its monotonic deadline";
        stopAndRecord(child, result);
        return result;
    }
    if (receive_status == ReceiveStatus::io_error) {
        result.status = ParserClientStatus::io_error;
        result.native_error = receive_error;
        result.error = nativeErrorMessage("Cannot read parser worker response", receive_error);
        stopAndRecord(child, result);
        return result;
    }
    if (receive_status == ReceiveStatus::too_large) {
        result.status = ParserClientStatus::protocol_error;
        result.error = "Parser worker response exceeds the configured frame limit";
        stopAndRecord(child, result);
        return result;
    }

    std::string protocol_error;
    if (!decodeParserResponseFrame(
            frame, result.response, options.max_response_bytes, &protocol_error)) {
        int wait_error = 0;
        const WaitStatus wait_status = waitForChild(child, deadline, wait_error);
        if (wait_status == WaitStatus::timed_out) {
            result.status = ParserClientStatus::timed_out;
            result.error =
                "Parser worker closed its response but did not exit before its deadline; "
                "response protocol error: " +
                protocol_error;
            stopAndRecord(child, result);
            return result;
        }
        if (wait_status == WaitStatus::failed) {
            result.status = ParserClientStatus::io_error;
            result.native_error = wait_error;
            result.error = nativeErrorMessage("Cannot reap parser worker", wait_error) +
                           "; response protocol error: " + protocol_error;
            stopAndRecord(child, result);
            return result;
        }

        recordExit(result, child.waitStatus());
        if (result.worker_exit.signaled) {
            classifyWorkerExit(result);
            result.error += "; response protocol error: " + protocol_error;
        } else if (result.worker_exit.exited &&
                   result.worker_exit.exit_code != 0) {
            classifyWorkerExit(result);
            result.error += "; response protocol error: " + protocol_error;
        } else {
            result.status = ParserClientStatus::protocol_error;
            result.error = std::move(protocol_error);
        }
        return result;
    }
    result.response_received = true;

    int wait_error = 0;
    const WaitStatus wait_status = waitForChild(child, deadline, wait_error);
    if (wait_status == WaitStatus::timed_out) {
        result.status = ParserClientStatus::timed_out;
        result.error = "Parser worker wrote a response but did not exit before its deadline";
        stopAndRecord(child, result);
        return result;
    }
    if (wait_status == WaitStatus::failed) {
        result.status = ParserClientStatus::io_error;
        result.native_error = wait_error;
        result.error = nativeErrorMessage("Cannot reap parser worker", wait_error);
        stopAndRecord(child, result);
        return result;
    }

    recordExit(result, child.waitStatus());
    classifyWorkerExit(result);
    return result;
}

}  // namespace

ParserClientResult parseDocxFileWithWorker(
    const std::filesystem::path& document_path,
    const ParserClientOptions& options) {
    ParserClientResult failure;
    if (!validateOptions(options, failure)) {
        return failure;
    }
    if (document_path.empty() || pathHasEmbeddedNull(document_path)) {
        return invalidArgument("Document path must be non-empty and contain no NUL byte");
    }

    const int descriptor = ::open(document_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const int native_error = errno;
        ParserClientResult result;
        result.status = ParserClientStatus::input_open_failed;
        result.native_error = native_error;
        result.error = nativeErrorMessage("Cannot open DOCX input in parent", native_error);
        return result;
    }
    UniqueDescriptor owned_descriptor(descriptor);
    if (!validateInputDescriptor(owned_descriptor.get(), failure)) {
        failure.status = ParserClientStatus::input_open_failed;
        return failure;
    }
    return parseValidatedDescriptor(owned_descriptor.get(), options);
}

ParserClientResult parseDocxDescriptorWithWorker(
    int input_fd,
    const ParserClientOptions& options) {
    ParserClientResult failure;
    if (!validateOptions(options, failure) || !validateInputDescriptor(input_fd, failure)) {
        return failure;
    }
    return parseValidatedDescriptor(input_fd, options);
}

}  // namespace docxstudio::worker
