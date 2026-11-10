#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/AppTheme.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RibbonWidget.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QSettings>
#include <QTest>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void embeddedRibbon() {
    docxstudio::app::MainWindow window;
    window.resize(1280, 860);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* ribbon = window.findChild<docxstudio::app::RibbonWidget*>();
    auto* insert = window.findChild<QAction*>(QStringLiteral("insert.headerFooter"));
    check(canvas && ribbon && insert, "header/footer ribbon fixture");
    const auto before = canvas->snapshot();
    insert->trigger();
    QApplication::processEvents();
    auto* tools = window.findChild<QWidget*>(QStringLiteral("story.tools"));
    auto* editor = canvas->findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
    check(tools && tools->isVisible() && ribbon->isAncestorOf(tools),
          "Header and Footer controls float over the document instead of appearing in the contextual ribbon");
    check(tools->isEnabled(), "embedded header/footer controls are disabled with the body formatting bar");
    auto* home = ribbon->findChild<QWidget*>(QStringLiteral("workspace.homeControls"));
    check(home && tools->mapToGlobal(QPoint()).y() >= home->mapToGlobal(QPoint(0, home->height())).y(),
          "header/footer controls are not below the primary formatting bar");
    check(canvas->snapshot().document == before.document, "opening the header/footer ribbon mutates content");
    if (!editor || !editor->hasFocus()) {
        auto* focused = QApplication::focusWidget();
        std::cerr << "Header editor focus: " << (focused ? focused->objectName().toStdString() : "none") << '\n';
    }
    check(editor && editor->hasFocus(), "opening the ribbon loses inline editor focus");
    check(!QRect(tools->mapToGlobal(QPoint()), tools->size()).intersects(
              QRect(canvas->viewport()->mapToGlobal(QPoint()), canvas->viewport()->size())),
          "header/footer ribbon overlaps document paper");
    canvas->endHeaderFooterEditing();
    check(!tools->isVisible(), "header/footer ribbon stays open after leaving story editing");
}

void embeddedWorkflow() {
    docxstudio::app::MainWindow window;
    window.resize(820, 600); window.show();
    QApplication::processEvents();
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("documentTabs"));
    auto* canvas = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget());
    auto* ribbon = window.findChild<docxstudio::app::RibbonWidget*>();
    check(canvas->setHeaderFooterText(QStringLiteral("abc\t\t"), QStringLiteral("Footer\t\t")), "embedded workflow fixture");
    canvas->beginHeaderFooterEditing(false, 0, 0);
    QApplication::processEvents();
    auto* tools = canvas->headerFooterControls();
    auto* editor = canvas->findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
    auto cursor = editor->textCursor(); cursor.setPosition(1); editor->setTextCursor(cursor);
    auto* page = tools->findChild<QAbstractButton*>(QStringLiteral("story.insertPage"));
    auto* header = tools->findChild<QAbstractButton*>(QStringLiteral("story.header"));
    auto* close = tools->findChild<QAbstractButton*>(QStringLiteral("story.close"));
    page->click();
    check(canvas->headerText() == QStringLiteral("a{PAGE}bc\t\t") && editor->hasFocus(),
          "embedded Page Number loses the page editor insertion position");
    canvas->undo();
    check(canvas->headerText() == QStringLiteral("abc\t\t"), "embedded page field is not one Undo transaction");
    for (int width : {820, 1280}) {
        window.resize(width, width == 820 ? 600 : 860); QApplication::processEvents();
        for (auto* control : tools->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            check(!control->isVisible() || tools->rect().contains(control->geometry()),
                  "embedded header/footer ribbon clips a visible control");
        }
        if (qEnvironmentVariableIsSet("OWL_DOCS_STORY_TOOLBAR_RENDER")) {
            window.grab().save(QStringLiteral("/tmp/owl-docs-story-ribbon-%1.png").arg(width));
        }
    }
    canvas->beginHeaderFooterEditing(false, 2, 0);
    auto* right = canvas->findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor2"));
    QTest::keyClick(right, Qt::Key_Tab);
    check(header->hasFocus(), "Tab from the page cannot reach the embedded header/footer ribbon");
    QTest::keyClick(header, Qt::Key_Escape);
    check(!canvas->isHeaderFooterEditing() && !tools->isVisible(), "Escape from ribbon controls does not leave story editing");
    canvas->beginHeaderFooterEditing(true, 1, 0);
    canvas->markSaved();
    const auto original = tabs->currentIndex();
    window.findChild<QAction*>(QStringLiteral("file.new"))->trigger();
    check(!tools->isVisible(), "another document shows the previous tab's header/footer ribbon");
    auto* other = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget());
    other->beginHeaderFooterEditing(false, 0, 0);
    auto* otherTools = other->headerFooterControls();
    check(otherTools->isVisible() && !tools->isVisible(), "two documents share visible story controls");
    tabs->setCurrentIndex(original);
    check(tools->isVisible() && ribbon->isAncestorOf(tools) && !otherTools->isVisible(),
          "returning to a document loses its header/footer ribbon state");
    close->click();
    check(!canvas->isHeaderFooterEditing() && !tools->isVisible(), "embedded Close Header and Footer does not leave editing");
    // Closing a document with hosted controls must not leave dangling callbacks.
    tabs->setCurrentWidget(other);
    other->markSaved();
    window.findChild<QAction*>(QStringLiteral("file.new"))->trigger();
    QPointer<QWidget> releasedTools = otherTools;
    delete other;
    check(releasedTools.isNull(), "deleted document retains hosted story controls");
    qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget())->beginHeaderFooterEditing(false, 0, 0);
}

void narrowToolbar() {
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    docxstudio::app::applyAppTheme(&canvas);
    canvas.show();
    for (const int width : {420, 480, 1100}) {
        canvas.resize(width, 700);
        canvas.beginHeaderFooterEditing(false, 0, 0);
        QApplication::processEvents();
        auto* tools = canvas.findChild<QWidget*>(QStringLiteral("story.tools"));
        if (qEnvironmentVariableIsSet("OWL_DOCS_STORY_TOOLBAR_RENDER"))
            canvas.grab().save(QStringLiteral("/tmp/owl-docs-story-toolbar-%1.png").arg(width));
        check(tools && tools->isVisible() && canvas.viewport()->rect().contains(tools->geometry()),
              "header/footer controls extend beyond the narrow viewport");
        for (const auto* name : {"story.header", "story.footer", "story.region",
                                 "story.insertPage", "story.insertPages", "story.close"}) {
            auto* control = tools->findChild<QWidget*>(QString::fromUtf8(name));
            check(control && control->isVisible() && tools->rect().contains(control->geometry()) &&
                      control->width() >= control->minimumSizeHint().width(),
                  "a header/footer control is clipped or its label is truncated");
        }
        auto* close = tools->findChild<QAbstractButton*>(QStringLiteral("story.close"));
        check(close->text() == QStringLiteral("Close Header and Footer"),
              "narrow story controls lose the explicit Close Header and Footer label");
        canvas.endHeaderFooterEditing();
    }
}

void keyboardToolbar() {
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    docxstudio::app::applyAppTheme(&canvas);
    canvas.resize(420, 700);
    check(canvas.setFooterText(QStringLiteral("\t\tabc")), "keyboard toolbar fixture");
    canvas.show();
    canvas.beginHeaderFooterEditing(true, 2, 0);
    QApplication::processEvents();
    auto* editor = canvas.findChild<QPlainTextEdit*>(QStringLiteral("footerStoryEditor2"));
    auto cursor = editor->textCursor();
    cursor.setPosition(1);
    editor->setTextCursor(cursor);
    auto* region = canvas.findChild<QComboBox*>(QStringLiteral("story.region"));
    auto* page = canvas.findChild<QAbstractButton*>(QStringLiteral("story.insertPage"));
    auto* pages = canvas.findChild<QAbstractButton*>(QStringLiteral("story.insertPages"));
    auto* close = canvas.findChild<QAbstractButton*>(QStringLiteral("story.close"));
    region->setFocus();
    check(region->hasFocus(), "keyboard fixture could not focus the region selector");
    QTest::keyClick(region, Qt::Key_Tab);
    if (!page->hasFocus() && QApplication::focusWidget())
        std::cerr << "Focus after Tab: " << QApplication::focusWidget()->objectName().toStdString()
                  << "; page button type: " << page->metaObject()->className() << '\n';
    check(page->hasFocus(), "Tab skips the native Page Number toolbar button");
    QTest::keyClick(page, Qt::Key_Backtab);
    check(region->hasFocus(), "reverse Tab skips the story region selector");
    QTest::keyClick(region, Qt::Key_Tab);
    QTest::keyClick(page, Qt::Key_Return);
    check(canvas.footerText() == QStringLiteral("\t\ta{PAGE}bc") && editor->hasFocus() &&
              editor->textCursor().position() == 7,
          "Enter on Page Number loses the chosen story region or insertion caret");
    close->setFocus();
    QTest::keyClick(close, Qt::Key_Backtab);
    check(pages->hasFocus(), "reverse Tab skips the native Page Count toolbar button");
    QTest::keyClick(pages, Qt::Key_Return);
    check(canvas.footerText() == QStringLiteral("\t\ta{PAGE}{PAGES}bc") && editor->hasFocus(),
          "Enter on Page Count loses the chosen story region or insertion caret");
    close->setFocus();
    QTest::keyClick(close, Qt::Key_Return);
    check(!canvas.isHeaderFooterEditing(), "Enter on Close Header and Footer does not leave story mode");
}

void editorTraversal() {
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.resize(480, 700);
    canvas.show();
    for (const bool footer : {false, true}) {
        const QString prefix = footer ? QStringLiteral("footerStoryEditor") : QStringLiteral("headerStoryEditor");
        canvas.beginHeaderFooterEditing(footer, 0, 0);
        QApplication::processEvents();
        auto* left = canvas.findChild<QPlainTextEdit*>(prefix + QLatin1Char('0'));
        auto* center = canvas.findChild<QPlainTextEdit*>(prefix + QLatin1Char('1'));
        auto* right = canvas.findChild<QPlainTextEdit*>(prefix + QLatin1Char('2'));
        auto* header = canvas.findChild<QAbstractButton*>(QStringLiteral("story.header"));
        auto* region = canvas.findChild<QComboBox*>(QStringLiteral("story.region"));
        check(left && left->hasFocus(), "story traversal fixture lacks initial editor focus");
        const auto unchanged = canvas.snapshot().document;
        QTest::keyClick(left, Qt::Key_Tab);
        check(center->hasFocus(), "Tab from the initial story editor cannot reach the next region");
        check(region->currentIndex() == 1, "region selector does not follow the focused story editor");
        QTest::keyClick(center, Qt::Key_Backtab);
        check(left->hasFocus(), "reverse Tab cannot return to the previous story region");
        check(region->currentIndex() == 0, "region selector does not follow reverse editor traversal");
        canvas.beginHeaderFooterEditing(footer, 2, 0);
        QTest::keyClick(right, Qt::Key_Tab);
        check(header->hasFocus(), "Tab from the right story region cannot reach the toolbar");
        check(canvas.snapshot().document == unchanged, "story focus traversal inserts literal tabs into source text");
        canvas.endHeaderFooterEditing();
    }
}
}

int main(int argc, char** argv) {
    QTemporaryDir state;
    qputenv("XDG_DATA_HOME", state.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", state.path().toUtf8());
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("OwlDocs Header Ribbon Tests");
    QCoreApplication::setApplicationName("Header Ribbon");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, state.path());
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("ribbon")) {
        embeddedRibbon();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("ribbon-workflow")) {
        embeddedWorkflow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("keyboard")) {
        keyboardToolbar();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("editor-traversal")) {
        editorTraversal();
        return 0;
    }
    embeddedRibbon();
    embeddedWorkflow();
    narrowToolbar();
    keyboardToolbar();
    editorTraversal();
    return 0;
}
