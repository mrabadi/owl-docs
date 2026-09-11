#pragma once

#include <QDockWidget>
#include <QStringList>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

namespace docxstudio::app {

class ChatDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit ChatDock(QWidget* parent = nullptr);

    void setConnected(bool connected, const QString& detail = {});
    void setBusy(bool busy);
    void setModels(const QStringList& models);
    void setEfforts(const QStringList& efforts);
    void setServiceTiers(const QStringList& tiers,
                         const QString& defaultTier,
                         const QString& warning = {});
    void clearConversation();
    void appendStoredMessage(const QString& role, const QString& text);
    void appendUserMessage(const QString& text);
    void beginAssistantMessage();
    void appendAssistantDelta(const QString& text);
    void finishAssistantMessage();
    void showError(const QString& text);
    void showPreview(const QString& label, const QString& summary);
    void hidePreview();
    QString selectedModel() const;
    QString selectedEffort() const;
    QString selectedServiceTier() const;

signals:
    void enableRequested();
    void sendRequested(const QString& text, const QString& model,
                       const QString& effort, const QString& serviceTier);
    void acceptPreviewRequested();
    void rejectPreviewRequested();
    void modelChanged(const QString& model);
    void previewLastResponseRequested(const QString& response);

private:
    void sendCurrentMessage();
    void appendHtmlMessage(const QString& role, const QString& text, const QString& color);

    QLabel* privacy_{};
    QLabel* status_{};
    QPushButton* enable_{};
    QComboBox* models_{};
    QComboBox* efforts_{};
    QComboBox* tiers_{};
    QTextBrowser* transcript_{};
    QPlainTextEdit* input_{};
    QPushButton* send_{};
    QPushButton* previewResponse_{};
    QWidget* preview_{};
    QLabel* previewSummary_{};
    QString assistantBuffer_;
    QString lastAssistant_;
};

}  // namespace docxstudio::app
