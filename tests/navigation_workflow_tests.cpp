#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/NavigationDock.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QLineEdit>
#include <QListWidget>
#include <QKeyEvent>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

using docxstudio::app::DocumentCanvas;
using docxstudio::app::NavigationDock;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void processEventsFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QTabWidget* documentTabs(docxstudio::app::MainWindow& window) {
    for (auto* tabs : window.findChildren<QTabWidget*>()) {
        if (qobject_cast<DocumentCanvas*>(tabs->currentWidget())) return tabs;
    }
    return nullptr;
}

void testModelessNavigationWorkflow() {
    docxstudio::app::MainWindow window;
    window.resize(1100, 760);
    window.show();
    QApplication::processEvents();

    auto* tabs = documentTabs(window);
    auto* canvas = tabs
        ? qobject_cast<DocumentCanvas*>(tabs->currentWidget()) : nullptr;
    auto* dock = window.findChild<NavigationDock*>(
        QStringLiteral("navigationDock"));
    auto* find = window.findChild<QAction*>(QStringLiteral("edit.find"));
    auto* replace = window.findChild<QAction*>(QStringLiteral("edit.replace"));
    auto* findNext = window.findChild<QAction*>(
        QStringLiteral("edit.findNext"));
    auto* findPrevious = window.findChild<QAction*>(
        QStringLiteral("edit.findPrevious"));
    auto* toggle = window.findChild<QAction*>(
        QStringLiteral("view.navigation"));
    auto* newDocument = window.findChild<QAction*>(QStringLiteral("file.new"));
    check(tabs && canvas && dock && find && replace && findNext &&
              findPrevious && toggle && newDocument,
          "main window is missing navigation commands or controls");

    canvas->insertText(QStringLiteral("Alpha beta alpha"));
    check(canvas->insertTable(1, 1, false),
          "could not insert a table for mixed navigation search");
    const auto tableId = canvas->snapshot().document.tables().front().id();
    check(canvas->activateTableCell(tableId, 0, 0),
          "could not activate the navigation test cell");
    canvas->insertText(QStringLiteral("alpha table"));

    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_F, Qt::ControlModifier);
    QApplication::processEvents();
    auto* query = dock->findChild<QLineEdit*>(
        QStringLiteral("navigation.query"));
    auto* replacement = dock->findChild<QLineEdit*>(
        QStringLiteral("navigation.replacement"));
    auto* results = dock->findChild<QListWidget*>(
        QStringLiteral("navigation.results"));
    auto* replaceOne = dock->findChild<QPushButton*>(
        QStringLiteral("navigation.replace"));
    auto* replaceAll = dock->findChild<QPushButton*>(
        QStringLiteral("navigation.replaceAll"));
    auto* matchCase = dock->findChild<QCheckBox*>(
        QStringLiteral("navigation.matchCase"));
    check(query && replacement && results && replaceOne && replaceAll &&
              matchCase,
          "navigation dock is missing a workflow control");
    check(dock->isVisible() && toggle->isChecked() && query->hasFocus(),
          "Find did not open and focus the modeless navigation pane");

    query->setText(QStringLiteral("alpha"));
    QTest::keyClick(query, Qt::Key_Return);
    check(canvas->selectedText() == QStringLiteral("Alpha"),
          "Enter before the live-search debounce did not find from the caret");
    processEventsFor(160);
    check(results->count() == 3,
          "live search did not include body and table-cell matches");
    check(results->item(2)->text().startsWith(QStringLiteral("Table 1")),
          "table-cell result was not presented in body-block order");

    results->scrollToItem(results->item(2));
    QTest::mouseClick(results->viewport(), Qt::LeftButton,
                      Qt::NoModifier,
                      results->visualItemRect(results->item(2)).center());
    QApplication::processEvents();
    check(canvas->selectedText().compare(QStringLiteral("alpha"),
                                         Qt::CaseInsensitive) == 0,
          "selecting a result did not reveal and select table-cell text");
    results->setCurrentRow(1);
    check(canvas->selectedText().compare(QStringLiteral("alpha"),
                                         Qt::CaseInsensitive) == 0,
          "moving result focus unexpectedly activated another match");
    results->setFocus();
    QTest::keyClick(results, Qt::Key_Return);
    check(canvas->selectedText() == QStringLiteral("alpha"),
          "Enter on a result row did not activate the match");

    results->scrollToItem(results->item(2));
    QTest::mouseClick(results->viewport(), Qt::LeftButton,
                      Qt::NoModifier,
                      results->visualItemRect(results->item(2)).center());
    results->setFocus();
    QTest::keyClick(results, Qt::Key_F3);
    check(dock->currentResultIndex() == 0 &&
              canvas->selectedText() == QStringLiteral("Alpha"),
          "F3 from the navigation pane did not wrap to the first result");
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_Right);
    check(dock->currentResultIndex() == -1,
          "moving the caret off a match left stale navigation selection");
    QTest::keyClick(canvas, Qt::Key_F3);
    check(dock->currentResultIndex() == 1 &&
              canvas->selectedText() == QStringLiteral("alpha"),
          "F3 from the canvas ignored the live caret");
    QTest::keyClick(canvas, Qt::Key_F3, Qt::ShiftModifier);
    check(dock->currentResultIndex() == 0,
          "Shift+F3 did not return to the preceding result");
    results->setFocus();
    QTest::keyClick(results, Qt::Key_F3, Qt::ShiftModifier);
    check(dock->currentResultIndex() == 2,
          "Shift+F3 from the navigation pane did not wrap to the last result");

    QTest::keyClick(results, Qt::Key_H, Qt::ControlModifier);
    QApplication::processEvents();
    check(dock->mode() == NavigationDock::Mode::replace &&
              replacement->isVisible() && replacement->hasFocus() == false,
          "Replace did not switch the persistent pane into Replace mode");
    replacement->setText(QStringLiteral("omega"));
    const auto beforeOne = canvas->snapshot().revision;
    replaceOne->click();
    check(canvas->snapshot().revision.value() == beforeOne.value() + 1 &&
              canvas->searchHits(QStringLiteral("alpha")).size() == 2,
          "Replace did not apply exactly one table-cell match");

    const auto beforeAll = canvas->snapshot().revision;
    replaceAll->click();
    check(canvas->snapshot().revision.value() == beforeAll.value() + 1 &&
              canvas->searchHits(QStringLiteral("alpha")).empty(),
          "Replace All was not one transaction across remaining body matches");
    canvas->undo();
    check(canvas->searchHits(QStringLiteral("alpha")).size() == 2,
          "one Undo did not restore the complete Replace All transaction");
    canvas->undo();
    check(canvas->searchHits(QStringLiteral("alpha")).size() == 3,
          "second Undo did not independently restore Replace current");

    const auto live = canvas->snapshot();
    const auto& liveParagraph = live.document.paragraphs().front();
    const auto previewParagraph = docxstudio::core::NodeId::generate();
    QString previewSummary;
    QString previewError;
    check(canvas->createOperationsPreview(
              live.revision,
              {docxstudio::core::SplitParagraph{
                   {liveParagraph.id(), liveParagraph.text().size()},
                   previewParagraph},
               docxstudio::core::InsertText{
                   {previewParagraph, 0}, u"alpha preview", std::nullopt}},
              QStringLiteral("Navigation preview"), previewSummary,
              previewError),
          "could not create navigation preview fixture");
    QApplication::processEvents();
    check(results->count() == 4 &&
              !replaceOne->isEnabled() && !replaceAll->isEnabled() &&
              results->item(2)->text().startsWith(
                  QStringLiteral("Paragraph 2")) &&
              results->item(3)->text().startsWith(QStringLiteral("Table 1")),
          "opening a preview did not atomically refresh result rows and labels");
    canvas->discardPreview();
    QApplication::processEvents();
    check(results->count() == 3 &&
              replaceOne->isEnabled() && replaceAll->isEnabled() &&
              results->item(2)->text().startsWith(QStringLiteral("Table 1")),
          "discarding a preview did not restore live search results immediately");

    matchCase->setChecked(true);
    query->setText(QStringLiteral("Alpha"));
    processEventsFor(160);
    check(results->count() == 1,
          "case-sensitive live search returned the wrong count");

    newDocument->trigger();
    QApplication::processEvents();
    check(tabs->count() == 2 && dock->query().isEmpty() &&
              !dock->matchCase(),
          "new tab inherited another document's navigation query");
    dock->setQuery(QStringLiteral("second document"));
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    check(dock->query() == QStringLiteral("Alpha") && dock->matchCase() &&
              dock->mode() == NavigationDock::Mode::replace,
          "switching tabs did not restore per-document navigation state");

    query->setFocus();
    QTest::keyClick(query, Qt::Key_Escape);
    check(!dock->isVisible() && !toggle->isChecked(),
          "Escape from the navigation pane did not close and uncheck it");
    toggle->trigger();
    QApplication::processEvents();
    check(dock->isVisible() && toggle->isChecked() && query->hasFocus(),
          "Navigation toggle did not reopen the pane with keyboard focus");
}

void testReplacementAdvancesPastReplacementContainingQuery() {
    docxstudio::app::MainWindow window;
    window.resize(900, 640);
    window.show();
    QApplication::processEvents();

    auto* tabs = documentTabs(window);
    auto* canvas = tabs
        ? qobject_cast<DocumentCanvas*>(tabs->currentWidget()) : nullptr;
    auto* dock = window.findChild<NavigationDock*>(
        QStringLiteral("navigationDock"));
    auto* query = dock ? dock->findChild<QLineEdit*>(
        QStringLiteral("navigation.query")) : nullptr;
    auto* replacement = dock ? dock->findChild<QLineEdit*>(
        QStringLiteral("navigation.replacement")) : nullptr;
    auto* replaceOne = dock ? dock->findChild<QPushButton*>(
        QStringLiteral("navigation.replace")) : nullptr;
    check(canvas && dock && query && replacement && replaceOne,
          "replacement-advance fixture is missing navigation controls");

    canvas->insertText(QStringLiteral("cat cat"));
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_H, Qt::ControlModifier);
    query->setText(QStringLiteral("cat"));
    replacement->setText(QStringLiteral("cats"));
    processEventsFor(160);
    check(dock->resultCount() == 2,
          "replacement-advance fixture found the wrong initial matches");

    dock->setCurrentResultIndex(0);
    replaceOne->click();
    auto selection = canvas->selection();
    check(selection.anchor.utf16_offset == 5 &&
              selection.focus.utf16_offset == 8,
          "Replace reselected the query inside its own replacement");

    replaceOne->click();
    selection = canvas->selection();
    check(selection.anchor.utf16_offset == 0 &&
              selection.focus.utf16_offset == 3,
          "replacing the final match did not wrap to the first remaining match");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Owl Docs Navigation Workflow Tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settingsDirectory;
    check(settingsDirectory.isValid(),
          "could not create isolated navigation settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());

    testModelessNavigationWorkflow();
    testReplacementAdvancesPastReplacementContainingQuery();
    std::cout << "navigation workflow tests passed\n";
    return 0;
}
