#include "docxstudio/app/NavigationDock.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace docxstudio::app {
namespace {

class NavigationResultsList final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void(int)> activateCurrent;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return ||
             event->key() == Qt::Key_Enter) &&
            currentRow() >= 0) {
            if (activateCurrent) activateCurrent(currentRow());
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
};

}  // namespace

NavigationDock::NavigationDock(QWidget* parent)
    : QDockWidget(tr("Navigation"), parent) {
    setObjectName(QStringLiteral("navigationDock"));
    setAccessibleName(tr("Document navigation"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    setMinimumWidth(300);

    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("navigation.body"));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    modes_ = new QTabBar(body);
    modes_->setObjectName(QStringLiteral("navigation.mode"));
    modes_->setAccessibleName(tr("Navigation mode"));
    modes_->setExpanding(true);
    modes_->setDocumentMode(true);
    modes_->addTab(tr("Find"));
    modes_->addTab(tr("Replace"));
    modes_->setTabToolTip(0, tr("Find text in the document"));
    modes_->setTabToolTip(1, tr("Find and replace text in the document"));
    layout->addWidget(modes_);

    auto* queryRow = new QWidget(body);
    queryRow->setObjectName(QStringLiteral("navigation.queryRow"));
    auto* queryLayout = new QHBoxLayout(queryRow);
    queryLayout->setContentsMargins(0, 0, 0, 0);
    queryLayout->setSpacing(6);
    auto* queryLabel = new QLabel(tr("Find:"), queryRow);
    query_ = new QLineEdit(queryRow);
    query_->setObjectName(QStringLiteral("navigation.query"));
    query_->setAccessibleName(tr("Find text"));
    query_->setAccessibleDescription(
        tr("Text to find in the active document"));
    query_->setClearButtonEnabled(true);
    query_->setPlaceholderText(tr("Search document"));
    queryLabel->setBuddy(query_);
    queryLayout->addWidget(queryLabel);
    queryLayout->addWidget(query_, 1);
    layout->addWidget(queryRow);

    replacementRow_ = new QWidget(body);
    replacementRow_->setObjectName(QStringLiteral("navigation.replacementRow"));
    auto* replacementLayout = new QHBoxLayout(replacementRow_);
    replacementLayout->setContentsMargins(0, 0, 0, 0);
    replacementLayout->setSpacing(6);
    auto* replacementLabel = new QLabel(tr("Replace:"), replacementRow_);
    replacement_ = new QLineEdit(replacementRow_);
    replacement_->setObjectName(QStringLiteral("navigation.replacement"));
    replacement_->setAccessibleName(tr("Replacement text"));
    replacement_->setAccessibleDescription(
        tr("Text to insert in place of the current match"));
    replacement_->setClearButtonEnabled(true);
    replacementLabel->setBuddy(replacement_);
    replacementLayout->addWidget(replacementLabel);
    replacementLayout->addWidget(replacement_, 1);
    layout->addWidget(replacementRow_);

    auto* options = new QHBoxLayout;
    matchCase_ = new QCheckBox(tr("Match case"), body);
    matchCase_->setObjectName(QStringLiteral("navigation.matchCase"));
    matchCase_->setAccessibleDescription(
        tr("Find only text with the same uppercase and lowercase letters"));
    wholeWords_ = new QCheckBox(tr("Whole words"), body);
    wholeWords_->setObjectName(QStringLiteral("navigation.wholeWords"));
    wholeWords_->setAccessibleDescription(
        tr("Find only complete words"));
    options->addWidget(matchCase_);
    options->addWidget(wholeWords_);
    options->addStretch(1);
    layout->addLayout(options);

    auto* navigationActions = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous"), body);
    previous_->setObjectName(QStringLiteral("navigation.previous"));
    previous_->setAccessibleName(tr("Previous result"));
    previous_->setToolTip(tr("Go to the previous result (Shift+F3)"));
    next_ = new QPushButton(tr("Next"), body);
    next_->setObjectName(QStringLiteral("navigation.next"));
    next_->setAccessibleName(tr("Next result"));
    next_->setToolTip(tr("Go to the next result (F3)"));
    navigationActions->addWidget(previous_);
    navigationActions->addWidget(next_);
    layout->addLayout(navigationActions);

    resultCount_ = new QLabel(body);
    resultCount_->setObjectName(QStringLiteral("navigation.resultCount"));
    resultCount_->setAccessibleName(tr("Search result count"));
    resultCount_->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(resultCount_);

    auto* navigationResults = new NavigationResultsList(body);
    results_ = navigationResults;
    results_->setObjectName(QStringLiteral("navigation.results"));
    results_->setAccessibleName(tr("Search results"));
    results_->setAccessibleDescription(
        tr("Matches in document order; press Enter to go to a result"));
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setWordWrap(true);
    layout->addWidget(results_, 1);

    replaceActions_ = new QWidget(body);
    replaceActions_->setObjectName(QStringLiteral("navigation.replaceActions"));
    auto* replaceLayout = new QHBoxLayout(replaceActions_);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(6);
    replace_ = new QPushButton(tr("Replace"), replaceActions_);
    replace_->setObjectName(QStringLiteral("navigation.replace"));
    replace_->setAccessibleName(tr("Replace current result"));
    replace_->setToolTip(tr("Replace the selected search result"));
    replaceAll_ = new QPushButton(tr("Replace All"), replaceActions_);
    replaceAll_->setObjectName(QStringLiteral("navigation.replaceAll"));
    replaceAll_->setAccessibleName(tr("Replace all results"));
    replaceAll_->setToolTip(
        tr("Replace every matching result as one undoable edit"));
    replaceLayout->addWidget(replace_);
    replaceLayout->addWidget(replaceAll_);
    layout->addWidget(replaceActions_);

    setWidget(body);

    connect(modes_, &QTabBar::currentChanged, this, [this](int index) {
        const Mode requested = index == 1 ? Mode::replace : Mode::find;
        if (mode_ == requested) return;
        mode_ = requested;
        updateModePresentation();
        emit modeChanged(mode_);
    });
    connect(query_, &QLineEdit::textChanged, this,
            [this] { emitQueryChanged(); });
    connect(replacement_, &QLineEdit::textChanged, this,
            &NavigationDock::replacementChanged);
    connect(matchCase_, &QCheckBox::toggled, this,
            [this] { emitQueryChanged(); });
    connect(wholeWords_, &QCheckBox::toggled, this,
            [this] { emitQueryChanged(); });
    connect(query_, &QLineEdit::returnPressed, this, [this] {
        // The live result list is debounced. Enter must still search from the
        // caret immediately when pressed before that timer has populated rows.
        if (!query().isEmpty()) {
            emit nextRequested(query(), matchCase(), wholeWords());
        }
    });
    connect(previous_, &QPushButton::clicked, this, [this] {
        emit previousRequested(query(), matchCase(), wholeWords());
    });
    connect(next_, &QPushButton::clicked, this, [this] {
        emit nextRequested(query(), matchCase(), wholeWords());
    });
    connect(results_, &QListWidget::currentRowChanged, this,
            [this] { updateResultPresentation(); });
    connect(results_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) {
        const int index = results_->row(item);
        if (index >= 0) emit resultActivated(index);
    });
    navigationResults->activateCurrent = [this](int index) {
        emit resultActivated(index);
    };
    connect(replace_, &QPushButton::clicked, this, [this] {
        emit replaceRequested(query(), replacement(), matchCase(),
                              wholeWords());
    });
    connect(replaceAll_, &QPushButton::clicked, this, [this] {
        emit replaceAllRequested(query(), replacement(), matchCase(),
                                 wholeWords());
    });

    auto* dismissShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), body);
    dismissShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(dismissShortcut, &QShortcut::activated, this, [this] {
        emit dismissRequested();
        hide();
    });

    updateModePresentation();
    updateResultPresentation();
}

QString NavigationDock::query() const {
    return query_->text();
}

QString NavigationDock::replacement() const {
    return replacement_->text();
}

bool NavigationDock::matchCase() const noexcept {
    return matchCase_->isChecked();
}

bool NavigationDock::wholeWords() const noexcept {
    return wholeWords_->isChecked();
}

int NavigationDock::resultCount() const noexcept {
    return results_->count();
}

int NavigationDock::currentResultIndex() const noexcept {
    return results_->currentRow();
}

void NavigationDock::setMode(Mode mode) {
    const int index = mode == Mode::replace ? 1 : 0;
    if (mode_ == mode && modes_->currentIndex() == index) {
        updateModePresentation();
        return;
    }
    // QTabBar::setCurrentIndex emits currentChanged and centralizes the public
    // modeChanged notification in the connection above.
    modes_->setCurrentIndex(index);
}

void NavigationDock::setQuery(const QString& queryValue) {
    query_->setText(queryValue);
}

void NavigationDock::setReplacement(const QString& replacementValue) {
    replacement_->setText(replacementValue);
}

void NavigationDock::setMatchCase(bool enabled) {
    matchCase_->setChecked(enabled);
}

void NavigationDock::setWholeWords(bool enabled) {
    wholeWords_->setChecked(enabled);
}

void NavigationDock::setReplacementLocked(bool locked) {
    if (replacementLocked_ == locked) return;
    replacementLocked_ = locked;
    updateActionAvailability();
}

void NavigationDock::setSearchResults(
    const QList<NavigationResult>& searchResults, int currentIndex) {
    const QSignalBlocker blocker(results_);
    results_->clear();
    for (const auto& result : searchResults) {
        QString rowText = result.title;
        if (!result.snippet.isEmpty()) {
            if (!rowText.isEmpty()) rowText += QLatin1Char('\n');
            rowText += result.snippet;
        }
        auto* item = new QListWidgetItem(rowText, results_);
        item->setToolTip(result.snippet);
        const QString accessible = result.accessibleText.isEmpty()
            ? rowText : result.accessibleText;
        item->setData(Qt::AccessibleTextRole, accessible);
        if (!result.snippet.isEmpty()) {
            item->setData(Qt::AccessibleDescriptionRole, result.snippet);
        }
    }
    if (currentIndex >= 0 && currentIndex < results_->count()) {
        results_->setCurrentRow(currentIndex);
    } else {
        results_->setCurrentRow(-1);
    }
    updateResultPresentation();
}

void NavigationDock::setCurrentResultIndex(int index) {
    if (index < 0 || index >= results_->count()) index = -1;
    results_->setCurrentRow(index);
    if (index >= 0) results_->scrollToItem(results_->item(index));
}

void NavigationDock::clearSearchResults() {
    setSearchResults({});
}

void NavigationDock::focusQuery(bool selectAll) {
    show();
    raise();
    query_->setFocus(Qt::ShortcutFocusReason);
    if (selectAll) query_->selectAll();
}

void NavigationDock::emitQueryChanged() {
    // Do not leave stale matches actionable while the consumer computes the
    // result set for the new query/options.
    clearSearchResults();
    emit queryChanged(query(), matchCase(), wholeWords());
}

void NavigationDock::updateModePresentation() {
    const bool replacing = mode_ == Mode::replace;
    replacementRow_->setVisible(replacing);
    replaceActions_->setVisible(replacing);
    updateActionAvailability();
}

void NavigationDock::updateResultPresentation() {
    const int count = results_->count();
    const int current = results_->currentRow();
    if (query().isEmpty()) {
        resultCount_->setText(tr("Type to search"));
    } else if (count == 0) {
        resultCount_->setText(tr("No results"));
    } else if (current >= 0) {
        resultCount_->setText(
            tr("Result %1 of %2").arg(current + 1).arg(count));
    } else {
        resultCount_->setText(tr("%n result(s)", nullptr, count));
    }
    resultCount_->setAccessibleName(
        tr("Search status: %1").arg(resultCount_->text()));
    resultCount_->setAccessibleDescription(resultCount_->text());
    updateActionAvailability();
}

void NavigationDock::updateActionAvailability() {
    const bool hasQuery = !query().isEmpty();
    const bool hasResults = hasQuery && results_->count() > 0;
    previous_->setEnabled(hasResults);
    next_->setEnabled(hasResults);
    replace_->setEnabled(!replacementLocked_ &&
                         mode_ == Mode::replace && hasResults &&
                         results_->currentRow() >= 0);
    replaceAll_->setEnabled(!replacementLocked_ &&
                            mode_ == Mode::replace && hasResults);
}

}  // namespace docxstudio::app
