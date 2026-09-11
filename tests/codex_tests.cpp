#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "docxstudio/codex/client.hpp"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/codex/redaction.hpp"
#include "docxstudio/codex/transport.hpp"

namespace {

using docxstudio::codex::AccountState;
using docxstudio::codex::Client;
using docxstudio::codex::EventKind;
using docxstudio::codex::InitializeOptions;
using docxstudio::codex::Json;
using docxstudio::codex::MessageTransport;
using docxstudio::codex::ModelListOptions;
using docxstudio::codex::Process;
using docxstudio::codex::ProcessCallbacks;
using docxstudio::codex::ProcessSpec;
using docxstudio::codex::RequestId;
using docxstudio::codex::Result;
using docxstudio::codex::ServerEvent;
using docxstudio::codex::StdioJsonlTransport;
using docxstudio::codex::ThreadStartOptions;
using docxstudio::codex::ThreadResumeOptions;
using docxstudio::codex::TurnStartOptions;
using docxstudio::codex::UserInput;

int failures = 0;

void check(const bool condition,
           const char* expression,
           const char* file,
           const int line) {
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << file << ':' << line << ": CHECK failed: " << expression
              << '\n';
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, \
                                __FILE__, __LINE__)

class FakeProcess final : public Process {
public:
    bool start(const ProcessSpec& requestedSpec,
               ProcessCallbacks requestedCallbacks,
               std::string& error) override {
        if (failStart) {
            error = "synthetic start failure";
            return false;
        }
        spec = requestedSpec;
        callbacks = std::move(requestedCallbacks);
        running = true;
        return true;
    }

    bool writeStandardInput(const std::string_view bytes,
                            std::string& error) override {
        if (!running || failWrite) {
            error = "synthetic write failure";
            return false;
        }
        writes.emplace_back(bytes);
        return true;
    }

    void stop() noexcept override { running = false; }

    void emitStdout(const std::string_view bytes) const {
        if (callbacks.standardOutput) {
            callbacks.standardOutput(bytes);
        }
    }

    void emitStderr(const std::string_view bytes) const {
        if (callbacks.standardError) {
            callbacks.standardError(bytes);
        }
    }

    void emitExit(const int code) {
        running = false;
        if (callbacks.exited) {
            callbacks.exited(code);
        }
    }

    bool failStart = false;
    bool failWrite = false;
    bool running = false;
    ProcessSpec spec;
    ProcessCallbacks callbacks;
    std::vector<std::string> writes;
};

struct ClientHarness {
    explicit ClientHarness(
        const std::size_t maxBytes =
            StdioJsonlTransport::kDefaultMaxMessageBytes) {
        auto ownedProcess = std::make_unique<FakeProcess>();
        process = ownedProcess.get();
        transport = std::make_shared<StdioJsonlTransport>(
            std::move(ownedProcess), maxBytes);
        client = std::make_unique<Client>(transport);
        std::string error;
        CHECK(client->start(ProcessSpec{}, error));
        CHECK(error.empty());
    }

    FakeProcess* process = nullptr;
    std::shared_ptr<MessageTransport> transport;
    std::unique_ptr<Client> client;
};

Json writtenMessage(const FakeProcess& process, const std::size_t index) {
    CHECK(index < process.writes.size());
    if (index >= process.writes.size()) {
        return Json();
    }
    const std::string& frame = process.writes[index];
    CHECK(!frame.empty() && frame.back() == '\n');
    return Json::parse(frame);
}

void testHandshakeAndJsonlFraming() {
    ClientHarness harness;
    CHECK(harness.process->spec.program == "codex");
    CHECK(harness.process->spec.arguments ==
          std::vector<std::string>({"app-server", "--stdio"}));

    bool completed = false;
    InitializeOptions options;
    options.client.name = "owl-docs-tests";
    options.client.title = "Owl Docs Tests";
    options.client.version = "1.2.3";
    const RequestId requestId = harness.client->initialize(
        options, [&](auto result) {
            CHECK(result.ok());
            CHECK(result.value->userAgent == "codex-test/1");
            CHECK(result.value->platformOs == "linux");
            completed = true;
        });

    CHECK(requestId == 1);
    CHECK(harness.process->writes.size() == 1);
    const Json request = writtenMessage(*harness.process, 0);
    CHECK(request.at("method") == "initialize");
    CHECK(request.at("id") == requestId);
    CHECK(!request.contains("jsonrpc"));
    CHECK(request.at("params").at("clientInfo").at("name") ==
          "owl-docs-tests");
    CHECK(!request.at("params").contains("capabilities"));

    harness.process->emitStdout("{\"id\":1,\"result\":{");
    CHECK(!completed);
    harness.process->emitStdout(
        "\"userAgent\":\"codex-test/1\",\"platformOs\":\"linux\","
        "\"platformFamily\":\"unix\"}}\r\n");
    CHECK(completed);
    CHECK(harness.client->initialized());
    CHECK(harness.process->writes.size() == 2);
    const Json initialized = writtenMessage(*harness.process, 1);
    CHECK(initialized.at("method") == "initialized");
    CHECK(initialized.at("params").is_object());
    CHECK(!initialized.contains("id"));
}

void testCorrelationAndDynamicCatalog() {
    ClientHarness harness;
    bool accountCompleted = false;
    bool modelsCompleted = false;

    const RequestId accountId = harness.client->readAccount(
        false, [&](Result<AccountState> result) {
            CHECK(result.ok());
            CHECK(result.value->signedIn);
            CHECK(result.value->accountType == "chatgpt");
            CHECK(result.value->email == "writer@example.test");
            CHECK(result.value->planType == "plus");
            accountCompleted = true;
        });
    ModelListOptions listOptions;
    listOptions.limit = 25;
    listOptions.includeHidden = false;
    const RequestId modelsId = harness.client->listModels(
        listOptions, [&](auto result) {
            CHECK(result.ok());
            CHECK(result.value->models.size() == 1);
            const auto& model = result.value->models.front();
            CHECK(model.wireModel() == "gpt-future-codex");
            CHECK(model.defaultReasoningEffort == "high");
            CHECK(model.reasoningEfforts.size() == 2);
            CHECK(model.reasoningEfforts[1].id == "ultra");
            CHECK(model.serviceTiers.size() == 2);
            CHECK(model.serviceTiers[0].id == "default");
            CHECK(model.serviceTiers[1].id == "warp");
            CHECK(model.defaultServiceTier == "default");
            CHECK(result.value->nextCursor == "page-2");
            modelsCompleted = true;
        });

    CHECK(accountId == 1);
    CHECK(modelsId == 2);
    const Json accountRequest = writtenMessage(*harness.process, 0);
    const Json modelRequest = writtenMessage(*harness.process, 1);
    CHECK(accountRequest.at("method") == "account/read");
    CHECK(accountRequest.at("params").at("refreshToken") == false);
    CHECK(modelRequest.at("method") == "model/list");
    CHECK(modelRequest.at("params").at("limit") == 25);
    CHECK(modelRequest.at("params").at("includeHidden") == false);

    const Json modelResponse = {
        {"id", modelsId},
        {"result",
         {{"data",
           {{{"id", "catalog-entry"},
             {"model", "gpt-future-codex"},
             {"displayName", "Future Codex"},
             {"description", "A test model"},
             {"hidden", false},
             {"isDefault", true},
             {"defaultReasoningEffort", "high"},
             {"supportedReasoningEfforts",
              {{{"reasoningEffort", "high"},
                {"description", "Deep reasoning"}},
               {{"reasoningEffort", "ultra"},
                {"description", "A future value"}}}},
             {"defaultServiceTier", "default"},
             {"serviceTiers",
              {{{"id", "default"},
                {"name", "Standard"},
                {"description", "Normal speed"}},
               {{"id", "warp"},
                {"name", "Warp"},
                {"description", "A future tier"}}}},
             {"inputModalities", {"text", "image"}}}}},
          {"nextCursor", "page-2"}}}};
    harness.process->emitStdout(modelResponse.dump() + "\n");
    CHECK(modelsCompleted);
    CHECK(!accountCompleted);

    const Json accountResponse =
        {{"id", accountId},
         {"result",
          {{"requiresOpenaiAuth", true},
           {"account",
            {{"type", "chatgpt"},
             {"email", "writer@example.test"},
             {"planType", "plus"}}}}}};
    harness.process->emitStdout(accountResponse.dump() + "\n");
    CHECK(accountCompleted);
}

void testThreadTurnEventsAndServerRequests() {
    ClientHarness harness;
    std::vector<ServerEvent> events;
    harness.client->setEventHandler(
        [&](const ServerEvent& event) { events.push_back(event); });

    ThreadStartOptions threadOptions;
    threadOptions.model = "gpt-future-codex";
    threadOptions.workingDirectory = "/safe/document-session";
    threadOptions.approvalPolicy = "on-request";
    threadOptions.sandbox = "read-only";
    threadOptions.serviceTier = "fast";
    threadOptions.ephemeral = true;

    bool threadCompleted = false;
    const RequestId threadId = harness.client->startThread(
        threadOptions, [&](auto result) {
            CHECK(result.ok());
            CHECK(result.value->id == "thread-7");
            CHECK(result.value->model == "gpt-future-codex");
            CHECK(result.value->reasoningEffort == "high");
            CHECK(result.value->serviceTier == "fast");
            threadCompleted = true;
        });

    const Json threadRequest = writtenMessage(*harness.process, 0);
    CHECK(threadRequest.at("method") == "thread/start");
    CHECK(threadRequest.at("params").at("sandbox") == "read-only");
    CHECK(threadRequest.at("params").at("approvalPolicy") == "on-request");
    CHECK(threadRequest.at("params").at("serviceTier") == "fast");

    harness.process->emitStdout(
        Json({{"id", threadId},
              {"result",
               {{"thread", {{"id", "thread-7"}}},
                {"model", "gpt-future-codex"},
                {"reasoningEffort", "high"},
                {"serviceTier", "fast"}}}})
            .dump() +
        "\n");
    CHECK(threadCompleted);

    TurnStartOptions turnOptions;
    turnOptions.threadId = "thread-7";
    turnOptions.input = {UserInput::text("Improve the selected paragraph")};
    turnOptions.model = "gpt-future-codex";
    turnOptions.effort = "ultra";
    turnOptions.serviceTierForTurn = "default";
    turnOptions.clientUserMessageId = "message-42";

    bool turnCompleted = false;
    const RequestId turnRequestId = harness.client->startTurn(
        turnOptions, [&](auto result) {
            CHECK(result.ok());
            CHECK(result.value->id == "turn-9");
            CHECK(result.value->status == "inProgress");
            turnCompleted = true;
        });
    const Json turnRequest = writtenMessage(*harness.process, 1);
    CHECK(turnRequest.at("method") == "turn/start");
    CHECK(turnRequest.at("params").at("threadId") == "thread-7");
    CHECK(turnRequest.at("params").at("effort") == "ultra");
    CHECK(turnRequest.at("params").at("serviceTierForTurn") == "default");
    CHECK(turnRequest.at("params").at("input").at(0).at("type") == "text");

    harness.process->emitStdout(
        "{\"method\":\"item/agentMessage/delta\",\"params\":{"
        "\"threadId\":\"thread-7\",\"turnId\":\"turn-9\","
        "\"itemId\":\"item-2\",\"delta\":\"Revised\"}}\n");
    CHECK(events.size() == 1);
    CHECK(events[0].kind == EventKind::AgentMessageDelta);
    CHECK(events[0].threadId == "thread-7");
    CHECK(events[0].turnId == "turn-9");
    CHECK(events[0].itemId == "item-2");
    CHECK(events[0].delta == "Revised");

    harness.process->emitStdout(
        "{\"id\":\"tool-call-3\",\"method\":\"item/tool/call\","
        "\"params\":{\"threadId\":\"thread-7\",\"turnId\":"
        "\"turn-9\",\"tool\":\"editor_v1_read\",\"arguments\":{}}}\n");
    CHECK(events.size() == 2);
    CHECK(events[1].kind == EventKind::ServerRequest);
    CHECK(events[1].isRequest);
    CHECK(events[1].requestId->get<std::string>() == "tool-call-3");

    std::string responseError;
    CHECK(harness.client->respond(
        *events[1].requestId,
        {{"contentItems", {{{"type", "inputText"}, {"text", "snapshot"}}}},
         {"success", true}},
        responseError));
    const Json toolResponse = writtenMessage(*harness.process, 2);
    CHECK(toolResponse.at("id") == "tool-call-3");
    CHECK(toolResponse.at("result").at("success") == true);

    harness.process->emitStdout(
        Json({{"id", turnRequestId},
              {"result",
               {{"turn",
                 {{"id", "turn-9"},
                  {"status", "inProgress"},
                  {"items", Json::array()}}}}}})
            .dump() +
        "\n");
    CHECK(turnCompleted);
}

void testErrorsExitAndRedaction() {
    ClientHarness harness;
    std::vector<std::string> diagnostics;
    harness.client->setProtocolErrorHandler(
        [&](const std::string_view message) {
            diagnostics.emplace_back(message);
        });

    bool rpcErrorReceived = false;
    const RequestId failedId = harness.client->readAccount(
        true, [&](auto result) {
            CHECK(!result.ok());
            CHECK(result.error.has_value());
            if (result.error) {
                CHECK(result.error->code == -32001);
                CHECK(result.error->message == "overloaded");
                CHECK(result.error->data.at("retryAfterMs") == 50);
            }
            rpcErrorReceived = true;
        });
    harness.process->emitStdout(
        Json({{"id", failedId},
              {"error",
               {{"code", -32001},
                {"message", "overloaded"},
                {"data", {{"retryAfterMs", 50}}}}}})
            .dump() +
        "\n");
    CHECK(rpcErrorReceived);

    harness.process->emitStdout("{not-json}\n");
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.back().find("malformed JSONL") != std::string::npos);
    harness.process->emitStderr(
        "Authorization: Bearer stderr-secret-token\n");
    CHECK(diagnostics.back().find("stderr-secret-token") ==
          std::string::npos);
    CHECK(diagnostics.back().find("[REDACTED]") != std::string::npos);
    harness.process->emitStdout("{\"id\":999,\"result\":{}}\n");
    CHECK(diagnostics.back().find("unknown request id") != std::string::npos);

    bool exitFailureReceived = false;
    harness.client->listModels({}, [&](auto result) {
        CHECK(!result.ok());
        CHECK(result.error.has_value());
        if (result.error) CHECK(result.error->code == -32097);
        exitFailureReceived = true;
    });
    harness.process->emitExit(23);
    CHECK(exitFailureReceived);

    // Assemble the deliberately fake credential marker at runtime so the
    // repository never contains a string that resembles a usable API key.
    const std::string fakeApiKey =
        std::string("sk") + "-test-redaction-only-00000000";
    const Json secrets =
        {{"access_token", "secret-access"},
         {"token", "secret-generic"},
         {"tokenUsage", 1234},
         {"nested", {{"api_key", fakeApiKey}}}};
    const Json redacted = docxstudio::codex::redactJson(secrets);
    CHECK(redacted.at("access_token") == "[REDACTED]");
    CHECK(redacted.at("token") == "[REDACTED]");
    CHECK(redacted.at("tokenUsage") == 1234);
    CHECK(redacted.at("nested").at("api_key") == "[REDACTED]");

    const std::string log = docxstudio::codex::redactForLog(
        "Authorization: Bearer abc.def-123 OPENAI_API_KEY=" + fakeApiKey);
    CHECK(log.find("abc.def-123") == std::string::npos);
    CHECK(log.find(fakeApiKey) == std::string::npos);
    CHECK(log.find("[REDACTED]") != std::string::npos);

    ClientHarness boundedHarness(32);
    std::string boundedDiagnostic;
    boundedHarness.client->setProtocolErrorHandler(
        [&](const std::string_view message) {
            boundedDiagnostic = std::string(message);
        });
    boundedHarness.process->emitStdout(std::string(33, 'x') + "\n");
    CHECK(boundedDiagnostic.find("larger than") != std::string::npos);
}

void testDynamicToolRegistrationAndThreadResume() {
    ClientHarness harness;
    ThreadStartOptions start;
    for (const auto& definition : docxstudio::codex::editorV1ToolDefinitions()) {
        start.dynamicTools.push_back(definition.toDynamicToolSpec());
    }
    bool started = false;
    const RequestId startId = harness.client->startThread(start, [&](auto result) {
        CHECK(result.ok());
        CHECK(result.value->id == "thread-persisted");
        started = true;
    });
    const Json startRequest = writtenMessage(*harness.process, 0);
    CHECK(startRequest.at("params").at("dynamicTools").size() == 3);
    CHECK(startRequest.at("params").at("dynamicTools").at(0).at("name") ==
          docxstudio::codex::kEditorReadTool);
    harness.process->emitStdout(
        Json({{"id", startId},
              {"result", {{"thread", {{"id", "thread-persisted"}}}}}})
            .dump() + "\n");
    CHECK(started);

    ThreadResumeOptions resume;
    resume.threadId = "thread-persisted";
    resume.workingDirectory = "/empty/document-sandbox";
    resume.approvalPolicy = "never";
    resume.sandbox = "read-only";
    bool resumed = false;
    const RequestId resumeId = harness.client->resumeThread(resume, [&](auto result) {
        CHECK(result.ok());
        CHECK(result.value->id == "thread-persisted");
        resumed = true;
    });
    const Json resumeRequest = writtenMessage(*harness.process, 1);
    CHECK(resumeRequest.at("method") == "thread/resume");
    CHECK(resumeRequest.at("params").at("threadId") == "thread-persisted");
    CHECK(resumeRequest.at("params").at("excludeTurns") == true);
    CHECK(resumeRequest.at("params").at("sandbox") == "read-only");
    harness.process->emitStdout(
        Json({{"id", resumeId},
              {"result", {{"thread", {{"id", "thread-persisted"},
                                        {"turns", Json::array()}}}}}})
            .dump() + "\n");
    CHECK(resumed);
}

void testEditorToolSchemas() {
    const auto definitions = docxstudio::codex::editorV1ToolDefinitions();
    CHECK(definitions.size() == 3);
    CHECK(definitions[0].name == docxstudio::codex::kEditorReadTool);
    CHECK(definitions[1].name == docxstudio::codex::kEditorPreviewTool);
    CHECK(definitions[2].name ==
          docxstudio::codex::kEditorFileCapabilityTool);
    CHECK(definitions[0].annotations.at("readOnlyHint") == true);
    CHECK(definitions[1].annotations.at("readOnlyHint") == false);
    CHECK(definitions[1].outputSchema.at("properties")
              .at("committed")
              .at("const") == false);
    const auto& readScopes = definitions[0].inputSchema.at("properties")
                                 .at("scope").at("enum");
    CHECK(std::find(readScopes.begin(), readScopes.end(), "viewport") ==
          readScopes.end());
    CHECK(!definitions[2].inputSchema.at("properties").contains("path"));
    CHECK(definitions[2].inputSchema.at("properties")
              .contains("capabilityId"));
    CHECK(definitions[2].inputSchema.dump().find("filesystem path") !=
          std::string::npos);

    const Json dynamicSpec = definitions[0].toDynamicToolSpec();
    CHECK(dynamicSpec.at("type") == "function");
    CHECK(dynamicSpec.contains("inputSchema"));
    CHECK(!dynamicSpec.contains("outputSchema"));
    CHECK(!dynamicSpec.contains("annotations"));

    const Json manifest = docxstudio::codex::editorV1ToolManifest();
    CHECK(manifest.at("schemaVersion") == "editor.v1");
    CHECK(manifest.at("tools").size() == 3);
    CHECK(docxstudio::codex::isEditorV1Tool("editor_v1_preview"));
    CHECK(!docxstudio::codex::isEditorV1Tool("filesystem_read"));
}

}  // namespace

int main() {
    testHandshakeAndJsonlFraming();
    testCorrelationAndDynamicCatalog();
    testThreadTurnEventsAndServerRequests();
    testErrorsExitAndRedaction();
    testDynamicToolRegistrationAndThreadResume();
    testEditorToolSchemas();

    if (failures != 0) {
        std::cerr << failures << " Codex integration test(s) failed\n";
        return 1;
    }
    std::cout << "All Codex integration tests passed\n";
    return 0;
}
