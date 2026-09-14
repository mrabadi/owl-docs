#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "docxstudio/codex/transport.hpp"
#include "docxstudio/codex/types.hpp"

namespace docxstudio::codex {

struct ClientInfo {
    std::string name = "owl_docs";
    std::string title = "Owl Docs";
    std::string version = "0.3.0";
};

struct InitializeOptions {
    ClientInfo client;
    bool experimentalApi = false;
    Json extensions;
};

struct ModelListOptions {
    std::optional<std::string> cursor;
    std::optional<std::uint32_t> limit;
    std::optional<bool> includeHidden;
};

struct ThreadStartOptions {
    std::optional<std::string> model;
    std::optional<std::string> workingDirectory;
    std::optional<std::string> approvalPolicy;
    std::optional<std::string> sandbox;
    std::optional<std::string> serviceTier;
    std::optional<std::string> developerInstructions;
    std::optional<bool> ephemeral;
    // App-hosted functions exposed to the model for this thread. Calls arrive
    // as item/tool/call server requests and must be answered by the client.
    std::vector<Json> dynamicTools;
    // Forward-compatible app-server configuration overrides. The caller is
    // responsible for ensuring these are allowed by workspace policy.
    Json config;
};

struct ThreadResumeOptions {
    std::string threadId;
    std::optional<std::string> model;
    std::optional<std::string> workingDirectory;
    std::optional<std::string> approvalPolicy;
    std::optional<std::string> sandbox;
    std::optional<std::string> serviceTier;
    std::optional<std::string> developerInstructions;
    bool excludeTurns{true};
    // Forward-compatible app-server configuration overrides. Keep these in
    // step with the restrictions used when the thread was first created.
    Json config;
};

struct UserInput {
    enum class Kind { Text, ImageUrl, LocalImage };

    Kind kind = Kind::Text;
    std::string value;
    std::optional<std::string> detail;

    static UserInput text(std::string text);
    static UserInput imageUrl(std::string url,
                              std::optional<std::string> detail = std::nullopt);
    // Local paths are appropriate only for an explicit user attachment. The
    // editor tool bridge uses opaque file capabilities instead.
    static UserInput localImage(
        std::string path,
        std::optional<std::string> detail = std::nullopt);

    [[nodiscard]] Json toJson() const;
};

struct TurnStartOptions {
    std::string threadId;
    std::vector<UserInput> input;
    std::optional<std::string> model;
    std::optional<std::string> effort;
    // Sticky override for this and subsequent turns.
    std::optional<std::string> serviceTier;
    // One-turn speed override. Use "default" for standard speed.
    std::optional<std::string> serviceTierForTurn;
    std::optional<std::string> clientUserMessageId;
};

class Client {
public:
    using RawCallback = std::function<void(Result<Json>)>;
    using InitializeCallback = std::function<void(Result<InitializeResult>)>;
    using AccountCallback = std::function<void(Result<AccountState>)>;
    using ModelListCallback = std::function<void(Result<ModelPage>)>;
    using ThreadCallback = std::function<void(Result<ThreadState>)>;
    using TurnCallback = std::function<void(Result<TurnState>)>;
    using EventHandler = std::function<void(const ServerEvent&)>;
    using ProtocolErrorHandler = std::function<void(std::string_view)>;
    using ExitHandler = std::function<void(int)>;

    explicit Client(std::shared_ptr<MessageTransport> transport);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool start(const ProcessSpec& spec, std::string& error);
    void shutdown() noexcept;

    void setEventHandler(EventHandler handler);
    void setProtocolErrorHandler(ProtocolErrorHandler handler);
    void setExitHandler(ExitHandler handler);

    [[nodiscard]] bool initialized() const noexcept;

    RequestId initialize(const InitializeOptions& options,
                         InitializeCallback callback = {});
    RequestId readAccount(bool refreshToken,
                          AccountCallback callback = {});
    RequestId listModels(const ModelListOptions& options = {},
                         ModelListCallback callback = {});
    RequestId startThread(const ThreadStartOptions& options,
                          ThreadCallback callback = {});
    RequestId resumeThread(const ThreadResumeOptions& options,
                           ThreadCallback callback = {});
    RequestId startTurn(const TurnStartOptions& options,
                        TurnCallback callback = {});

    RequestId sendRequest(std::string method,
                          Json params,
                          RawCallback callback = {});
    bool sendNotification(std::string method,
                          Json params,
                          std::string& error);
    bool respond(const Json& requestId, Json result, std::string& error);
    bool respondError(const Json& requestId,
                      const RpcError& rpcError,
                      std::string& error);

private:
    struct PendingRequest {
        std::string method;
        RawCallback callback;
    };

    void receive(const Json& message);
    void transportFailed(std::string_view message);
    void transportExited(int exitCode);
    void reportProtocolError(std::string message) const;
    void failAllPending(RpcError error) noexcept;

    std::shared_ptr<MessageTransport> transport_;
    std::atomic<RequestId> nextRequestId_{1};
    std::atomic<bool> initialized_{false};

    mutable std::mutex mutex_;
    std::unordered_map<RequestId, PendingRequest> pending_;
    EventHandler eventHandler_;
    ProtocolErrorHandler protocolErrorHandler_;
    ExitHandler exitHandler_;
    bool shutDown_ = false;
};

}  // namespace docxstudio::codex
