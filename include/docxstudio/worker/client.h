#pragma once

#include "docxstudio/worker/protocol.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace docxstudio::worker {

enum class ParserClientStatus {
    ok,
    invalid_argument,
    input_open_failed,
    spawn_failed,
    timed_out,
    io_error,
    protocol_error,
    worker_exited,
    worker_signaled,
};

struct ParserWorkerExit {
    bool available{false};
    bool exited{false};
    int exit_code{-1};
    bool signaled{false};
    int signal_number{0};
};

struct ParserClientOptions {
    // Must name the trusted parser helper by an absolute path. The document
    // path is never placed in argv or the environment.
    std::filesystem::path helper_executable;
    std::chrono::milliseconds timeout{15000};
    std::uint64_t max_response_bytes{128ULL * 1024ULL * 1024ULL};
};

struct ParserClientResult {
    ParserClientStatus status{ParserClientStatus::invalid_argument};
    ParserResponse response;
    bool response_received{false};
    ParserWorkerExit worker_exit;
    int native_error{0};
    std::string error;

    // A successful transport can still carry a ParserResponse whose status
    // reports an invalid or unsupported document.
    [[nodiscard]] bool transportOk() const noexcept {
        return status == ParserClientStatus::ok && response_received;
    }
};

// Opens the path only in the parent, then delegates through an inherited
// O_RDONLY descriptor. Symlinks are resolved with the parent's credentials.
[[nodiscard]] ParserClientResult parseDocxFileWithWorker(
    const std::filesystem::path& document_path,
    const ParserClientOptions& options);

// The caller retains ownership of input_fd. The client duplicates it and the
// worker uses pread(), so the caller's file offset is unchanged. Only an
// O_RDONLY descriptor for a regular file or memfd is accepted.
[[nodiscard]] ParserClientResult parseDocxDescriptorWithWorker(
    int input_fd,
    const ParserClientOptions& options);

}  // namespace docxstudio::worker
