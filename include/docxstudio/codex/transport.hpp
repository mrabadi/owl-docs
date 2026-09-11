#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace docxstudio::codex {

using Json = nlohmann::json;

// A deliberately small process seam. The desktop layer can implement this with
// QProcess without pulling Qt types into the protocol or test targets.
struct ProcessSpec {
    std::string program = "codex";
    std::vector<std::string> arguments = {"app-server", "--stdio"};
    std::string workingDirectory;
};

struct ProcessCallbacks {
    std::function<void(std::string_view)> standardOutput;
    std::function<void(std::string_view)> standardError;
    std::function<void(int exitCode)> exited;
};

class Process {
public:
    virtual ~Process() = default;

    virtual bool start(const ProcessSpec& spec,
                       ProcessCallbacks callbacks,
                       std::string& error) = 0;
    virtual bool writeStandardInput(std::string_view bytes,
                                    std::string& error) = 0;
    virtual void stop() noexcept = 0;
};

class MessageTransport {
public:
    using MessageHandler = std::function<void(const Json&)>;
    using ErrorHandler = std::function<void(std::string_view)>;
    using ExitHandler = std::function<void(int)>;

    virtual ~MessageTransport() = default;

    virtual void setHandlers(MessageHandler message,
                             ErrorHandler error,
                             ExitHandler exited) = 0;
    virtual bool start(const ProcessSpec& spec, std::string& error) = 0;
    virtual bool send(const Json& message, std::string& error) = 0;
    virtual void stop() noexcept = 0;
};

// Frames the app-server's newline-delimited JSON protocol over a Process.
// Handlers run on the thread used by the Process implementation. A Qt adapter
// should therefore keep QProcess on the UI thread or marshal callbacks itself.
class StdioJsonlTransport final : public MessageTransport {
public:
    static constexpr std::size_t kDefaultMaxMessageBytes = 8U * 1024U * 1024U;

    explicit StdioJsonlTransport(
        std::unique_ptr<Process> process,
        std::size_t maxMessageBytes = kDefaultMaxMessageBytes);
    ~StdioJsonlTransport() override;

    StdioJsonlTransport(const StdioJsonlTransport&) = delete;
    StdioJsonlTransport& operator=(const StdioJsonlTransport&) = delete;

    void setHandlers(MessageHandler message,
                     ErrorHandler error,
                     ExitHandler exited) override;
    bool start(const ProcessSpec& spec, std::string& error) override;
    bool send(const Json& message, std::string& error) override;
    void stop() noexcept override;

private:
    void consumeStandardOutput(std::string_view bytes);
    void consumeStandardError(std::string_view bytes);
    void processExited(int exitCode);
    void reportError(std::string message) const;

    std::unique_ptr<Process> process_;
    const std::size_t maxMessageBytes_;

    mutable std::mutex mutex_;
    std::string inputBuffer_;
    MessageHandler messageHandler_;
    ErrorHandler errorHandler_;
    ExitHandler exitHandler_;
    bool started_ = false;
};

}  // namespace docxstudio::codex
