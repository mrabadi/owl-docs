#include "docxstudio/app/NavigationDock.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QPushButton>
#include <QTabBar>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {

using docxstudio::app::NavigationDock;
using docxstudio::app::NavigationResult;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);

    NavigationDock dock;
    dock.resize(360, 600);
    dock.show();
    QApplication::processEvents();

    auto* modes = dock.findChild<QTabBar*>(QStringLiteral("navigation.mode"));
    auto* query = dock.findChild<QLineEdit*>(QStringLiteral("navigation.query"));
    auto* replacement = dock.findChild<QLineEdit*>(
        QStringLiteral("navigation.replacement"));
    auto* replacementRow = dock.findChild<QWidget*>(
        QStringLiteral("navigation.replacementRow"));
    auto* matchCase = dock.findChild<QCheckBox*>(
        QStringLiteral("navigation.matchCase"));
    auto* wholeWords = dock.findChild<QCheckBox*>(
        QStringLiteral("navigation.wholeWords"));
    auto* previous = dock.findChild<QPushButton*>(
        QStringLiteral("navigation.previous"));
    auto* next = dock.findChild<QPushButton*>(QStringLiteral("navigation.next"));
    auto* count = dock.findChild<QLabel*>(
        QStringLiteral("navigation.resultCount"));
    auto* results = dock.findChild<QListWidget*>(
        QStringLiteral("navigation.results"));
    auto* replace = dock.findChild<QPushButton*>(
        QStringLiteral("navigation.replace"));
    auto* replaceAll = dock.findChild<QPushButton*>(
        QStringLiteral("navigation.replaceAll"));
    auto* replaceActions = dock.findChild<QWidget*>(
        QStringLiteral("navigation.replaceActions"));
    check(modes && query && replacement && replacementRow && matchCase &&
              wholeWords && previous && next && count && results && replace &&
              replaceAll && replaceActions,
          "navigation dock is missing a required control");
    check(!dock.accessibleName().isEmpty() &&
              !modes->accessibleName().isEmpty() &&
              !query->accessibleName().isEmpty() &&
              !replacement->accessibleName().isEmpty() &&
              !previous->accessibleName().isEmpty() &&
              !next->accessibleName().isEmpty() &&
              !results->accessibleName().isEmpty() &&
              !replace->accessibleName().isEmpty() &&
              !replaceAll->accessibleName().isEmpty(),
          "navigation controls are missing accessible names");
    check(dock.mode() == NavigationDock::Mode::find &&
              !replacementRow->isVisible() && !replaceActions->isVisible(),
          "navigation dock did not start in Find mode");

    int queryChanges = 0;
    QString lastQuery;
    bool lastCase = false;
    bool lastWholeWords = false;
    QObject::connect(
        &dock, &NavigationDock::queryChanged,
        [&](const QString& value, bool caseSensitive, bool whole) {
            ++queryChanges;
            lastQuery = value;
            lastCase = caseSensitive;
            lastWholeWords = whole;
        });
    query->setText(QStringLiteral("owl"));
    matchCase->setChecked(true);
    wholeWords->setChecked(true);
    check(queryChanges == 3 && lastQuery == QStringLiteral("owl") &&
              lastCase && lastWholeWords,
          "live query/options did not emit a complete search request");
    dock.setMatchCase(false);
    dock.setWholeWords(false);
    check(!dock.matchCase() && !dock.wholeWords(),
          "public option setters did not restore navigation state");
    dock.setMatchCase(true);
    dock.setWholeWords(true);
    check(count->text() == QStringLiteral("No results") &&
              !next->isEnabled() && !previous->isEnabled(),
          "stale search actions remained enabled after changing the query");

    dock.setSearchResults(
        {{QStringLiteral("Page 1"), QStringLiteral("An owl appears"),
          QStringLiteral("First owl result")},
         {QStringLiteral("Table 1, cell 2"), QStringLiteral("Owl value"),
          QStringLiteral("Second owl result")}},
        0);
    check(dock.resultCount() == 2 && dock.currentResultIndex() == 0 &&
              count->text() == QStringLiteral("Result 1 of 2") &&
              next->isEnabled() && previous->isEnabled(),
          "search result count/current row was not presented correctly");
    check(results->item(0)->data(Qt::AccessibleTextRole).toString() ==
              QStringLiteral("First owl result"),
          "result-specific accessible text was lost");

    int nextRequests = 0;
    int previousRequests = 0;
    QObject::connect(&dock, &NavigationDock::nextRequested,
                     [&](const QString& value, bool caseSensitive, bool whole) {
        check(value == QStringLiteral("owl") && caseSensitive && whole,
              "Next omitted active search parameters");
        ++nextRequests;
    });
    QObject::connect(
        &dock, &NavigationDock::previousRequested,
        [&](const QString& value, bool caseSensitive, bool whole) {
            check(value == QStringLiteral("owl") && caseSensitive && whole,
                  "Previous omitted active search parameters");
            ++previousRequests;
        });
    next->click();
    previous->click();
    check(nextRequests == 1 && previousRequests == 1,
          "result navigation buttons did not emit requests");

    int activated = -1;
    int activationCount = 0;
    QObject::connect(&dock, &NavigationDock::resultActivated,
                     [&](int index) {
        activated = index;
        ++activationCount;
    });
    results->setCurrentRow(1);
    check(activationCount == 0,
          "changing or focusing a result row navigated before activation");
    emit results->itemClicked(results->item(1));
    check(activated == 1 && activationCount == 1 &&
              count->text() == QStringLiteral("Result 2 of 2"),
          "clicking a result did not emit exactly one latest row index");
    QKeyEvent activateKey(QEvent::KeyPress, Qt::Key_Return,
                          Qt::NoModifier);
    QApplication::sendEvent(results, &activateKey);
    check(activateKey.isAccepted() && activationCount == 2,
          "Enter did not activate the selected result exactly once");
    check(count->accessibleName().contains(QStringLiteral("Result 2 of 2")),
          "search status accessibility did not expose its live value");

    int modeChanges = 0;
    QObject::connect(&dock, &NavigationDock::modeChanged,
                     [&](NavigationDock::Mode mode) {
        check(mode == NavigationDock::Mode::replace,
              "mode signal reported the wrong mode");
        ++modeChanges;
    });
    dock.setMode(NavigationDock::Mode::replace);
    QApplication::processEvents();
    check(modeChanges == 1 && dock.mode() == NavigationDock::Mode::replace &&
              replacementRow->isVisible() && replaceActions->isVisible() &&
              replace->isEnabled() && replaceAll->isEnabled(),
          "Replace mode did not expose or enable replacement controls");

    int replacementChanges = 0;
    int replaceRequests = 0;
    int replaceAllRequests = 0;
    QObject::connect(&dock, &NavigationDock::replacementChanged,
                     [&](const QString& value) {
        check(value == QStringLiteral("bird"),
              "replacement signal reported the wrong value");
        ++replacementChanges;
    });
    QObject::connect(
        &dock, &NavigationDock::replaceRequested,
        [&](const QString& value, const QString& replacementValue,
            bool caseSensitive, bool whole) {
            check(value == QStringLiteral("owl") &&
                      replacementValue == QStringLiteral("bird") &&
                      caseSensitive && whole,
                  "Replace omitted active fields or options");
            ++replaceRequests;
        });
    QObject::connect(
        &dock, &NavigationDock::replaceAllRequested,
        [&](const QString& value, const QString& replacementValue,
            bool caseSensitive, bool whole) {
            check(value == QStringLiteral("owl") &&
                      replacementValue == QStringLiteral("bird") &&
                      caseSensitive && whole,
                  "Replace All omitted active fields or options");
            ++replaceAllRequests;
        });
    replacement->setText(QStringLiteral("bird"));
    replace->click();
    replaceAll->click();
    check(replacementChanges == 1 && replaceRequests == 1 &&
              replaceAllRequests == 1,
          "replacement controls did not emit their requested actions");

    dock.clearSearchResults();
    check(dock.resultCount() == 0 && !replace->isEnabled() &&
              !replaceAll->isEnabled() && !next->isEnabled(),
          "clearing results left an action enabled");
    dock.setQuery({});
    check(count->text() == QStringLiteral("Type to search"),
          "empty query did not present an idle result status");

    dock.setQuery(QStringLiteral("focus me"));
    dock.focusQuery();
    check(query->hasFocus() && query->selectedText() == query->text(),
          "focusQuery did not focus and select the query field");

    std::cout << "navigation dock tests passed\n";
    return 0;
}
