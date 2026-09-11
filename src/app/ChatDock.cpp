#include "docxstudio/app/ChatDock.h"

#include <QComboBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

#include <functional>

namespace docxstudio::app {
namespace {

class SendTextEdit final : public QPlainTextEdit {
public:
    std::function<void()> send;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !(event->modifiers() & Qt::ShiftModifier)) {
            if (send) {
                send();
            }
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

}  // namespace

ChatDock::ChatDock(QWidget* parent) : QDockWidget(tr("Codex"), parent) {
    setObjectName(QStringLiteral("codexDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(320);

    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    privacy_ = new QLabel(tr("Chat is off. The document stays local until you enable Codex."), body);
    privacy_->setWordWrap(true);
    privacy_->setStyleSheet(QStringLiteral("QLabel { background: #fff4ce; padding: 8px; border-radius: 4px; }"));
    layout->addWidget(privacy_);

    auto* connection = new QHBoxLayout;
    status_ = new QLabel(tr("Offline"), body);
    enable_ = new QPushButton(tr("Enable Codex…"), body);
    connect(enable_, &QPushButton::clicked, this, &ChatDock::enableRequested);
    connection->addWidget(status_);
    connection->addStretch(1);
    connection->addWidget(enable_);
    layout->addLayout(connection);

    auto* controls = new QHBoxLayout;
    models_ = new QComboBox(body);
    models_->setToolTip(tr("Model (discovered from Codex)"));
    efforts_ = new QComboBox(body);
    efforts_->setToolTip(tr("Reasoning effort"));
    tiers_ = new QComboBox(body);
    tiers_->setToolTip(tr("Speed / service tier"));
    controls->addWidget(models_, 2);
    controls->addWidget(efforts_, 1);
    controls->addWidget(tiers_, 1);
    layout->addLayout(controls);
    connect(models_, &QComboBox::currentTextChanged, this, &ChatDock::modelChanged);

    transcript_ = new QTextBrowser(body);
    transcript_->setOpenExternalLinks(false);
    transcript_->setPlaceholderText(tr("Codex conversation"));
    layout->addWidget(transcript_, 1);

    previewResponse_ = new QPushButton(tr("Preview last response in selection"), body);
    previewResponse_->setToolTip(tr("Creates an isolated editor preview; the live document is unchanged until you accept."));
    previewResponse_->hide();
    connect(previewResponse_, &QPushButton::clicked, this, [this] {
        if (!lastAssistant_.isEmpty()) emit previewLastResponseRequested(lastAssistant_);
    });
    layout->addWidget(previewResponse_);

    preview_ = new QWidget(body);
    preview_->setStyleSheet(QStringLiteral("QWidget { background: #eaf4ff; border-radius: 4px; }"));
    auto* previewLayout = new QVBoxLayout(preview_);
    previewSummary_ = new QLabel(preview_);
    previewSummary_->setWordWrap(true);
    auto* previewActions = new QHBoxLayout;
    auto* accept = new QPushButton(tr("Accept as one edit"), preview_);
    auto* reject = new QPushButton(tr("Discard"), preview_);
    connect(accept, &QPushButton::clicked, this, &ChatDock::acceptPreviewRequested);
    connect(reject, &QPushButton::clicked, this, &ChatDock::rejectPreviewRequested);
    previewActions->addWidget(accept);
    previewActions->addWidget(reject);
    previewLayout->addWidget(previewSummary_);
    previewLayout->addLayout(previewActions);
    preview_->hide();
    layout->addWidget(preview_);

    auto* typedInput = new SendTextEdit;
    typedInput->setParent(body);
    typedInput->setPlaceholderText(tr("Ask about or propose changes to this document…"));
    typedInput->setMaximumHeight(110);
    typedInput->send = [this] { sendCurrentMessage(); };
    input_ = typedInput;
    layout->addWidget(input_);

    send_ = new QPushButton(tr("Send"), body);
    send_->setDefault(true);
    connect(send_, &QPushButton::clicked, this, &ChatDock::sendCurrentMessage);
    layout->addWidget(send_);

    models_->setEnabled(false);
    efforts_->setEnabled(false);
    tiers_->setEnabled(false);
    input_->setEnabled(false);
    send_->setEnabled(false);
    setWidget(body);
}

void ChatDock::setConnected(bool connected, const QString& detail) {
    status_->setText(connected ? tr("Connected")
                               : detail.isEmpty() ? tr("Offline") : detail);
    if (!detail.isEmpty()) {
        status_->setToolTip(detail);
    }
    enable_->setVisible(!connected);
    models_->setEnabled(connected);
    efforts_->setEnabled(connected);
    tiers_->setEnabled(connected && tiers_->count() > 0);
    input_->setEnabled(connected);
    send_->setEnabled(connected);
    privacy_->setText(connected
                          ? tr("Codex is enabled. Requested document context may be processed by Codex/OpenAI.")
                          : tr("Chat is off. The document stays local until you enable Codex."));
}

void ChatDock::setBusy(bool busy) {
    send_->setEnabled(!busy && input_->isEnabled());
    input_->setEnabled(!busy && models_->isEnabled());
    if (!busy) {
        input_->setFocus();
    }
}

void ChatDock::setModels(const QStringList& models) {
    models_->clear();
    models_->addItems(models);
}

void ChatDock::setEfforts(const QStringList& efforts) {
    efforts_->clear();
    efforts_->addItem(tr("Default"), QString());
    for (const auto& effort : efforts) efforts_->addItem(effort, effort);
}

void ChatDock::setServiceTiers(const QStringList& tiers,
                               const QString& defaultTier,
                               const QString& warning) {
    tiers_->clear();
    for (const auto& tier : tiers) {
        const QString label = tier.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0 ||
                                      tier.compare(QStringLiteral("standard"), Qt::CaseInsensitive) == 0
                                  ? tr("Standard")
                              : tier.compare(QStringLiteral("fast"), Qt::CaseInsensitive) == 0
                                  ? tr("Fast")
                                  : tier;
        tiers_->addItem(label, tier);
    }

    auto tierIndex = [this](const QString& id) {
        for (int index = 0; index < tiers_->count(); ++index) {
            if (tiers_->itemData(index).toString().compare(id, Qt::CaseInsensitive) == 0) {
                return index;
            }
        }
        return -1;
    };
    int selected = -1;
    if (!defaultTier.isEmpty() &&
        defaultTier.compare(QStringLiteral("fast"), Qt::CaseInsensitive) != 0) {
        selected = tierIndex(defaultTier);
    }
    if (selected < 0) selected = tierIndex(QStringLiteral("default"));
    if (selected < 0) selected = tierIndex(QStringLiteral("standard"));
    if (selected < 0 && !tiers.isEmpty()) {
        // An empty value asks app-server to use its ordinary/default tier. It
        // also prevents an advertised Fast tier from becoming active merely
        // because it happened to be the first catalog entry.
        tiers_->insertItem(0, tr("Standard"), QString());
        selected = 0;
    }
    if (selected >= 0) tiers_->setCurrentIndex(selected);
    tiers_->setEnabled(models_->isEnabled() && tiers_->count() > 0);
    tiers_->setToolTip(warning.isEmpty() ? tr("Speed / service tier") : warning);
}

void ChatDock::clearConversation() {
    transcript_->clear();
    assistantBuffer_.clear();
    lastAssistant_.clear();
    previewResponse_->hide();
    hidePreview();
}

void ChatDock::appendStoredMessage(const QString& role, const QString& text) {
    if (role == QStringLiteral("user")) {
        appendHtmlMessage(tr("You"), text, QStringLiteral("#2457a6"));
    } else if (role == QStringLiteral("assistant")) {
        appendHtmlMessage(tr("Codex"), text, QStringLiteral("#166534"));
        lastAssistant_ = text;
        previewResponse_->setVisible(!lastAssistant_.trimmed().isEmpty());
    } else {
        appendHtmlMessage(tr("System"), text, QStringLiteral("#667085"));
    }
}

void ChatDock::appendHtmlMessage(const QString& role, const QString& text, const QString& color) {
    transcript_->append(QStringLiteral("<p><b style=\"color:%1\">%2</b><br>%3</p>")
                            .arg(color, role.toHtmlEscaped(), text.toHtmlEscaped().replace('\n', "<br>")));
}

void ChatDock::appendUserMessage(const QString& text) {
    appendHtmlMessage(tr("You"), text, QStringLiteral("#2457a6"));
}

void ChatDock::beginAssistantMessage() {
    assistantBuffer_.clear();
    QTextCursor cursor(transcript_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock();
    QTextCharFormat role;
    role.setFontWeight(QFont::Bold);
    role.setForeground(QColor(QStringLiteral("#166534")));
    cursor.setCharFormat(role);
    cursor.insertText(tr("Codex"));
    cursor.insertBlock();
    cursor.setCharFormat(QTextCharFormat());
    transcript_->setTextCursor(cursor);
}

void ChatDock::appendAssistantDelta(const QString& text) {
    assistantBuffer_ += text;
    QTextCursor cursor(transcript_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void ChatDock::finishAssistantMessage() {
    lastAssistant_ = assistantBuffer_;
    previewResponse_->setVisible(!lastAssistant_.trimmed().isEmpty());
    QTextCursor cursor(transcript_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock();
    transcript_->setTextCursor(cursor);
    assistantBuffer_.clear();
}

void ChatDock::showError(const QString& text) {
    appendHtmlMessage(tr("Error"), text, QStringLiteral("#b42318"));
}

void ChatDock::showPreview(const QString& label, const QString& summary) {
    previewSummary_->setText(QStringLiteral("<b>%1</b><br>%2")
                                 .arg(label.toHtmlEscaped(), summary.toHtmlEscaped()));
    preview_->show();
    previewResponse_->hide();
}

void ChatDock::hidePreview() {
    preview_->hide();
    previewResponse_->setVisible(!lastAssistant_.trimmed().isEmpty());
}

QString ChatDock::selectedModel() const { return models_->currentText(); }
QString ChatDock::selectedEffort() const {
    return efforts_->currentData().isValid() ? efforts_->currentData().toString()
                                              : efforts_->currentText();
}
QString ChatDock::selectedServiceTier() const {
    return tiers_->currentData().isValid() ? tiers_->currentData().toString()
                                           : tiers_->currentText();
}

void ChatDock::sendCurrentMessage() {
    const auto text = input_->toPlainText().trimmed();
    if (text.isEmpty() || !send_->isEnabled()) {
        return;
    }
    input_->clear();
    appendUserMessage(text);
    emit sendRequested(text, selectedModel(), selectedEffort(), selectedServiceTier());
}

}  // namespace docxstudio::app
