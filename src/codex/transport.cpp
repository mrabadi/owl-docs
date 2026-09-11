#include "docxstudio/codex/transport.hpp"

#include <exception>
#include <utility>

#include "docxstudio/codex/redaction.hpp"

namespace docxstudio::codex {

StdioJsonlTransport::StdioJsonlTransport(
    std::unique_ptr<Process> process,
    const std::size_t maxMessageBytes)
    : process_(std::move(process)),
      maxMessageBytes_(maxMessageBytes == 0 ? kDefaultMaxMessageBytes
                                            : maxMessageBytes) {}

StdioJsonlTransport::~StdioJsonlTransport() { stop(); }

void StdioJsonlTransport::setHandlers(MessageHandler message,
                                      ErrorHandler error,
                                      ExitHandler exited) {
    std::lock_guard<std::mutex> lock(mutex_);
    messageHandler_ = std::move(message);
    errorHandler_ = std::move(error);
    exitHandler_ = std::move(exited);
}

bool StdioJsonlTransport::start(const ProcessSpec& spec, std::string& error) {
    if (!process_) {
        error = "No process implementation was supplied";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            error = "Codex app-server transport is already running";
            return false;
        }
        inputBuffer_.clear();
    }

    ProcessCallbacks callbacks;
    callbacks.standardOutput =
        [this](const std::string_view bytes) { consumeStandardOutput(bytes); };
    callbacks.standardError =
        [this](const std::string_view bytes) { consumeStandardError(bytes); };
    callbacks.exited = [this](const int code) { processExited(code); };

    if (!process_->start(spec, std::move(callbacks), error)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
    }
    return true;
}

bool StdioJsonlTransport::send(const Json& message, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            error = "Codex app-server transport is not running";
            return false;
        }
    }

    std::string frame;
    try {
        frame = message.dump();
    } catch (const std::exception&) {
        error = "Could not serialize a Codex app-server message";
        return false;
    }

    if (frame.size() > maxMessageBytes_) {
        error = "Codex app-server message exceeds the configured size limit";
        return false;
    }
    frame.push_back('\n');
    return process_->writeStandardInput(frame, error);
}

void StdioJsonlTransport::stop() noexcept {
    bool shouldStop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldStop = started_;
        started_ = false;
        inputBuffer_.clear();
    }
    if (shouldStop && process_) {
        process_->stop();
    }
}

void StdioJsonlTransport::consumeStandardOutput(const std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    std::vector<std::string> frames;
    bool overflow = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        inputBuffer_.append(bytes.data(), bytes.size());

        std::size_t newline = 0;
        while ((newline = inputBuffer_.find('\n')) != std::string::npos) {
            if (newline > maxMessageBytes_) {
                overflow = true;
            } else {
                std::string frame = inputBuffer_.substr(0, newline);
                if (!frame.empty() && frame.back() == '\r') {
                    frame.pop_back();
                }
                if (!frame.empty()) {
                    frames.push_back(std::move(frame));
                }
            }
            inputBuffer_.erase(0, newline + 1);
        }

        if (inputBuffer_.size() > maxMessageBytes_) {
            inputBuffer_.clear();
            overflow = true;
        }
    }

    if (overflow) {
        reportError(
            "Codex app-server emitted a JSONL message larger than the configured limit");
    }

    for (const std::string& frame : frames) {
        const Json parsed = Json::parse(frame, nullptr, false);
        if (parsed.is_discarded()) {
            reportError("Codex app-server emitted malformed JSONL");
            continue;
        }

        MessageHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = messageHandler_;
        }
        if (handler) {
            try {
                handler(parsed);
            } catch (...) {
                reportError(
                    "Codex app-server message handler threw an exception");
            }
        }
    }
}

void StdioJsonlTransport::consumeStandardError(const std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    reportError("Codex app-server stderr: " + redactForLog(bytes));
}

void StdioJsonlTransport::processExited(const int exitCode) {
    ExitHandler handler;
    bool hadPartialFrame = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        hadPartialFrame = !inputBuffer_.empty();
        inputBuffer_.clear();
        handler = exitHandler_;
    }
    if (hadPartialFrame) {
        reportError("Codex app-server exited with an incomplete JSONL message");
    }
    if (handler) {
        try {
            handler(exitCode);
        } catch (...) {
            reportError("Codex app-server exit handler threw an exception");
        }
    }
}

void StdioJsonlTransport::reportError(std::string message) const {
    ErrorHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = errorHandler_;
    }
    if (handler) {
        try {
            handler(message);
        } catch (...) {
            // Error reporting must never unwind through the process adapter.
        }
    }
}

}  // namespace docxstudio::codex
