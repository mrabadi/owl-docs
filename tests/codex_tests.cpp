#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    const std::size_t protocolDiagnosticCount = diagnostics.size();
    harness.process->emitStderr(
        "app-server diagnostic: optional MCP transport unavailable\n");
    CHECK(diagnostics.size() == protocolDiagnosticCount);
    harness.process->emitStdout("{\"id\":999,\"result\":{}}\n");
    CHECK(diagnostics.size() == protocolDiagnosticCount + 1);
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
    const Json restrictedConfig =
        {{"mcp_servers",
          {{"local_http", {{"enabled", false}}},
           {"writer_helper", {{"enabled", false}}}}}};
    ThreadStartOptions start;
    start.config = restrictedConfig;
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
    CHECK(startRequest.at("params").at("dynamicTools").size() == 4);
    CHECK(startRequest.at("params").at("dynamicTools").at(0).at("name") ==
          docxstudio::codex::kEditorReadTool);
    CHECK(startRequest.at("params").at("config") == restrictedConfig);
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
    resume.config = restrictedConfig;
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
    CHECK(resumeRequest.at("params").at("config") == restrictedConfig);
    CHECK(!resumeRequest.at("params").contains("dynamicTools"));
    harness.process->emitStdout(
        Json({{"id", resumeId},
              {"result", {{"thread", {{"id", "thread-persisted"},
                                        {"turns", Json::array()}}}}}})
            .dump() + "\n");
    CHECK(resumed);
}

void testIntegratedCapabilities() {
    const auto capabilities = docxstudio::codex::editorV1Capabilities();
    CHECK(capabilities.at("schemaVersion") == "editor.v1");
    CHECK(capabilities.at("catalogVersion") == "editor.v1.catalog.10");
    CHECK(capabilities.at("workflow").at("manualSkillActivationRequired") == false);
    CHECK(capabilities.at("workflow").value("figureMode", std::string()).find("new figures and reconstructions") != std::string::npos);
    CHECK(capabilities.at("workflow").value("figureEditing", std::string()).find("native patches retain source styling and background") != std::string::npos);
    const auto tools = docxstudio::codex::editorV1ToolDefinitions();
    const auto preview = std::find_if(tools.begin(), tools.end(), [](const auto& tool) {
        return tool.name == docxstudio::codex::kEditorPreviewTool;
    });
    CHECK(preview != tools.end());
    if (preview != tools.end()) CHECK(preview->description.find("native patches retain source styling and background") != std::string::npos);
    CHECK(capabilities.at("units").at("textOffsets") == "UTF-16 code units");
    CHECK(capabilities.at("bounds").at("previewOperations") == 256);
    CHECK(capabilities.at("bounds").at("encodedImageBytes") == 4194304);
    CHECK(capabilities.at("bounds").at("aggregateImageBytes") == 4194304);
    CHECK(capabilities.at("bounds").value("desktopEncodedImageBytes", 0) == 16777216);
    CHECK(capabilities.at("bounds").value("maxRequestJsonBytes", 0) == 8388608);
    CHECK(capabilities.at("groups").contains("figures"));
    CHECK(capabilities.at("examples").contains("insertFigure"));
    CHECK(capabilities.at("examples").contains("replaceFigure"));
    CHECK(capabilities.at("examples").contains("insertTable"));
    const auto& exampleCells = capabilities.at("examples").at("insertTable").at("operations").at(0).at("cells");
    CHECK(exampleCells.is_array());
    if (exampleCells.is_array()) {
        CHECK(exampleCells.size() == 2 && exampleCells.at(0).is_array() && exampleCells.at(1).is_array());
    }
    CHECK(capabilities.at("$defs").at("excalidrawLabel").at("properties").contains("id"));
    CHECK(capabilities.at("$defs").contains("excalidrawTarget"));
    const auto& elements = capabilities.at("$defs").at("excalidrawElement").at("oneOf");
    const auto arrow = std::find_if(elements.begin(), elements.end(), [](const Json& element) {
        return element.at("properties").at("type").value("const", std::string()) == "arrow";
    });
    CHECK(arrow != elements.end());
    if (arrow != elements.end()) CHECK(arrow->at("properties").contains("start") && arrow->at("properties").contains("end"));
    const auto& operations = capabilities.at("operationSchemas");
    const auto page = std::find_if(operations.begin(), operations.end(), [](const Json& operation) {
        return operation.at("properties").at("kind").value("const", std::string()) == "set_page_layout";
    });
    CHECK(page != operations.end());
    if (page != operations.end()) {
        CHECK(page->at("properties").contains("expectedLayoutStateId"));
        CHECK(std::find(page->at("required").begin(), page->at("required").end(), "expectedLayoutStateId") != page->at("required").end());
    }
    const auto definitions = docxstudio::codex::editorV1ToolDefinitions();
    const auto discovery = std::find_if(definitions.begin(), definitions.end(), [](const auto& definition) {
        return definition.name == "editor_v1_capabilities";
    });
    CHECK(discovery != definitions.end());
    if (discovery != definitions.end()) {
        CHECK(discovery->annotations.at("readOnlyHint") == true);
        CHECK(discovery->inputSchema.at("properties").contains("expectedRevision"));
        CHECK(discovery->outputSchema.at("properties").contains("runtime"));
    }
    CHECK(docxstudio::codex::isEditorV1Tool("editor_v1_capabilities"));
    CHECK(docxstudio::codex::editorV1ToolManifest().at("catalogVersion") == "editor.v1.catalog.10");
    const auto& readInput = definitions.at(0).inputSchema;
    for (const auto* scope : {"stories", "tables", "metadata", "images"})
        CHECK(std::find(readInput.at("properties").at("scope").at("enum").begin(), readInput.at("properties").at("scope").at("enum").end(), scope) !=
              readInput.at("properties").at("scope").at("enum").end());
    CHECK(readInput.at("properties").contains("maxBlocks"));
    CHECK(readInput.at("properties").contains("blockOffset"));
    CHECK(readInput.at("properties").contains("imageIds"));
    if (readInput.at("properties").contains("imageIds"))
        CHECK(readInput.at("properties").at("imageIds").at("maxItems") == 64);
    if (readInput.at("properties").contains("maxBlocks")) {
        CHECK(readInput.at("properties").at("maxBlocks").at("maximum") == 2048);
        CHECK(readInput.at("properties").at("maxBlocks").at("default") == 512);
    }
    const auto& readOutput = definitions.at(0).outputSchema;
    CHECK(readOutput.at("properties").at("pageLayout").at("properties").contains("stateId"));
    CHECK(readOutput.at("properties").contains("nextBlockOffset"));
}

void testNativeFigurePatchSchema() {
    const auto capabilities = docxstudio::codex::editorV1Capabilities();
    const auto& operations = capabilities.at("operationSchemas");
    const auto patch = std::find_if(operations.begin(), operations.end(), [](const Json& operation) {
        return operation.at("properties").at("kind").value("const", std::string()) == "patch_excalidraw_figure";
    });
    CHECK(patch != operations.end());
    if (patch == operations.end()) return;
    CHECK(patch->at("properties").at("edits").at("maxItems") == 128);
    CHECK(patch->at("properties").at("edits").at("minItems") == 1);
    CHECK(capabilities.at("$defs").at("excalidrawPatchChanges").at("properties").contains("points"));
    CHECK(capabilities.at("$defs").at("excalidrawPatchChanges").at("properties").contains("angle"));
    CHECK(!capabilities.at("$defs").at("excalidrawPatchChanges").at("properties").contains("files"));
}

void testListOperationSchemas() {
    const auto capabilities = docxstudio::codex::editorV1Capabilities();
    const auto& operations = capabilities.at("operationSchemas");
    for (const auto* kind : {"set_list", "set_list_level", "set_list_layout"}) {
        const auto entry = std::find_if(operations.begin(), operations.end(), [&](const Json& operation) {
            return operation.at("properties").at("kind").value("const", std::string()) == kind;
        });
        CHECK(entry != operations.end());
        if (entry == operations.end()) continue;
        const auto& ids = entry->at("properties").at("blockIds");
        CHECK(ids.at("minItems") == 1 && ids.at("maxItems") == 2048);
        CHECK(ids.at("uniqueItems") == true);
        if (std::string_view(kind) == "set_list")
            CHECK(entry->at("properties").at("listKind").at("enum") == Json::array({"bullets", "numbered", "none"}));
        if (std::string_view(kind) == "set_list_level")
            CHECK(entry->at("properties").at("level").at("minimum") == 1 && entry->at("properties").at("level").at("maximum") == 10);
        if (std::string_view(kind) == "set_list_layout") {
            const auto& levels = entry->at("properties").at("levels");
            CHECK(levels.at("minItems") == 10 && levels.at("maxItems") == 10);
            CHECK(levels.at("items").at("properties").at("bulletIndentSpaces").at("maximum") == 400);
            CHECK(levels.at("items").at("properties").at("textIndentSpaces").at("maximum") == 64);
        }
    }
}

void testSharedFormattingSchemas() {
    const auto capabilities = docxstudio::codex::editorV1Capabilities();
    const auto& definitions = capabilities.at("$defs");
    const auto& paragraph = definitions.at("paragraphStyle");
    for (const auto* name : {"leftIndentPoints", "rightIndentPoints", "firstLineIndentPoints", "keepLines", "pageBreakBefore", "lineSpacingPoints", "lineSpacingRule"})
        CHECK(paragraph.at("properties").contains(name));
    CHECK(paragraph.at("properties").at("styleId").at("oneOf").at(1).at("type") == "null");
    const auto& semanticParagraph = definitions.at("semanticParagraphFormat");
    for (const auto& [name, property] : semanticParagraph.at("properties").items()) {
        CHECK(paragraph.at("properties").contains(name));
        if (paragraph.at("properties").contains(name)) CHECK(paragraph.at("properties").at(name) == property);
    }
    CHECK(definitions.at("textStyle").at("properties").at("highlightColor") ==
          definitions.at("semanticTextStyle").at("properties").at("highlightColor"));
    CHECK(definitions.at("textStyle").at("properties").at("fontSizePoints") ==
          definitions.at("semanticTextStyle").at("properties").at("fontSizePoints"));
    const auto& operations = capabilities.at("operationSchemas");
    const auto page = std::find_if(operations.begin(), operations.end(), [](const Json& operation) {
        return operation.at("properties").at("kind").value("const", std::string()) == "set_page_layout";
    });
    CHECK(page != operations.end());
    if (page != operations.end()) CHECK(page->at("properties").at("expectedLayoutStateId").value("maximum", std::uint64_t{0}) == 9007199254740991ULL);
}

void testFigureReconstructionSchema() {
    const auto capabilities = docxstudio::codex::editorV1Capabilities();
    const auto& operations = capabilities.at("operationSchemas");
    const auto replace = std::find_if(operations.begin(), operations.end(), [](const Json& operation) {
        return operation.at("properties").at("kind").value("const", std::string()) == "replace_excalidraw_figure";
    });
    CHECK(replace != operations.end());
    if (replace == operations.end()) return;
    CHECK(replace->at("properties").contains("reconstructScene"));
    if (replace->at("properties").contains("reconstructScene")) {
        CHECK(replace->at("properties").at("reconstructScene").at("type") == "boolean");
        CHECK(replace->at("properties").at("reconstructScene").at("default") == false);
    }
    for (const auto& element : capabilities.at("examples").at("replaceFigure").at("operations").at(0).at("figure").at("elements"))
        if (element.contains("label")) CHECK(element.at("label").contains("id"));
}

void testPublishedCatalogMatchesRegistration() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "schemas/editor.v1.json";
    std::ifstream input(path);
    CHECK(input.good());
    if (!input.good()) return;
    const auto published = Json::parse(input);
    CHECK(published.value("catalogVersion", std::string()) == docxstudio::codex::kEditorToolCatalogVersion);
    const auto definitions = docxstudio::codex::editorV1ToolDefinitions();
    CHECK(published.at("tools").size() == definitions.size());
    for (const auto& definition : definitions) {
        const auto entry = std::find_if(published.at("tools").begin(), published.at("tools").end(), [&](const Json& tool) {
            return tool.value("name", std::string()) == definition.name;
        });
        CHECK(entry != published.at("tools").end());
        if (entry == published.at("tools").end()) continue;
        CHECK(entry->value("description", std::string()) == definition.description);
        auto expectedInput = definition.inputSchema;
        if (expectedInput.contains("$defs")) {
            for (const auto& [name, schema] : expectedInput.at("$defs").items()) {
                CHECK(published.at("$defs").contains(name));
                if (published.at("$defs").contains(name)) CHECK(published.at("$defs").at(name) == schema);
            }
            expectedInput.erase("$defs");
        }
        expectedInput["properties"]["documentId"] = {{"type", "string"}, {"minLength", 1}};
        if (!expectedInput.contains("required")) expectedInput["required"] = Json::array();
        expectedInput["required"].push_back("documentId");
        CHECK(published.at(Json::json_pointer(entry->at("inputSchema").at("$ref").get<std::string>().substr(1))) == expectedInput);
        CHECK(published.at(Json::json_pointer(entry->at("outputSchema").at("$ref").get<std::string>().substr(1))) == definition.outputSchema);
        CHECK(entry->at("annotations") == definition.annotations);
    }
}

void testEditorToolSchemas() {
    const auto definitions = docxstudio::codex::editorV1ToolDefinitions();
    CHECK(definitions.size() == 4);
    CHECK(definitions[0].name == docxstudio::codex::kEditorReadTool);
    CHECK(definitions[1].name == docxstudio::codex::kEditorSearchTool);
    CHECK(definitions[2].name == docxstudio::codex::kEditorPreviewTool);
    CHECK(definitions[3].name == docxstudio::codex::kEditorCapabilitiesTool);
    CHECK(std::none_of(definitions.begin(), definitions.end(), [](const auto& definition) {
        return definition.name == docxstudio::codex::kEditorFileCapabilityTool;
    }));
    CHECK(definitions[0].annotations.at("readOnlyHint") == true);
    CHECK(definitions[1].annotations.at("readOnlyHint") == true);
    CHECK(definitions[2].annotations.at("readOnlyHint") == false);
    CHECK(definitions[2].outputSchema.at("properties")
              .at("committed")
              .at("const") == false);
    const auto& readScopes = definitions[0].inputSchema.at("properties")
                                 .at("scope").at("enum");
    CHECK(std::find(readScopes.begin(), readScopes.end(), "viewport") ==
          readScopes.end());
    CHECK(definitions[1].inputSchema.at("required").at(0) == "query");
    CHECK(definitions[0].inputSchema.at("properties")
              .contains("tableCellTargets"));
    const auto& tableCellTarget =
        definitions[0].inputSchema.at("properties")
            .at("tableCellTargets").at("items");
    CHECK(tableCellTarget.at("required").at(0) == "tableId");
    CHECK(tableCellTarget.at("required").at(1) == "cellId");
    CHECK(definitions[1].inputSchema.at("properties")
              .at("maxResults").at("maximum") == 500);
    CHECK(definitions[1].inputSchema.at("properties")
              .at("caseSensitive").at("type") == "boolean");
    CHECK(definitions[1].inputSchema.at("properties")
              .at("wholeWord").at("type") == "boolean");
    CHECK(definitions[1].outputSchema.at("properties")
              .at("results").at("maxItems") == 500);
    CHECK(definitions[2].inputSchema.dump().find("insert_image") ==
          std::string::npos);
    CHECK(!definitions[3].inputSchema.at("properties").contains("path"));
    CHECK(definitions[3].annotations.at("readOnlyHint") == true);

    const Json dynamicSpec = definitions[0].toDynamicToolSpec();
    CHECK(dynamicSpec.at("type") == "function");
    CHECK(dynamicSpec.contains("inputSchema"));
    CHECK(!dynamicSpec.contains("outputSchema"));
    CHECK(!dynamicSpec.contains("annotations"));

    const Json manifest = docxstudio::codex::editorV1ToolManifest();
    CHECK(manifest.at("schemaVersion") == "editor.v1");
    CHECK(manifest.at("tools").size() == 4);
    CHECK(!docxstudio::codex::kEditorToolCatalogVersion.empty());
    CHECK(docxstudio::codex::isEditorV1Tool("editor_v1_search"));
    CHECK(docxstudio::codex::isEditorV1Tool("editor_v1_preview"));
    CHECK(!docxstudio::codex::isEditorV1Tool("filesystem_read"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "capabilities") {
        testIntegratedCapabilities();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "figure-patch") {
        testNativeFigurePatchSchema();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "published-catalog") {
        testPublishedCatalogMatchesRegistration();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "list-schema") {
        testListOperationSchemas();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "format-schema") {
        testSharedFormattingSchemas();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "figure-reconstruct") {
        testFigureReconstructionSchema();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--dump-editor-manifest") {
        std::cout << docxstudio::codex::editorV1ToolManifest().dump(2) << '\n';
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--dump-editor-capabilities") {
        std::cout << docxstudio::codex::editorV1Capabilities().dump(2) << '\n';
        return 0;
    }
    testHandshakeAndJsonlFraming();
    testCorrelationAndDynamicCatalog();
    testThreadTurnEventsAndServerRequests();
    testErrorsExitAndRedaction();
    testDynamicToolRegistrationAndThreadResume();
    testIntegratedCapabilities();
    testNativeFigurePatchSchema();
    testListOperationSchemas();
    testSharedFormattingSchemas();
    testFigureReconstructionSchema();
    testPublishedCatalogMatchesRegistration();
    testEditorToolSchemas();

    if (failures != 0) {
        std::cerr << failures << " Codex integration test(s) failed\n";
        return 1;
    }
    std::cout << "All Codex integration tests passed\n";
    return 0;
}
