#include "docxstudio/codex/client.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "docxstudio/codex/redaction.hpp"

namespace docxstudio::codex {
namespace {

constexpr int kTransportClosedError = -32097;
constexpr int kTransportWriteError = -32098;
constexpr int kInvalidResponseError = -32603;

std::string optionalStringValue(const Json& object,
                                const char* key,
                                const std::string& fallback = {}) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || iterator->is_null() ||
        !iterator->is_string()) {
        return fallback;
    }
    return iterator->get<std::string>();
}

std::optional<std::string> optionalString(const Json& object,
                                          const char* key) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || iterator->is_null() ||
        !iterator->is_string()) {
        return std::nullopt;
    }
    return iterator->get<std::string>();
}

bool optionalBoolean(const Json& object,
                     const char* key,
                     const bool fallback = false) {
    const auto iterator = object.find(key);
    return iterator != object.end() && iterator->is_boolean()
               ? iterator->get<bool>()
               : fallback;
}

const Json& requiredObject(const Json& parent,
                           const char* key,
                           const char* responseName) {
    if (!parent.is_object()) {
        throw std::invalid_argument(std::string(responseName) +
                                    " response is not an object");
    }
    const auto iterator = parent.find(key);
    if (iterator == parent.end() || !iterator->is_object()) {
        throw std::invalid_argument(std::string(responseName) +
                                    " response has no object field '" + key +
                                    "'");
    }
    return *iterator;
}

std::string requiredString(const Json& parent,
                           const char* key,
                           const char* responseName) {
    const auto iterator = parent.find(key);
    if (iterator == parent.end() || !iterator->is_string()) {
        throw std::invalid_argument(std::string(responseName) +
                                    " response has no string field '" + key +
                                    "'");
    }
    return iterator->get<std::string>();
}

RpcError invalidResponse(const char* method, const std::exception& exception) {
    return {kInvalidResponseError,
            std::string("Invalid ") + method + " response: " + exception.what(),
            Json()};
}

EventKind eventKindForMethod(const std::string& method) {
    if (method == "thread/started") {
        return EventKind::ThreadStarted;
    }
    if (method == "thread/status/changed") {
        return EventKind::ThreadStatusChanged;
    }
    if (method == "turn/started") {
        return EventKind::TurnStarted;
    }
    if (method == "turn/completed") {
        return EventKind::TurnCompleted;
    }
    if (method == "item/started") {
        return EventKind::ItemStarted;
    }
    if (method == "item/completed") {
        return EventKind::ItemCompleted;
    }
    if (method == "item/agentMessage/delta") {
        return EventKind::AgentMessageDelta;
    }
    if (method == "item/plan/delta" || method == "turn/plan/updated") {
        return EventKind::PlanDelta;
    }
    if (method == "item/reasoning/summaryTextDelta" ||
        method == "item/reasoning/summaryPartAdded") {
        return EventKind::ReasoningSummaryDelta;
    }
    if (method == "item/reasoning/textDelta") {
        return EventKind::ReasoningDelta;
    }
    if (method == "command/exec/outputDelta" ||
        method == "process/outputDelta" ||
        method == "item/commandExecution/outputDelta") {
        return EventKind::CommandOutputDelta;
    }
    if (method == "item/fileChange/outputDelta" ||
        method == "item/fileChange/patchUpdated" ||
        method == "turn/diff/updated") {
        return EventKind::FileChangeDelta;
    }
    if (method == "item/mcpToolCall/progress") {
        return EventKind::ToolProgress;
    }
    if (method == "account/updated" ||
        method == "account/login/completed") {
        return EventKind::AccountUpdated;
    }
    if (method == "account/rateLimits/updated") {
        return EventKind::RateLimitsUpdated;
    }
    if (method == "warning" || method == "guardianWarning" ||
        method == "deprecationNotice" || method == "configWarning") {
        return EventKind::Warning;
    }
    if (method == "error" || method == "thread/realtime/error") {
        return EventKind::Error;
    }
    return EventKind::Unknown;
}

template <typename T, typename Parser, typename Callback>
void deliverTyped(Result<Json> raw,
                  const char* method,
                  Parser parser,
                  Callback callback) {
    if (!callback) {
        return;
    }
    if (!raw) {
        callback(Result<T>::failure(std::move(*raw.error)));
        return;
    }
    std::optional<T> parsed;
    try {
        parsed.emplace(parser(*raw.value));
    } catch (const std::exception& exception) {
        callback(Result<T>::failure(invalidResponse(method, exception)));
        return;
    }
    callback(Result<T>::success(std::move(*parsed)));
}

}  // namespace

InitializeResult parseInitializeResult(const Json& result) {
    if (!result.is_object()) {
        throw std::invalid_argument("initialize response is not an object");
    }
    InitializeResult parsed;
    parsed.userAgent = optionalStringValue(result, "userAgent");
    parsed.platformFamily = optionalStringValue(result, "platformFamily");
    parsed.platformOs = optionalStringValue(result, "platformOs");
    parsed.raw = result;
    return parsed;
}

AccountState parseAccountState(const Json& result) {
    if (!result.is_object()) {
        throw std::invalid_argument("account/read response is not an object");
    }

    AccountState parsed;
    parsed.requiresOpenaiAuth =
        optionalBoolean(result, "requiresOpenaiAuth", true);
    const auto account = result.find("account");
    if (account != result.end() && account->is_object()) {
        parsed.signedIn = true;
        parsed.accountType = optionalStringValue(*account, "type");
        parsed.email = optionalString(*account, "email");
        parsed.planType = optionalString(*account, "planType");
    }
    parsed.raw = result;
    return parsed;
}

ModelPage parseModelPage(const Json& result) {
    if (!result.is_object()) {
        throw std::invalid_argument("model/list response is not an object");
    }
    const auto data = result.find("data");
    if (data == result.end() || !data->is_array()) {
        throw std::invalid_argument("model/list response has no array field 'data'");
    }

    ModelPage page;
    page.nextCursor = optionalString(result, "nextCursor");
    page.raw = result;

    for (const Json& item : *data) {
        if (!item.is_object()) {
            continue;
        }
        ModelInfo model;
        model.id = optionalStringValue(item, "id");
        model.model = optionalStringValue(item, "model", model.id);
        if (model.id.empty() && model.model.empty()) {
            continue;
        }
        model.displayName = optionalStringValue(item, "displayName", model.model);
        model.description = optionalStringValue(item, "description");
        model.hidden = optionalBoolean(item, "hidden");
        model.isDefault = optionalBoolean(item, "isDefault");
        model.supportsPersonality =
            optionalBoolean(item, "supportsPersonality");
        model.defaultReasoningEffort =
            optionalStringValue(item, "defaultReasoningEffort");
        model.defaultServiceTier = optionalString(item, "defaultServiceTier");

        const auto efforts = item.find("supportedReasoningEfforts");
        if (efforts != item.end() && efforts->is_array()) {
            for (const Json& effort : *efforts) {
                if (effort.is_string()) {
                    model.reasoningEfforts.push_back(
                        {effort.get<std::string>(), {}});
                } else if (effort.is_object()) {
                    const std::string id =
                        optionalStringValue(effort, "reasoningEffort");
                    if (!id.empty()) {
                        model.reasoningEfforts.push_back(
                            {id, optionalStringValue(effort, "description")});
                    }
                }
            }
        }

        const auto tiers = item.find("serviceTiers");
        if (tiers != item.end() && tiers->is_array()) {
            for (const Json& tier : *tiers) {
                if (tier.is_string()) {
                    const std::string id = tier.get<std::string>();
                    model.serviceTiers.push_back({id, id, {}});
                } else if (tier.is_object()) {
                    const std::string id = optionalStringValue(tier, "id");
                    if (!id.empty()) {
                        model.serviceTiers.push_back(
                            {id, optionalStringValue(tier, "name", id),
                             optionalStringValue(tier, "description")});
                    }
                }
            }
        }
        if (model.serviceTiers.empty()) {
            // Compatibility with older catalogs. New clients prefer
            // serviceTiers, but retaining these values keeps the picker useful.
            const auto legacyTiers = item.find("additionalSpeedTiers");
            if (legacyTiers != item.end() && legacyTiers->is_array()) {
                for (const Json& tier : *legacyTiers) {
                    if (tier.is_string()) {
                        const std::string id = tier.get<std::string>();
                        model.serviceTiers.push_back({id, id, {}});
                    }
                }
            }
        }

        const auto modalities = item.find("inputModalities");
        if (modalities != item.end() && modalities->is_array()) {
            for (const Json& modality : *modalities) {
                if (modality.is_string()) {
                    model.inputModalities.push_back(modality.get<std::string>());
                }
            }
        }
        model.raw = item;
        page.models.push_back(std::move(model));
    }
    return page;
}

ThreadState parseThreadState(const Json& result) {
    const Json& thread = requiredObject(result, "thread", "thread operation");
    ThreadState parsed;
    parsed.id = requiredString(thread, "id", "thread operation");
    parsed.model = optionalStringValue(result, "model",
                                       optionalStringValue(thread, "model"));
    parsed.reasoningEffort = optionalString(result, "reasoningEffort");
    if (!parsed.reasoningEffort) {
        parsed.reasoningEffort = optionalString(thread, "reasoningEffort");
    }
    parsed.serviceTier = optionalString(result, "serviceTier");
    parsed.raw = result;
    return parsed;
}

TurnState parseTurnState(const Json& result) {
    const Json& turn = requiredObject(result, "turn", "turn/start");
    TurnState parsed;
    parsed.id = requiredString(turn, "id", "turn/start");
    parsed.status = optionalStringValue(turn, "status");
    parsed.raw = result;
    return parsed;
}

ServerEvent parseServerEvent(std::string method,
                             Json params,
                             std::optional<Json> requestId) {
    ServerEvent event;
    event.kind = requestId ? EventKind::ServerRequest
                           : eventKindForMethod(method);
    event.method = std::move(method);
    event.params = std::move(params);
    event.isRequest = requestId.has_value();
    event.requestId = std::move(requestId);

    if (!event.params.is_object()) {
        return event;
    }
    event.threadId = optionalStringValue(event.params, "threadId");
    event.turnId = optionalStringValue(event.params, "turnId");
    event.itemId = optionalStringValue(event.params, "itemId");
    event.delta = optionalStringValue(event.params, "delta");

    const auto thread = event.params.find("thread");
    if (event.threadId.empty() && thread != event.params.end() &&
        thread->is_object()) {
        event.threadId = optionalStringValue(*thread, "id");
    }
    const auto turn = event.params.find("turn");
    if (event.turnId.empty() && turn != event.params.end() &&
        turn->is_object()) {
        event.turnId = optionalStringValue(*turn, "id");
    }
    const auto item = event.params.find("item");
    if (event.itemId.empty() && item != event.params.end() &&
        item->is_object()) {
        event.itemId = optionalStringValue(*item, "id");
    }
    return event;
}

UserInput UserInput::text(std::string textValue) {
    return {Kind::Text, std::move(textValue), std::nullopt};
}

UserInput UserInput::imageUrl(std::string url,
                              std::optional<std::string> imageDetail) {
    return {Kind::ImageUrl, std::move(url), std::move(imageDetail)};
}

UserInput UserInput::localImage(std::string path,
                                std::optional<std::string> imageDetail) {
    return {Kind::LocalImage, std::move(path), std::move(imageDetail)};
}

Json UserInput::toJson() const {
    Json encoded = Json::object();
    switch (kind) {
        case Kind::Text:
            encoded = {{"type", "text"}, {"text", value}};
            break;
        case Kind::ImageUrl:
            encoded = {{"type", "image"}, {"url", value}};
            break;
        case Kind::LocalImage:
            encoded = {{"type", "localImage"}, {"path", value}};
            break;
    }
    if (detail && kind != Kind::Text) {
        encoded["detail"] = *detail;
    }
    return encoded;
}

Client::Client(std::shared_ptr<MessageTransport> transport)
    : transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("Client requires a message transport");
    }
    transport_->setHandlers(
        [this](const Json& message) { receive(message); },
        [this](const std::string_view message) { transportFailed(message); },
        [this](const int exitCode) { transportExited(exitCode); });
}

Client::~Client() {
    if (transport_) {
        transport_->setHandlers({}, {}, {});
        transport_->stop();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    shutDown_ = true;
}

bool Client::start(const ProcessSpec& spec, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutDown_ = false;
    }
    initialized_.store(false);
    return transport_->start(spec, error);
}

void Client::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutDown_) {
            return;
        }
        shutDown_ = true;
    }
    initialized_.store(false);
    transport_->stop();
    failAllPending({kTransportClosedError,
                    "Codex app-server transport was stopped", Json()});
}

void Client::setEventHandler(EventHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    eventHandler_ = std::move(handler);
}

void Client::setProtocolErrorHandler(ProtocolErrorHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    protocolErrorHandler_ = std::move(handler);
}

void Client::setExitHandler(ExitHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    exitHandler_ = std::move(handler);
}

bool Client::initialized() const noexcept { return initialized_.load(); }

RequestId Client::initialize(const InitializeOptions& options,
                             InitializeCallback callback) {
    Json clientInfo = {{"name", options.client.name},
                       {"version", options.client.version}};
    if (!options.client.title.empty()) {
        clientInfo["title"] = options.client.title;
    }
    Json params = {{"clientInfo", std::move(clientInfo)}};
    Json capabilities = Json::object();
    if (options.experimentalApi) {
        capabilities["experimentalApi"] = true;
    }
    if (options.extensions.is_object() && !options.extensions.empty()) {
        capabilities["extensions"] = options.extensions;
    }
    if (!capabilities.empty()) {
        params["capabilities"] = std::move(capabilities);
    }

    return sendRequest(
        "initialize", std::move(params),
        [this, callback = std::move(callback)](Result<Json> raw) mutable {
            if (!raw) {
                if (callback) {
                    callback(Result<InitializeResult>::failure(
                        std::move(*raw.error)));
                }
                return;
            }

            InitializeResult parsed;
            try {
                parsed = parseInitializeResult(*raw.value);
            } catch (const std::exception& exception) {
                if (callback) {
                    callback(Result<InitializeResult>::failure(
                        invalidResponse("initialize", exception)));
                }
                return;
            }

            std::string notificationError;
            if (!sendNotification("initialized", Json::object(),
                                  notificationError)) {
                if (callback) {
                    callback(Result<InitializeResult>::failure(
                        {kTransportWriteError,
                         "Could not complete the Codex initialize handshake: " +
                             notificationError,
                         Json()}));
                }
                return;
            }
            initialized_.store(true);
            if (callback) {
                callback(Result<InitializeResult>::success(std::move(parsed)));
            }
        });
}

RequestId Client::readAccount(const bool refreshToken,
                              AccountCallback callback) {
    return sendRequest(
        "account/read", {{"refreshToken", refreshToken}},
        [callback = std::move(callback)](Result<Json> raw) mutable {
            deliverTyped<AccountState>(std::move(raw), "account/read",
                                       parseAccountState, std::move(callback));
        });
}

RequestId Client::listModels(const ModelListOptions& options,
                             ModelListCallback callback) {
    Json params = Json::object();
    if (options.cursor) {
        params["cursor"] = *options.cursor;
    }
    if (options.limit) {
        params["limit"] = *options.limit;
    }
    if (options.includeHidden) {
        params["includeHidden"] = *options.includeHidden;
    }
    return sendRequest(
        "model/list", std::move(params),
        [callback = std::move(callback)](Result<Json> raw) mutable {
            deliverTyped<ModelPage>(std::move(raw), "model/list",
                                    parseModelPage, std::move(callback));
        });
}

RequestId Client::startThread(const ThreadStartOptions& options,
                              ThreadCallback callback) {
    Json params = Json::object();
    if (options.model) {
        params["model"] = *options.model;
    }
    if (options.workingDirectory) {
        params["cwd"] = *options.workingDirectory;
    }
    if (options.approvalPolicy) {
        params["approvalPolicy"] = *options.approvalPolicy;
    }
    if (options.sandbox) {
        params["sandbox"] = *options.sandbox;
    }
    if (options.serviceTier) {
        params["serviceTier"] = *options.serviceTier;
    }
    if (options.developerInstructions) {
        params["developerInstructions"] = *options.developerInstructions;
    }
    if (options.ephemeral) {
        params["ephemeral"] = *options.ephemeral;
    }
    if (!options.dynamicTools.empty()) {
        params["dynamicTools"] = options.dynamicTools;
    }
    if (options.config.is_object() && !options.config.empty()) {
        params["config"] = options.config;
    }
    return sendRequest(
        "thread/start", std::move(params),
        [callback = std::move(callback)](Result<Json> raw) mutable {
            deliverTyped<ThreadState>(std::move(raw), "thread/start",
                                      parseThreadState, std::move(callback));
        });
}

RequestId Client::resumeThread(const ThreadResumeOptions& options,
                               ThreadCallback callback) {
    Json params = {{"threadId", options.threadId},
                   {"excludeTurns", options.excludeTurns}};
    if (options.model) {
        params["model"] = *options.model;
    }
    if (options.workingDirectory) {
        params["cwd"] = *options.workingDirectory;
    }
    if (options.approvalPolicy) {
        params["approvalPolicy"] = *options.approvalPolicy;
    }
    if (options.sandbox) {
        params["sandbox"] = *options.sandbox;
    }
    if (options.serviceTier) {
        params["serviceTier"] = *options.serviceTier;
    }
    if (options.developerInstructions) {
        params["developerInstructions"] = *options.developerInstructions;
    }
    return sendRequest(
        "thread/resume", std::move(params),
        [callback = std::move(callback)](Result<Json> raw) mutable {
            deliverTyped<ThreadState>(std::move(raw), "thread/resume",
                                      parseThreadState, std::move(callback));
        });
}

RequestId Client::startTurn(const TurnStartOptions& options,
                            TurnCallback callback) {
    Json input = Json::array();
    for (const UserInput& item : options.input) {
        input.push_back(item.toJson());
    }
    Json params = {{"threadId", options.threadId},
                   {"input", std::move(input)}};
    if (options.model) {
        params["model"] = *options.model;
    }
    if (options.effort) {
        params["effort"] = *options.effort;
    }
    if (options.serviceTier) {
        params["serviceTier"] = *options.serviceTier;
    }
    if (options.serviceTierForTurn) {
        params["serviceTierForTurn"] = *options.serviceTierForTurn;
    }
    if (options.clientUserMessageId) {
        params["clientUserMessageId"] = *options.clientUserMessageId;
    }
    return sendRequest(
        "turn/start", std::move(params),
        [callback = std::move(callback)](Result<Json> raw) mutable {
            deliverTyped<TurnState>(std::move(raw), "turn/start",
                                    parseTurnState, std::move(callback));
        });
}

RequestId Client::sendRequest(std::string method,
                              Json params,
                              RawCallback callback) {
    const RequestId requestId = nextRequestId_.fetch_add(1);
    Json message = {{"id", requestId},
                    {"method", method},
                    {"params", std::move(params)}};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(requestId,
                         PendingRequest{std::move(method), std::move(callback)});
    }

    std::string error;
    if (!transport_->send(message, error)) {
        RawCallback failedCallback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto pending = pending_.find(requestId);
            if (pending != pending_.end()) {
                failedCallback = std::move(pending->second.callback);
                pending_.erase(pending);
            }
        }
        if (failedCallback) {
            try {
                failedCallback(Result<Json>::failure(
                    {kTransportWriteError,
                     "Could not send request to Codex app-server: " +
                         redactForLog(error),
                     Json()}));
            } catch (...) {
                reportProtocolError(
                    "Client response callback threw after a transport failure");
            }
        }
    }
    return requestId;
}

bool Client::sendNotification(std::string method,
                              Json params,
                              std::string& error) {
    return transport_->send(
        {{"method", std::move(method)}, {"params", std::move(params)}}, error);
}

bool Client::respond(const Json& requestId,
                     Json result,
                     std::string& error) {
    if (!(requestId.is_string() || requestId.is_number_integer() ||
          requestId.is_number_unsigned())) {
        error = "Server request id must be a string or integer";
        return false;
    }
    return transport_->send({{"id", requestId}, {"result", std::move(result)}},
                            error);
}

bool Client::respondError(const Json& requestId,
                          const RpcError& rpcError,
                          std::string& error) {
    if (!(requestId.is_string() || requestId.is_number_integer() ||
          requestId.is_number_unsigned())) {
        error = "Server request id must be a string or integer";
        return false;
    }
    Json encodedError = {{"code", rpcError.code},
                         {"message", rpcError.message}};
    if (!rpcError.data.is_null()) {
        encodedError["data"] = rpcError.data;
    }
    return transport_->send(
        {{"id", requestId}, {"error", std::move(encodedError)}}, error);
}

void Client::receive(const Json& message) {
    if (!message.is_object()) {
        reportProtocolError("Codex app-server message is not a JSON object");
        return;
    }

    const auto method = message.find("method");
    if (method != message.end()) {
        if (!method->is_string()) {
            reportProtocolError(
                "Codex app-server message has a non-string method");
            return;
        }
        Json params = message.value("params", Json::object());
        std::optional<Json> serverRequestId;
        const auto incomingId = message.find("id");
        if (incomingId != message.end()) {
            if (!(incomingId->is_string() || incomingId->is_number_integer() ||
                  incomingId->is_number_unsigned())) {
                reportProtocolError(
                    "Codex app-server request has an invalid request id");
                return;
            }
            serverRequestId = *incomingId;
        }

        const ServerEvent event = parseServerEvent(
            method->get<std::string>(), std::move(params),
            std::move(serverRequestId));
        EventHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = eventHandler_;
        }
        if (handler) {
            try {
                handler(event);
            } catch (...) {
                reportProtocolError(
                    "Client event handler threw while processing a Codex event");
            }
        }
        return;
    }

    const auto id = message.find("id");
    if (id == message.end()) {
        reportProtocolError(
            "Codex app-server response has neither a method nor an id");
        return;
    }
    if (!id->is_number_integer() && !id->is_number_unsigned()) {
        reportProtocolError(
            "Codex app-server response id does not match a client request");
        return;
    }

    RequestId requestId = 0;
    try {
        requestId = id->get<RequestId>();
    } catch (const std::exception&) {
        reportProtocolError("Codex app-server response id is out of range");
        return;
    }

    const bool hasResult = message.contains("result");
    const bool hasError = message.contains("error");
    if (hasResult == hasError) {
        reportProtocolError(
            "Codex app-server response must contain exactly one of result or error");
        return;
    }

    PendingRequest pending;
    bool foundPending = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = pending_.find(requestId);
        if (iterator != pending_.end()) {
            pending = std::move(iterator->second);
            pending_.erase(iterator);
            foundPending = true;
        }
    }
    if (!foundPending) {
        reportProtocolError("Codex app-server returned an unknown request id");
        return;
    }
    if (!pending.callback) {
        return;
    }

    if (hasResult) {
        try {
            pending.callback(Result<Json>::success(message["result"]));
        } catch (...) {
            reportProtocolError(
                "Client response callback threw while processing a Codex response");
        }
        return;
    }

    const Json& error = message["error"];
    RpcError rpcError;
    if (error.is_object()) {
        const auto code = error.find("code");
        if (code != error.end() && code->is_number_integer()) {
            rpcError.code = code->get<int>();
        }
        rpcError.message = optionalStringValue(error, "message",
                                               "Codex app-server request failed");
        const auto data = error.find("data");
        if (data != error.end()) {
            rpcError.data = *data;
        }
    } else {
        rpcError.code = kInvalidResponseError;
        rpcError.message = "Codex app-server returned a malformed error";
    }
    try {
        pending.callback(Result<Json>::failure(std::move(rpcError)));
    } catch (...) {
        reportProtocolError(
            "Client response callback threw while processing a Codex error");
    }
}

void Client::transportFailed(const std::string_view message) {
    reportProtocolError(redactForLog(message));
}

void Client::transportExited(const int exitCode) {
    initialized_.store(false);
    failAllPending({kTransportClosedError,
                    "Codex app-server exited before replying", Json()});
    ExitHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = exitHandler_;
    }
    if (handler) {
        try {
            handler(exitCode);
        } catch (...) {
            reportProtocolError("Client exit handler threw");
        }
    }
}

void Client::reportProtocolError(std::string message) const {
    ProtocolErrorHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = protocolErrorHandler_;
    }
    if (handler) {
        try {
            handler(message);
        } catch (...) {
            // Diagnostics must never unwind through a process or Qt callback.
        }
    }
}

void Client::failAllPending(RpcError error) noexcept {
    std::vector<RawCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks.reserve(pending_.size());
        for (auto& [requestId, pending] : pending_) {
            (void)requestId;
            if (pending.callback) {
                callbacks.push_back(std::move(pending.callback));
            }
        }
        pending_.clear();
    }
    for (RawCallback& callback : callbacks) {
        try {
            callback(Result<Json>::failure(error));
        } catch (...) {
            // Process shutdown must not throw through a destructor or Qt signal.
        }
    }
}

}  // namespace docxstudio::codex
