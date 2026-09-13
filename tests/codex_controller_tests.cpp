#include "docxstudio/app/CodexController.h"

#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/codex/transport.hpp"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

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
          "controller did not complete initialize then request account state");
    check(writtenMessage(*process, 1).value("method", std::string()) ==
              "initialized",
          "initialized notification did not follow initialize");
    const Json accountRead = writtenMessage(*process, 2);
    check(accountRead.value("method", std::string()) == "account/read",
          "account/read did not follow the initialize handshake");

    process->emitStdout(
        {{"id", accountRead.at("id")},
         {"result", {{"requiresOpenaiAuth", true},
                     {"account", {{"type", "chatgpt"},
                                  {"planType", "plus"}}}}}});
    check(process->writes.size() == 4,
          "controller did not discover models after managed account auth");
    const Json modelList = writtenMessage(*process, 3);
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
    check(process->writes.size() == 5,
          "controller did not start a thread for the first document turn");
    const Json threadStart = writtenMessage(*process, 4);
    check(threadStart.value("method", std::string()) == "thread/start",
          "first document turn did not start a thread");
    check(threadStart.at("params").value("sandbox", std::string()) ==
              "read-only" &&
              threadStart.at("params").value("approvalPolicy", std::string()) ==
                  "never",
          "document thread did not retain its restricted legacy policy");
    check(threadStart.at("params").contains("dynamicTools"),
          "thread/start omitted the editor.v1 dynamic tools");
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
    }
    check(initialize.at("id").get<std::int64_t>() <
              threadStart.at("id").get<std::int64_t>(),
          "dynamic tools were registered before experimental capability negotiation");
    controller.shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testExperimentalHandshakePrecedesDynamicTools();
    if (failures != 0) {
        std::cerr << failures << " Codex controller test(s) failed\n";
        return 1;
    }
    std::cout << "All Codex controller tests passed\n";
    return 0;
}
