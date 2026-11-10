#include "docxstudio/app/CodexController.h"

#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/codex/transport.hpp"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using docxstudio::codex::Json;
using docxstudio::codex::Process;
using docxstudio::codex::ProcessCallbacks;
using docxstudio::codex::ProcessSpec;

int failures = 0;

void check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << message << '\n';
}

class FakeProcess final : public Process {
public:
    bool start(const ProcessSpec& requestedSpec,
               ProcessCallbacks requestedCallbacks,
               std::string&) override {
        spec = requestedSpec;
        callbacks = std::move(requestedCallbacks);
        running = true;
        return true;
    }

    bool writeStandardInput(std::string_view bytes,
                            std::string& error) override {
        if (!running) {
            error = "fake process is not running";
            return false;
        }
        writes.emplace_back(bytes);
        return true;
    }

    void stop() noexcept override { running = false; }

    void emitStdout(const Json& message) const {
        const std::string frame = message.dump() + '\n';
        if (callbacks.standardOutput) callbacks.standardOutput(frame);
    }

    bool running{false};
    ProcessSpec spec;
    ProcessCallbacks callbacks;
    std::vector<std::string> writes;
};

Json writtenMessage(const FakeProcess& process, const std::size_t index) {
    check(index < process.writes.size(), "expected app-server message was not written");
    if (index >= process.writes.size()) return {};
    return Json::parse(process.writes[index]);
}

void supplyCatalog(FakeProcess& process, const Json& models) {
    process.emitStdout({{"id", writtenMessage(process, 0).at("id")},
        {"result", {{"userAgent", "codex-controller-test"}, {"platformFamily", "unix"}, {"platformOs", "linux"}}}});
    process.emitStdout({{"id", writtenMessage(process, 2).at("id")},
        {"result", {{"config", {{"mcp_servers", Json::object()}}}, {"origins", Json::object()}}}});
    process.emitStdout({{"id", writtenMessage(process, 3).at("id")},
        {"result", {{"requiresOpenaiAuth", true}, {"account", {{"type", "chatgpt"}, {"planType", "plus"}}}}}});
    process.emitStdout({{"id", writtenMessage(process, 4).at("id")},
        {"result", {{"data", models}, {"nextCursor", nullptr}}}});
}

void beginDocumentTurn(docxstudio::app::CodexController& controller, FakeProcess& process,
                       const QString& document, const char* thread, const char* turn) {
    controller.sendMessage(QStringLiteral("Edit the document"), QStringLiteral("gpt-6.1-sol"),
        QStringLiteral("low"), QStringLiteral("default"), document, {}, {});
    auto request = writtenMessage(process, process.writes.size() - 1);
    if (request.value("method", std::string()) == "thread/start" || request.value("method", std::string()) == "thread/resume") {
        process.emitStdout({{"id", request.at("id")}, {"result", {{"thread", {{"id", thread}}}}}});
        request = writtenMessage(process, process.writes.size() - 1);
    }
    check(request.value("method", std::string()) == "turn/start", "turn fixture did not start a turn");
    process.emitStdout({{"id", request.at("id")}, {"result", {{"turn", {{"id", turn}, {"status", "inProgress"}}}}}});
}

void testToolsBelongToTheActiveTurn() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(nullptr, [&] {
        auto result = std::make_unique<FakeProcess>(); process = result.get(); return result;
    });
    int calls = 0;
    QString routedDocument;
    controller.setEditorToolHandler([&](const QString& document, const QString&, const Json& arguments, QString&) {
        ++calls; routedDocument = document;
        check(arguments.at("documentId") == document.toStdString(), "model supplied a different document binding");
        return Json{{"ok", true}};
    });
    controller.enable();
    supplyCatalog(*process, Json::array({{{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"}}}));
    beginDocumentTurn(controller, *process, QStringLiteral("document:A"), "thread-A", "turn-A");
    int requestNumber = 0;
    const auto tool = [&](const char* thread, const char* turn, Json arguments, const char* nameSpace = "") {
        Json params{{"threadId", thread}, {"turnId", turn}, {"callId", "call"},
            {"tool", "editor_v1_read"}, {"arguments", std::move(arguments)}};
        if (*nameSpace) params["namespace"] = nameSpace;
        process->emitStdout({{"id", "tool-" + std::to_string(++requestNumber)},
            {"method", "item/tool/call"}, {"params", std::move(params)}});
        return writtenMessage(*process, process->writes.size() - 1).at("result").at("success").get<bool>();
    };
    check(tool("thread-A", "turn-A", {{"scope", "selection"}, {"documentId", "forged"}}) && calls == 1,
          "the active turn cannot use its document-bound editor tools");
    process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "thread-A"},
        {"turn", {{"id", "turn-A"}, {"status", "completed"}}}}}});
    beginDocumentTurn(controller, *process, QStringLiteral("document:B"), "thread-B", "turn-B");
    check(!tool("thread-A", "turn-A", {{"scope", "selection"}}) && calls == 1,
          "a stale known thread can dispatch an editor tool during another document turn");
    check(!tool("thread-B", "turn-old", {{"scope", "selection"}}) && calls == 1,
          "a stale turn can dispatch an editor tool in the current thread");
    check(!tool("thread-B", "turn-B", "not-an-object") && calls == 1,
          "malformed tool arguments are silently treated as a valid selection read");
    check(!tool("thread-B", "turn-B", {{"scope", "selection"}}, "unregistered_namespace") && calls == 1,
          "an unregistered namespace can dispatch a flat OwlDocs editor tool");
    check(tool("thread-B", "turn-B", {{"scope", "document"}}) && calls == 2 && routedDocument == QStringLiteral("document:B"),
          "denying a stale tool request damages the valid current turn");
    process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "thread-B"},
        {"turn", {{"id", "turn-B"}, {"status", "completed"}}}}}});
    check(!tool("thread-B", "turn-B", {{"scope", "selection"}}) && calls == 2,
          "an editor tool can run after its turn has completed");
}

void testDisconnectDuringEditorTool() {
    for (const auto* action : {"shutdown", "destroy", "complete"}) {
        FakeProcess* process = nullptr;
        auto controller = std::make_unique<docxstudio::app::CodexController>(nullptr, [&] {
            auto result = std::make_unique<FakeProcess>(); process = result.get(); return result;
        });
        int calls = 0;
        controller->setEditorToolHandler([&](const QString&, const QString&, const Json&, QString&) {
            ++calls;
            if (std::string_view(action) == "destroy") controller.reset();
            else if (std::string_view(action) == "shutdown") controller->shutdown();
            else process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "thread-A"},
                {"turn", {{"id", "turn-A"}, {"status", "completed"}}}}}});
            return Json{{"ok", true}};
        });
        controller->enable();
        supplyCatalog(*process, Json::array({{{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"}}}));
        beginDocumentTurn(*controller, *process, QStringLiteral("document:A"), "thread-A", "turn-A");
        const auto writes = process->writes.size();
        const Json request = {{"id", "disconnect-tool"}, {"method", "item/tool/call"},
            {"params", {{"threadId", "thread-A"}, {"turnId", "turn-A"}, {"callId", "call"},
                {"tool", "editor_v1_read"}, {"arguments", {{"scope", "document"}}}}}};
        // A process read can contain more frames after the callback stopped
        // its session. Both the controller and frame loop must remain safe.
        const auto frames = request.dump() + '\n' + request.dump() + '\n';
        process->callbacks.standardOutput(frames);
        check(calls == 1, "a tool callback was dispatched after session teardown or turn completion");
        const bool completed = std::string_view(action) == "complete";
        check(process->writes.size() == writes + (completed ? 1 : 0),
              "a stale tool result was returned after its callback ended the session/turn");
        if (controller && !completed) check(!controller->connected(), "disconnect during an editor tool restarted the session");
        QCoreApplication::processEvents();
    }
}

void testDuplicateSavedThreadUsesTheActiveDocument() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(nullptr, [&] {
        auto result = std::make_unique<FakeProcess>(); process = result.get(); return result;
    });
    controller.restoreDocumentThread(QStringLiteral("document:A"), QStringLiteral("shared-thread"));
    controller.restoreDocumentThread(QStringLiteral("document:B"), QStringLiteral("shared-thread"));
    QString routedDocument;
    controller.setEditorToolHandler([&](const QString& document, const QString&, const Json& arguments, QString&) {
        routedDocument = document;
        check(arguments.at("documentId") == document.toStdString(), "duplicate thread bypassed host document injection");
        return Json{{"ok", true}};
    });
    controller.enable();
    supplyCatalog(*process, Json::array({{{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"}}}));
    for (const auto* key : {"document:A", "document:B"}) {
        const auto document = QString::fromLatin1(key);
        beginDocumentTurn(controller, *process, document, "shared-thread", key);
        process->emitStdout({{"id", key}, {"method", "item/tool/call"},
            {"params", {{"threadId", "shared-thread"}, {"turnId", key}, {"callId", "call"},
                {"tool", "editor_v1_read"}, {"arguments", {{"scope", "document"}}}}}});
        check(routedDocument == document, "a duplicated saved thread routes the active turn into a different document");
        process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "shared-thread"},
            {"turn", {{"id", key}, {"status", "completed"}}}}}});
    }
}

void testCompletionStatusIsNotAssumedSuccessful() {
    for (const auto* status : {"failed", "interrupted"}) {
        FakeProcess* process = nullptr;
        docxstudio::app::CodexController controller(nullptr, [&] {
            auto result = std::make_unique<FakeProcess>(); process = result.get(); return result;
        });
        QString reportedError;
        int finished = 0;
        bool busy = false;
        QObject::connect(&controller, &docxstudio::app::CodexController::errorOccurred,
            [&](const QString& message) { reportedError = message; });
        QObject::connect(&controller, &docxstudio::app::CodexController::assistantFinished,
            [&](const QString&) { ++finished; });
        QObject::connect(&controller, &docxstudio::app::CodexController::busyChanged,
            [&](bool value) { busy = value; });
        controller.enable();
        supplyCatalog(*process, Json::array({{{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"}}}));
        beginDocumentTurn(controller, *process, QStringLiteral("document:A"), "thread-A", "turn-A");
        process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "thread-A"},
            {"turn", {{"id", "turn-old"}, {"status", "completed"}}}}}});
        check(busy && finished == 0, "completion of an old turn ends the active turn");
        process->emitStdout({{"method", "turn/completed"}, {"params", {{"threadId", "thread-A"},
            {"turn", {{"id", "turn-A"}, {"status", status}, {"error", {{"message", "renderer unavailable"}}}}}}}});
        check(!busy && finished == 1 && !reportedError.isEmpty(),
              "a failed or interrupted turn is silently reported as a normal completed response");
        if (std::string(status) == "failed") check(reportedError.contains("renderer unavailable"),
            "the turn failure omits its actionable server error");
    }
}

void testSolDefaultAndSupportedEfforts() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(nullptr, [&] {
        auto result = std::make_unique<FakeProcess>();
        process = result.get();
        return result;
    });
    QStringList selectedEfforts;
    QObject::connect(&controller, &docxstudio::app::CodexController::effortsChanged,
        [&](const QStringList& values) { selectedEfforts = values; });
    controller.enable();
    check(process != nullptr, "controller did not create its process adapter");
    if (!process) return;
    supplyCatalog(*process, Json::array({
        {{"id", "gpt-5.6-luna"}, {"model", "gpt-5.6-luna"}, {"isDefault", true},
            {"supportedReasoningEfforts", {"low", "medium"}}},
        {{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"}, {"isDefault", false},
            {"supportedReasoningEfforts", {"ultra", "max", "high", "low", "xhigh", "medium"}}}
    }));
    check(selectedEfforts == QStringList({"low", "medium", "high", "xhigh", "ultra"}),
          "Codex did not default to GPT-6.1 Sol with the requested ordered reasoning choices");
    const auto writes = process->writes.size();
    controller.enable();
    check(process->writes.size() == writes, "opening connected Codex started a duplicate connection");
    controller.selectModel(QStringLiteral("gpt-6.1-sol"));
    check(selectedEfforts == QStringList({"low", "medium", "high", "xhigh", "ultra"}),
          "selecting Sol changed its available reasoning choices");
    controller.selectModel(QStringLiteral("gpt-5.6-luna"));
    check(selectedEfforts == QStringList({"low", "medium"}), "Sol choices leaked into another model");
}

void testExperimentalHandshakePrecedesDynamicTools() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(
        nullptr, [&process] {
            auto result = std::make_unique<FakeProcess>();
            process = result.get();
            return result;
        });

    QStringList models;
    QString error;
    bool connected = false;
    QObject::connect(
        &controller, &docxstudio::app::CodexController::modelsChanged,
        [&models](const QStringList& values) { models = values; });
    QObject::connect(
        &controller, &docxstudio::app::CodexController::statusChanged,
        [&connected](const bool active, const QString&) {
            if (active) connected = true;
        });
    QObject::connect(
        &controller, &docxstudio::app::CodexController::errorOccurred,
        [&error](const QString& value) { error = value; });

    controller.enable();
    check(process != nullptr, "controller did not create its process adapter");
    if (!process) return;
    check(process->spec.program == "codex", "controller launched the wrong program");
    check(process->spec.arguments ==
              std::vector<std::string>({"app-server", "--stdio"}),
          "controller launched the wrong app-server command");
    check(process->writes.size() == 1,
          "controller sent requests before the initialize handshake");

    const Json initialize = writtenMessage(*process, 0);
    check(initialize.value("method", std::string()) == "initialize",
          "first app-server message was not initialize");
    check(initialize.at("params").at("clientInfo").value("title", std::string()) == "OwlDocs" &&
              initialize.at("params").at("clientInfo").value("name", std::string()) == "owl_docs",
          "Codex client title is not OwlDocs or its stable integration identity changed");
    check(initialize.contains("params") &&
              initialize["params"].contains("capabilities") &&
              initialize["params"]["capabilities"].value(
                  "experimentalApi", false),
          "initialize did not opt into the experimental dynamic-tool API");
    check(!initialize["params"].contains("dynamicTools"),
          "dynamic tools were incorrectly sent in initialize");

    process->emitStdout(
        {{"id", initialize.at("id")},
         {"result", {{"userAgent", "codex-controller-test"},
                     {"platformFamily", "unix"},
                     {"platformOs", "linux"}}}});
    check(process->writes.size() == 3,
          "controller did not complete initialize then inspect effective config");
    check(writtenMessage(*process, 1).value("method", std::string()) ==
              "initialized",
          "initialized notification did not follow initialize");
    const Json configRead = writtenMessage(*process, 2);
    check(configRead.value("method", std::string()) == "config/read",
          "config/read did not follow the initialize handshake");
    check(configRead.at("params").value("includeLayers", true) == false,
          "config/read unnecessarily requested raw config layers");

    process->emitStdout(
        {{"id", configRead.at("id")},
         {"result",
          {{"config",
            {{"mcp_servers",
              {{"local_http", {{"enabled", true},
                                {"url", "http://127.0.0.1:65535/mcp"}}},
               {"writer_helper", {{"enabled", true},
                                   {"command", "synthetic-helper"}}}}}}},
           {"origins", Json::object()}}}});
    check(process->writes.size() == 4,
          "controller did not request account state after config isolation");
    const Json accountRead = writtenMessage(*process, 3);
    check(accountRead.value("method", std::string()) == "account/read",
          "account/read did not follow restricted config discovery");

    process->emitStdout(
        {{"id", accountRead.at("id")},
         {"result", {{"requiresOpenaiAuth", true},
                     {"account", {{"type", "chatgpt"},
                                  {"planType", "plus"}}}}}});
    check(process->writes.size() == 5,
          "controller did not discover models after managed account auth");
    const Json modelList = writtenMessage(*process, 4);
    check(modelList.value("method", std::string()) == "model/list",
          "model/list did not follow account/read");

    process->emitStdout(
        {{"id", modelList.at("id")},
         {"result",
          {{"data",
            {{{"id", "gpt-test"},
              {"model", "gpt-test"},
              {"displayName", "GPT Test"},
              {"description", "Synthetic controller test model"},
              {"hidden", false},
              {"isDefault", true},
              {"defaultReasoningEffort", "medium"},
              {"supportedReasoningEfforts",
               {{{"reasoningEffort", "medium"},
                 {"description", "Synthetic default"}}}},
              {"serviceTiers",
               {{{"id", "default"},
                 {"name", "Standard"},
                 {"description", "Synthetic standard tier"}}}}}}},
           {"nextCursor", nullptr}}}});
    check(error.isEmpty(), "controller reported an error during setup");
    check(connected, "controller did not become connected after model discovery");
    check(models == QStringList{QStringLiteral("gpt-test")},
          "controller did not publish the discovered model");

    controller.sendMessage(QStringLiteral("Improve this paragraph"),
                           QStringLiteral("gpt-test"),
                           QStringLiteral("medium"),
                           QStringLiteral("default"),
                           QStringLiteral("document:test"),
                           QStringLiteral("Selected text"),
                           QStringLiteral("Outline"));
    check(process->writes.size() == 6,
          "controller did not start a thread for the first document turn");
    const Json threadStart = writtenMessage(*process, 5);
    check(threadStart.value("method", std::string()) == "thread/start",
          "first document turn did not start a thread");
    check(threadStart.at("params").value("sandbox", std::string()) ==
              "read-only" &&
              threadStart.at("params").value("approvalPolicy", std::string()) ==
                  "never",
          "document thread did not retain its restricted legacy policy");
    check(threadStart.at("params").contains("dynamicTools"),
          "thread/start omitted the editor.v1 dynamic tools");
    const std::string instructions =
        threadStart.at("params").value("developerInstructions", std::string());
    check(instructions.find("editor_v1_capabilities") != std::string::npos &&
              instructions.find("no manual skill installation") != std::string::npos &&
              instructions.find("UTF-16") != std::string::npos &&
              instructions.find("current revision") != std::string::npos,
          "built-in document guidance omits discovery, units, revision safety or no-skill activation");
    check(instructions.find("Existing object IDs") != std::string::npos &&
              instructions.find("new stable IDs") != std::string::npos,
          "document guidance forbids declared stable IDs needed for compound object creation");
    check(instructions.find("set_text_style") != std::string::npos &&
              instructions.find("set_paragraph_style") != std::string::npos &&
              instructions.find("do not merely explain") != std::string::npos &&
              instructions.find("insert_excalidraw_figure") != std::string::npos &&
              instructions.find("replace_excalidraw_figure") != std::string::npos &&
              instructions.find("new figures and reconstructions") != std::string::npos,
          "document-mode instructions do not teach Codex to apply formatting");
    check(instructions.find("native patches retain source styling and background") != std::string::npos &&
              instructions.find("always render in Professional mode") == std::string::npos,
          "document guidance incorrectly restyles existing native figure patches");
    check(threadStart.at("params").contains("config"),
          "thread/start omitted the restricted Codex config");
    if (threadStart.at("params").contains("config")) {
        const Json& restricted = threadStart.at("params").at("config");
        check(restricted.contains("mcp_servers"),
              "restricted thread config omitted inherited MCP servers");
        if (restricted.contains("mcp_servers")) {
            const Json& servers = restricted.at("mcp_servers");
            check(servers.size() == 2,
                  "restricted thread config did not cover every inherited MCP server");
            check(servers.at("local_http") == Json({{"enabled", false}}),
                  "HTTP MCP server was not disabled without copying its URL");
            check(servers.at("writer_helper") == Json({{"enabled", false}}),
                  "stdio MCP server was not disabled without copying its command");
        }
    }
    if (threadStart.at("params").contains("dynamicTools")) {
        const Json& tools = threadStart.at("params").at("dynamicTools");
        check(tools.is_array() &&
                  tools.size() ==
                      docxstudio::codex::editorV1ToolDefinitions().size(),
              "thread/start registered an incomplete editor.v1 catalog");
        check(!tools.empty() &&
                  tools.front().value("name", std::string()) ==
                      docxstudio::codex::kEditorReadTool,
              "thread/start registered the wrong editor.v1 catalog");
        const auto preview = std::find_if(
            tools.begin(), tools.end(), [](const Json& tool) {
                return tool.value("name", std::string()) ==
                       docxstudio::codex::kEditorPreviewTool;
            });
        check(preview != tools.end() &&
                  preview->value("description", std::string()).find(
                      "set_text_style") != std::string::npos &&
                  preview->value("description", std::string()).find(
                      "set_paragraph_style") != std::string::npos,
              "preview tool description does not advertise formatting operations");
    }
    check(initialize.at("id").get<std::int64_t>() <
              threadStart.at("id").get<std::int64_t>(),
          "dynamic tools were registered before experimental capability negotiation");

    process->emitStdout(
        {{"id", threadStart.at("id")},
         {"result", {{"thread", {{"id", "thread-new"}}}}}});
    check(process->writes.size() == 7,
          "controller did not start a turn after creating the thread");
    const Json firstTurn = writtenMessage(*process, 6);
    check(firstTurn.value("method", std::string()) == "turn/start",
          "controller sent the wrong request after thread creation");
    process->emitStdout(
        {{"id", firstTurn.at("id")},
         {"result", {{"turn", {{"id", "turn-new"},
                                  {"status", "inProgress"},
                                  {"items", Json::array()}}}}}});
    process->emitStdout(
        {{"method", "turn/completed"},
         {"params", {{"threadId", "thread-new"},
                     {"turn", {{"id", "turn-new"},
                                {"status", "completed"}}}}}});

    controller.sendMessage(QStringLiteral("Now center it"),
                           QStringLiteral("gpt-test"),
                           QStringLiteral("medium"),
                           QStringLiteral("default"),
                           QStringLiteral("document:test"),
                           QStringLiteral("Selected text"),
                           QStringLiteral("Outline"));
    check(process->writes.size() == 8,
          "controller did not accept a second turn in the same conversation");
    const Json secondTurn = writtenMessage(*process, 7);
    check(secondTurn.value("method", std::string()) == "turn/start" &&
              secondTurn.at("params").value("threadId", std::string()) ==
                  "thread-new",
          "second message did not continue the existing document thread");
    process->emitStdout(
        {{"id", secondTurn.at("id")},
         {"result", {{"turn", {{"id", "turn-second"},
                                  {"status", "inProgress"},
                                  {"items", Json::array()}}}}}});
    process->emitStdout(
        {{"method", "turn/completed"},
         {"params", {{"threadId", "thread-new"},
                     {"turn", {{"id", "turn-second"},
                                {"status", "completed"}}}}}});

    controller.restoreDocumentThread(QStringLiteral("document:resume"),
                                     QStringLiteral("thread-saved"));
    controller.sendMessage(QStringLiteral("Continue editing"),
                           QStringLiteral("gpt-test"),
                           QStringLiteral("medium"),
                           QStringLiteral("default"),
                           QStringLiteral("document:resume"),
                           QStringLiteral("More text"),
                           QStringLiteral("More outline"));
    check(process->writes.size() == 9,
          "controller did not resume the saved document thread");
    const Json threadResume = writtenMessage(*process, 8);
    check(threadResume.value("method", std::string()) == "thread/resume",
          "saved document did not use thread/resume");
    check(threadResume.at("params").at("config") ==
              threadStart.at("params").at("config"),
          "thread/resume did not reapply inherited MCP isolation");
    check(threadResume.at("params").value("developerInstructions", std::string()) ==
              instructions,
          "thread/resume did not restore the formatting-capable instructions");
    controller.shutdown();
}

void testMalformedConfigFailsClosed() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(
        nullptr, [&process] {
            auto result = std::make_unique<FakeProcess>();
            process = result.get();
            return result;
        });

    QString error;
    QObject::connect(
        &controller, &docxstudio::app::CodexController::errorOccurred,
        [&error](const QString& value) { error = value; });

    controller.enable();
    check(process != nullptr, "controller did not create its process adapter");
    if (!process) return;
    const Json initialize = writtenMessage(*process, 0);
    process->emitStdout(
        {{"id", initialize.at("id")},
         {"result", {{"userAgent", "codex-controller-test"},
                     {"platformFamily", "unix"},
                     {"platformOs", "linux"}}}});
    check(process->writes.size() == 3,
          "controller did not request effective config after initialize");
    const Json configRead = writtenMessage(*process, 2);
    check(configRead.value("method", std::string()) == "config/read",
          "controller did not issue config/read before authentication");

    process->emitStdout(
        {{"id", configRead.at("id")},
         {"result", {{"config", "malformed"},
                     {"origins", Json::object()}}}});
    check(!error.isEmpty(),
          "malformed effective config did not produce a user-visible setup error");
    check(process->writes.size() == 3,
          "controller continued authentication after unsafe config discovery failed");
    controller.shutdown();
}

void testMissingMcpConfigFailsClosed() {
    FakeProcess* process = nullptr;
    docxstudio::app::CodexController controller(
        nullptr, [&process] {
            auto result = std::make_unique<FakeProcess>();
            process = result.get();
            return result;
        });

    QString error;
    QObject::connect(
        &controller, &docxstudio::app::CodexController::errorOccurred,
        [&error](const QString& value) { error = value; });

    controller.enable();
    check(process != nullptr, "controller did not create its process adapter");
    if (!process) return;
    const Json initialize = writtenMessage(*process, 0);
    process->emitStdout(
        {{"id", initialize.at("id")},
         {"result", {{"userAgent", "codex-controller-test"},
                     {"platformFamily", "unix"},
                     {"platformOs", "linux"}}}});
    const Json configRead = writtenMessage(*process, 2);
    process->emitStdout(
        {{"id", configRead.at("id")},
         {"result", {{"config", Json::object()},
                     {"origins", Json::object()}}}});
    check(!error.isEmpty(),
          "missing MCP configuration did not fail closed");
    check(process->writes.size() == 3,
          "controller authenticated after MCP configuration was absent");
    controller.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("disconnect-tool")) {
        testDisconnectDuringEditorTool();
        return failures ? 1 : 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("document-binding")) {
        testDuplicateSavedThreadUsesTheActiveDocument();
        return failures ? 1 : 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("guidance")) {
        testExperimentalHandshakePrecedesDynamicTools();
        return failures ? 1 : 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("completion")) {
        testCompletionStatusIsNotAssumedSuccessful();
        return failures ? 1 : 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("routing")) {
        testToolsBelongToTheActiveTurn();
        return failures ? 1 : 0;
    }
    testToolsBelongToTheActiveTurn();
    testDuplicateSavedThreadUsesTheActiveDocument();
    testCompletionStatusIsNotAssumedSuccessful();
    testDisconnectDuringEditorTool();
    testExperimentalHandshakePrecedesDynamicTools();
    testSolDefaultAndSupportedEfforts();
    testMalformedConfigFailsClosed();
    testMissingMcpConfigFailsClosed();
    if (failures != 0) {
        std::cerr << failures << " Codex controller test(s) failed\n";
        return 1;
    }
    std::cout << "All Codex controller tests passed\n";
    return 0;
}
