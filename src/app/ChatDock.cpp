#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/AppTheme.h"

#include <QComboBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QToolButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionComboBox>

#include <functional>

namespace docxstudio::app {
namespace {

void fitChoiceLabels(QComboBox* choice) {
    QStyleOptionComboBox option;
    option.initFrom(choice);
    option.rect = QRect(0, 0, 200, choice->sizeHint().height());
    const int chrome = option.rect.width() - choice->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, choice).width();
    int textWidth = 0;
    for (int index = 0; index < choice->count(); ++index)
        textWidth = qMax(textWidth, choice->fontMetrics().horizontalAdvance(choice->itemText(index)));
    choice->setMinimumWidth(textWidth + chrome);
}

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
    applyAppTheme(this);
    setObjectName(QStringLiteral("codexDock"));
    setAccessibleName(tr("Codex document assistant"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(320);

    auto* body = new QWidget(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    privacy_ = new QLabel(tr("Document context is shared when you send a message."), body);
    privacy_->setObjectName(QStringLiteral("codex.privacy"));
    privacy_->setProperty("tone", QStringLiteral("info"));
    privacy_->setWordWrap(true);
    layout->addWidget(privacy_);

    auto* connection = new QHBoxLayout;
    status_ = new QLabel(tr("Offline"), body);
    enable_ = new QPushButton(tr("Reconnect"), body);
    enable_->setObjectName(QStringLiteral("codex.connection"));
    connect(enable_, &QPushButton::clicked, this, [this] {
        if (connected_) emit disconnectRequested();
        else emit enableRequested();
    });
    connection->addWidget(status_);
    connection->addStretch(1);
    connection->addWidget(enable_);
    layout->addLayout(connection);

    auto* settings = new QWidget(body);
    auto* controls = new QGridLayout(settings);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setHorizontalSpacing(6);
    controls->setVerticalSpacing(3);
    models_ = new QComboBox(settings);
    models_->setObjectName(QStringLiteral("codex.model"));
    models_->setAccessibleName(tr("Codex model"));
    models_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    models_->setMinimumContentsLength(8);
    models_->setToolTip(tr("Model (discovered from Codex)"));
    efforts_ = new QComboBox(settings);
    efforts_->setObjectName(QStringLiteral("codex.effort"));
    efforts_->setAccessibleName(tr("Reasoning effort"));
    efforts_->setToolTip(tr("Reasoning effort"));
    tiers_ = new QComboBox(settings);
    tiers_->setObjectName(QStringLiteral("codex.speed"));
    tiers_->setAccessibleName(tr("Response speed"));
    tiers_->setToolTip(tr("Speed / service tier"));
    controls->addWidget(new QLabel(tr("Model"), settings), 0, 0);
    controls->addWidget(new QLabel(tr("Reasoning"), settings), 0, 1);
    controls->addWidget(new QLabel(tr("Speed"), settings), 0, 2);
    controls->addWidget(models_, 1, 0);
    controls->addWidget(efforts_, 1, 1);
    controls->addWidget(tiers_, 1, 2);
    controls->setColumnStretch(0, 2);
    controls->setColumnStretch(1, 1);
    controls->setColumnStretch(2, 1);
    layout->addWidget(settings);
    connect(models_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) emit modelChanged(selectedModel());
    });

    transcript_ = new QTextBrowser(body);
    transcript_->setObjectName(QStringLiteral("codex.transcript"));
    transcript_->setAccessibleName(tr("Codex conversation"));
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
    preview_->setProperty("tone", QStringLiteral("info"));
    auto* previewLayout = new QVBoxLayout(preview_);
    previewSummary_ = new QLabel(preview_);
    previewSummary_->setWordWrap(true);
    auto* previewActions = new QHBoxLayout;
    auto* accept = new QPushButton(tr("Accept as one edit"), preview_);
    accept->setProperty("role", QStringLiteral("primary"));
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
    input_->setObjectName(QStringLiteral("codex.input"));
    input_->setAccessibleName(tr("Message to Codex"));
    input_->setAccessibleDescription(tr("Enter sends your message; Shift+Enter starts a new line"));
    layout->addWidget(input_);

    send_ = new QPushButton(tr("Send"), body);
    send_->setObjectName(QStringLiteral("codex.send"));
    send_->setProperty("role", QStringLiteral("primary"));
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
    connected_ = connected;
    status_->setText(connected ? tr("Connected")
                               : detail.isEmpty() ? tr("Offline") : detail);
    if (!detail.isEmpty()) {
        status_->setToolTip(detail);
    }
    enable_->setText(connected ? tr("Disconnect") : tr("Reconnect"));
    enable_->setAccessibleName(enable_->text());
    updateInteractionState();
}

void ChatDock::setBusy(bool busy) {
    busy_ = busy;
    updateInteractionState();
    if (!busy_ && connected_) {
        input_->setFocus();
    }
}

void ChatDock::updateInteractionState() {
    models_->setEnabled(connected_);
    efforts_->setEnabled(connected_);
    tiers_->setEnabled(connected_ && tiers_->count() > 0);
    input_->setEnabled(connected_ && !busy_);
    send_->setEnabled(connected_ && !busy_);
}

void ChatDock::setModels(const QStringList& models) {
    const QSignalBlocker blocker(models_);
    models_->clear();
    for (const auto& model : models)
        models_->addItem(model == QStringLiteral("gpt-6.1-sol") ? tr("GPT-6.1 Sol") : model, model);
    const int preferred = models_->findData(QStringLiteral("gpt-6.1-sol"));
    if (preferred >= 0) models_->setCurrentIndex(preferred);
}

void ChatDock::setEfforts(const QStringList& efforts) {
    efforts_->clear();
    if (efforts.isEmpty()) efforts_->addItem(tr("Default"), QString());
    for (const auto& effort : efforts) {
        const QString label = effort == QStringLiteral("low") ? tr("Light")
            : effort == QStringLiteral("medium") ? tr("Medium")
            : effort == QStringLiteral("high") ? tr("High")
            : effort == QStringLiteral("xhigh") ? tr("XHigh")
            : effort == QStringLiteral("ultra") ? tr("Ultra") : effort;
        efforts_->addItem(label, effort);
    }
    const int preferred = efforts_->findData(QStringLiteral("low"));
    if (preferred >= 0) efforts_->setCurrentIndex(preferred);
    fitChoiceLabels(efforts_);
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
    int selected = tierIndex(QStringLiteral("default"));
    if (selected < 0) selected = tierIndex(QStringLiteral("standard"));
    if (selected < 0 && !defaultTier.isEmpty() &&
        defaultTier.compare(QStringLiteral("fast"), Qt::CaseInsensitive) != 0) {
        selected = tierIndex(defaultTier);
    }
    if (selected < 0 && !tiers.isEmpty()) {
        // An empty value asks app-server to use its ordinary/default tier. It
        // also prevents an advertised Fast tier from becoming active merely
        // because it happened to be the first catalog entry.
        tiers_->insertItem(0, tr("Standard"), QString());
        selected = 0;
    }
    if (selected >= 0) tiers_->setCurrentIndex(selected);
    tiers_->setEnabled(connected_ && tiers_->count() > 0);
    tiers_->setToolTip(warning.isEmpty() ? tr("Speed / service tier") : warning);
    fitChoiceLabels(tiers_);
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

QString ChatDock::selectedModel() const { return models_->currentData().toString(); }
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
