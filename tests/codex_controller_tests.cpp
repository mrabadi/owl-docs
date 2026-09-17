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
    check(instructions.find("set_text_style") != std::string::npos &&
              instructions.find("set_paragraph_style") != std::string::npos &&
              instructions.find("do not merely explain") != std::string::npos &&
              instructions.find("insert_excalidraw_figure") != std::string::npos &&
              instructions.find("replace_excalidraw_figure") != std::string::npos &&
              instructions.find("always render in Professional mode") != std::string::npos,
          "document-mode instructions do not teach Codex to apply formatting");
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
    testExperimentalHandshakePrecedesDynamicTools();
    testMalformedConfigFailsClosed();
    testMissingMcpConfigFailsClosed();
    if (failures != 0) {
        std::cerr << failures << " Codex controller test(s) failed\n";
        return 1;
    }
    std::cout << "All Codex controller tests passed\n";
    return 0;
}
