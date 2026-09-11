#pragma once

#include "docxstudio/codex/client.hpp"

#include <QObject>
#include <QStringList>

#include <memory>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QTemporaryDir;

namespace docxstudio::app {

class CodexController final : public QObject {
    Q_OBJECT

public:
    using EditorToolHandler = std::function<codex::Json(
        const QString& documentKey,
        const QString& tool,
        const codex::Json& arguments,
        QString& error)>;

    explicit CodexController(QObject* parent = nullptr);
    ~CodexController() override;

    void enable();
    void shutdown();
    bool connected() const noexcept { return connected_; }
    void selectModel(const QString& wireModel);
    void setEditorToolHandler(EditorToolHandler handler);
    void restoreDocumentThread(const QString& documentKey,
                               const QString& threadId);
    void sendMessage(const QString& text,
                     const QString& model,
                     const QString& effort,
                     const QString& serviceTier,
                     const QString& documentKey,
                     const QString& selection,
                     const QString& outline);

signals:
    void statusChanged(bool connected, const QString& detail);
    void modelsChanged(const QStringList& models);
    void effortsChanged(const QStringList& efforts);
    void serviceTiersChanged(const QStringList& tiers,
                             const QString& defaultTier,
                             const QString& warning);
    void loginStarted(const QString& url);
    void assistantStarted(const QString& documentKey);
    void assistantDelta(const QString& documentKey, const QString& delta);
    void assistantFinished(const QString& documentKey);
    void busyChanged(bool busy);
    void errorOccurred(const QString& message);
    void threadCreated(const QString& documentKey, const QString& threadId);

private:
    void afterInitialize();
    void beginLogin();
    void loadCatalog();
    void loadCatalogPage(std::optional<std::string> cursor);
    void startThreadAndTurn(const QString& documentKey,
                            const QString& prompt,
                            const QString& model,
                            const QString& effort,
                            const QString& tier);
    void resumeThreadAndTurn(const QString& documentKey,
                             const QString& threadId,
                             const QString& prompt,
                             const QString& model,
                             const QString& effort,
                             const QString& tier);
    void startTurn(const QString& documentKey,
                   const QString& threadId,
                   const QString& prompt,
                   const QString& model,
                   const QString& effort,
                   const QString& tier);
    void handleEvent(const codex::ServerEvent& event);
    void failTurn(const QString& message);
    QString resultError(const codex::RpcError& error) const;

    std::shared_ptr<codex::MessageTransport> transport_;
    std::unique_ptr<codex::Client> client_;
    std::unique_ptr<QTemporaryDir> documentSandbox_;
    std::vector<codex::ModelInfo> models_;
    EditorToolHandler editorToolHandler_;
    std::unordered_map<std::string, std::string> threadIds_;
    std::unordered_set<std::string> threadsNeedingResume_;
    QString turnDocumentKey_;
    QString turnThreadId_;
    QString loginId_;
    bool connected_{false};
    bool enabling_{false};
    bool busy_{false};
    bool assistantActive_{false};
    bool stopping_{false};
};

}  // namespace docxstudio::app
