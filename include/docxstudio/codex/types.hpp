#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace docxstudio::codex {

using Json = nlohmann::json;
using RequestId = std::int64_t;

struct RpcError {
    int code = 0;
    std::string message;
    Json data;
};

template <typename T>
struct Result {
    std::optional<T> value;
    std::optional<RpcError> error;

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !error.has_value();
    }

    explicit operator bool() const noexcept { return ok(); }

    static Result success(T result) {
        Result outcome;
        outcome.value.emplace(std::move(result));
        return outcome;
    }

    static Result failure(RpcError rpcError) {
        Result outcome;
        outcome.error.emplace(std::move(rpcError));
        return outcome;
    }
};

struct InitializeResult {
    std::string userAgent;
    std::string platformFamily;
    std::string platformOs;
    Json raw;
};

struct AccountState {
    bool requiresOpenaiAuth = true;
    bool signedIn = false;
    std::string accountType;
    std::optional<std::string> email;
    std::optional<std::string> planType;
    Json raw;
};

// Values are intentionally strings rather than enums. The server catalog is
// authoritative and can add effort levels or service tiers independently of
// an editor release.
struct ReasoningEffortOption {
    std::string id;
    std::string description;
};

struct ServiceTierOption {
    std::string id;
    std::string name;
    std::string description;
};

struct ModelInfo {
    std::string id;
    std::string model;
    std::string displayName;
    std::string description;
    bool hidden = false;
    bool isDefault = false;
    bool supportsPersonality = false;
    std::string defaultReasoningEffort;
    std::vector<ReasoningEffortOption> reasoningEfforts;
    std::optional<std::string> defaultServiceTier;
    std::vector<ServiceTierOption> serviceTiers;
    std::vector<std::string> inputModalities;
    Json raw;

    [[nodiscard]] const std::string& wireModel() const noexcept {
        return model.empty() ? id : model;
    }
};

struct ModelPage {
    std::vector<ModelInfo> models;
    std::optional<std::string> nextCursor;
    Json raw;
};

struct ThreadState {
    std::string id;
    std::string model;
    std::optional<std::string> reasoningEffort;
    std::optional<std::string> serviceTier;
    Json raw;
};

struct TurnState {
    std::string id;
    std::string status;
    Json raw;
};

enum class EventKind {
    Unknown,
    ThreadStarted,
    ThreadStatusChanged,
    TurnStarted,
    TurnCompleted,
    ItemStarted,
    ItemCompleted,
    AgentMessageDelta,
    PlanDelta,
    ReasoningSummaryDelta,
    ReasoningDelta,
    CommandOutputDelta,
    FileChangeDelta,
    ToolProgress,
    AccountUpdated,
    RateLimitsUpdated,
    Warning,
    Error,
    ServerRequest,
};

struct ServerEvent {
    EventKind kind = EventKind::Unknown;
    std::string method;
    Json params;
    bool isRequest = false;
    std::optional<Json> requestId;
    std::string threadId;
    std::string turnId;
    std::string itemId;
    std::string delta;
};

InitializeResult parseInitializeResult(const Json& result);
AccountState parseAccountState(const Json& result);
ModelPage parseModelPage(const Json& result);
ThreadState parseThreadState(const Json& result);
TurnState parseTurnState(const Json& result);
ServerEvent parseServerEvent(std::string method,
                             Json params,
                             std::optional<Json> requestId = std::nullopt);

}  // namespace docxstudio::codex
