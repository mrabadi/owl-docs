#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/CodexController.h"
#include "docxstudio/codex/transport.hpp"
#include "docxstudio/app/NavigationDock.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QPalette>
#include <QSettings>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <cmath>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class UiCodexProcess final : public docxstudio::codex::Process {
public:
    explicit UiCodexProcess(int* starts = nullptr) : starts_(starts) {}
    bool start(const docxstudio::codex::ProcessSpec&,
               docxstudio::codex::ProcessCallbacks callbacks, std::string&) override {
        if (starts_) ++*starts_;
        callbacks_ = std::move(callbacks);
        return true;
    }
    bool writeStandardInput(std::string_view bytes, std::string&) override {
        writes_.emplace_back(bytes);
        return true;
    }
    void stop() noexcept override {}
    void reply(docxstudio::codex::Json result) {
        check(!writes_.empty(), "fake Codex received no request");
        const auto request = docxstudio::codex::Json::parse(writes_.back());
        check(request.contains("id"), "fake Codex expected a request ID");
        const std::string frame = docxstudio::codex::Json{
            {"id", request.at("id")}, {"result", std::move(result)}}.dump() + '\n';
        callbacks_.standardOutput(frame);
    }
private:
    int* starts_{};
    docxstudio::codex::ProcessCallbacks callbacks_;
    std::vector<std::string> writes_;
};

void finishCodexConnection(UiCodexProcess& process) {
    using docxstudio::codex::Json;
    process.reply({{"userAgent", "workspace-test"}, {"platformFamily", "unix"},
                   {"platformOs", "linux"}});
    process.reply({{"config", {{"mcp_servers", Json::object()}}}, {"origins", Json::object()}});
    process.reply({{"requiresOpenaiAuth", false}, {"account", nullptr}});
    Json efforts = Json::array();
    for (const auto* id : {"low", "medium", "high", "xhigh", "ultra"})
        efforts.push_back({{"reasoningEffort", id}, {"description", id}});
    Json model{{"id", "gpt-6.1-sol"}, {"model", "gpt-6.1-sol"},
               {"displayName", "GPT-6.1 Sol"}, {"description", "Synthetic UI model"},
               {"hidden", false}, {"isDefault", true}, {"defaultReasoningEffort", "low"},
               {"supportedReasoningEfforts", efforts},
               {"serviceTiers", Json::array({{{"id", "default"}, {"name", "Standard"}}})}};
    process.reply({{"data", Json::array({model})}, {"nextCursor", nullptr}});
}

void testChatOpeningConnectsOnce() {
    int starts = 0;
    UiCodexProcess* process = nullptr;
    docxstudio::app::MainWindow window(nullptr, [&] {
        auto adapter = std::make_unique<UiCodexProcess>(&starts);
        process = adapter.get();
        return adapter;
    });
    window.show();
    QApplication::processEvents();
    auto* dock = window.findChild<docxstudio::app::ChatDock*>();
    auto* controller = window.findChild<docxstudio::app::CodexController*>();
    auto* toggle = window.findChild<QAction*>("codex.toggle");
    check(dock && controller && toggle && !dock->isVisible() && !toggle->isChecked() && starts == 0,
          "startup connects to Codex without opening chat");
    toggle->trigger();
    check(dock->isVisible() && toggle->isChecked() && starts == 1 && process,
          "opening Codex chat does not start its connection automatically");
    toggle->trigger();
    toggle->trigger();
    check(starts == 1, "reopening chat duplicates an in-progress connection");
    finishCodexConnection(*process);
    check(controller->connected(), "synthetic Codex handshake did not connect");
    toggle->trigger();
    check(controller->connected() && !dock->isVisible() && !toggle->isChecked(),
          "hiding chat disconnects its session or leaves its toggle checked");
    toggle->trigger();
    for (const QSize size : {QSize(820, 600), QSize(1280, 860)}) {
        window.resize(size);
        QApplication::processEvents();
        check(window.size() == size, "chat controls force a larger window than requested");
        for (const auto* name : {"codex.model", "codex.effort", "codex.speed"}) {
            auto* choice = dock->findChild<QComboBox*>(QString::fromLatin1(name));
            check(choice && choice->isVisible() &&
                      dock->rect().contains(QRect(choice->mapTo(dock, QPoint()), choice->size())),
                  "visible chat choices are clipped at a supported window width");
        }
        for (const auto* name : {"codex.input", "codex.send"}) {
            auto* control = dock->findChild<QWidget*>(QString::fromLatin1(name));
            check(control && control->isVisible() &&
                      dock->rect().contains(QRect(control->mapTo(dock, QPoint()), control->size())),
                  "chat choices hide or clip the composer and Send button");
        }
    }
    check(starts == 1, "visibility or resize events duplicate a connected session");
    auto* connection = dock->findChild<QPushButton*>("codex.connection");
    check(connection && connection->isVisible() && connection->text() == "Disconnect",
          "connected chat has no explicit Disconnect control");
    connection->click();
    window.resize(900, 700);
    QApplication::processEvents();
    check(!controller->connected() && starts == 1 && connection->text() == "Reconnect",
          "an explicit disconnect is undone without reopening chat");
    toggle->trigger();
    toggle->trigger();
    check(starts == 2, "opening chat again does not reconnect after explicit disconnect");
}

void testInactiveEditFailureKeepsActiveFeedback() {
    docxstudio::app::MainWindow window;
    auto* tabs = window.findChild<QTabWidget*>("documentTabs");
    auto* inactive = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget());
    window.findChild<QAction*>("file.new")->trigger();
    check(tabs->currentWidget() != inactive, "inactive-error fixture did not switch documents");
    const QString feedback = QStringLiteral("Active document feedback");
    window.statusBar()->showMessage(feedback);
    check(!inactive->insertEquation(QStringLiteral("\\frac{"), true),
          "inactive-error fixture did not reject invalid math");
    check(window.statusBar()->currentMessage() == feedback,
          "an inactive document replaces the active document's edit feedback");
}

void testStoryModeDisablesBodyOnlyControls() {
    docxstudio::app::MainWindow window;
    window.show();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* bar = window.findChild<docxstudio::app::RibbonWidget*>();
    canvas->insertText("Body paragraph");
    canvas->beginHeaderFooterEditing(false, 0, 0);
    QApplication::processEvents();
    check(canvas->isHeaderFooterEditing(), "story controls fixture did not enter header editing");
    for (const auto* id : {"format.bold", "paragraph.bullets", "style.heading1",
             "insert.table", "insert.equation", "insert.pageBreak", "insert.excalidraw",
             "references.footnote"}) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
        check(action && !action->isEnabled(),
              "plain header/footer editing offers a body-only formatting or insertion command");
    }
    auto* home = bar->findChild<QWidget*>("workspace.homeControls");
    auto* storyTools = canvas->headerFooterControls();
    check(bar->isEnabled() && home && !home->isEnabled() && storyTools && storyTools->isEnabled() &&
              bar->isAncestorOf(storyTools) && window.findChild<QAction*>("insert.image")->isEnabled(),
          "story controls leave body formatting active, disable embedded story tools or disable supported picture insertion");
    canvas->endHeaderFooterEditing();
    QApplication::processEvents();
    check(bar->isEnabled() && home->isEnabled() && !storyTools->isVisible() &&
              window.findChild<QAction*>("format.bold")->isEnabled() &&
              window.findChild<QAction*>("insert.equation")->isEnabled(),
          "leaving story editing does not restore body controls");
}

void testAvailableCommandSearch() {
    docxstudio::app::CommandRegistry commands;
    auto* available = commands.add("file.save", "Save", QKeySequence::Save,
                                   [] {});
    auto* disabled = commands.add("table.deleteRows", "Delete Rows",
                                  QKeySequence(), [] {});
    auto* hidden = commands.add("review.placeholder", "Coming soon",
                                QKeySequence(), [] {});
    disabled->setEnabled(false);
    hidden->setVisible(false);
    check(commands.search({}) == QStringList{available->objectName()},
          "command search exposes hidden or unavailable actions");
}

void testCompactWorkspace() {
    docxstudio::app::MainWindow window;
    window.show();
    QApplication::processEvents();
    check(window.findChild<QWidget*>("workspace.header") &&
              window.findChild<QToolButton*>("workspace.save") &&
              window.findChild<QLabel*>("workspace.documentName"),
          "workspace lacks a persistent document header and Save button");
}

void testMenusShareBrandRow() {
    docxstudio::app::MainWindow window;
    window.show();
    QApplication::processEvents();
    auto* brand = window.findChild<QLabel*>("workspace.brand");
    auto* logo = window.findChild<QLabel*>("workspace.logo");
    auto* menus = window.menuBar();
    check(brand && logo && brand->text() == QStringLiteral("OwlDocs"),
          "workspace brand does not read OwlDocs beside the preserved logo");
    const QRect file = menus->actionGeometry(menus->actions().front());
    const int menuY = menus->mapToGlobal(file.center()).y();
    check(std::abs(brand->mapToGlobal(brand->rect().center()).y() - menuY) <= 10 &&
              std::abs(logo->mapToGlobal(logo->rect().center()).y() - menuY) <= 10,
          "File/Edit menus occupy a separate row from the owl logo and brand");
    for (int width : {1280, 820}) {
        window.resize(width, 700);
        QApplication::processEvents();
        check(brand->isVisible(), "compact workspace hides the OwlDocs brand");
        for (auto* action : menus->actions()) {
            const QRect bounds = menus->actionGeometry(action);
            auto* controls = menus->cornerWidget(Qt::TopRightCorner);
            check(bounds.isValid() && menus->rect().contains(bounds) &&
                      (!controls || !bounds.intersects(controls->geometry())),
                  "workspace clips a top-level menu at supported window widths");
        }
    }
}

void testMenuTextIsCenteredAndReadable() {
    docxstudio::app::MainWindow window;
    window.show();
    for (const int width : {820, 1280}) {
        window.resize(width, 700);
        QApplication::processEvents();
        auto* menus = window.menuBar();
        auto* logo = window.findChild<QLabel*>(QStringLiteral("workspace.logo"));
        check(logo && QFontInfo(menus->font()).pixelSize() >= 14,
              "top-level menus use text smaller than fourteen logical pixels");
        const int logoCenter = logo->mapToGlobal(logo->rect().center()).y();
        for (auto* action : menus->actions()) {
            const auto bounds = menus->actionGeometry(action);
            check(std::abs(menus->mapToGlobal(bounds.center()).y() - logoCenter) <= 2,
                  "top-level menu labels sit above or below the logo center");
            auto* controls = menus->cornerWidget(Qt::TopRightCorner);
            check(menus->rect().contains(bounds) && (!controls || !bounds.intersects(controls->geometry())),
                  "readable menu text is clipped by the Save/Find/Codex controls");
        }
    }
}

void testBulletClickExpandsInlineControls() {
    docxstudio::app::MainWindow window;
    window.show();
    QApplication::processEvents();
    auto* bar = window.findChild<docxstudio::app::RibbonWidget*>();
    auto* home = bar->findChild<QWidget*>("workspace.homeControls");
    auto* bullets = home->findChild<QToolButton*>("ribbonButton.paragraph.bullets");
    check(bullets && bullets->isVisible(), "Bullets is not directly available");
    bullets->click();
    QApplication::processEvents();
    auto* controls = bar->findChild<QWidget*>("ribbon.listTab");
    auto* properties = controls->findChild<QToolButton*>("ribbonButton.paragraph.listProperties");
    check(controls && controls->isVisible() && properties && properties->isVisible(),
          "enabling bullets does not expose direct list controls");
    for (const auto* id : {"paragraph.decreaseIndent", "paragraph.increaseIndent"}) {
        auto* button = controls->findChild<QToolButton*>(
            QStringLiteral("ribbonButton.%1").arg(id));
        check(button && button->isVisible() &&
                  button->mapToGlobal(QPoint()).y() >= home->mapToGlobal(home->rect().bottomLeft()).y(),
              "list indentation is not directly below the primary writing row");
    }
    auto* list = bar->findChild<QToolButton*>("workspace.list");
    check(!list || !list->isVisible(), "a redundant List dropdown appears above direct controls");
    bullets->click();
    QApplication::processEvents();
    check(!controls->isVisible(), "leaving a list leaves stale contextual controls");
}

void testNoPlaceholderCommands() {
    docxstudio::app::MainWindow window;
    for (const auto* id : {"insert.textBox", "insert.comment", "layout.columns",
             "review.comment", "review.trackChanges", "review.acceptChange",
             "review.rejectChange", "view.ruler"}) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
        check(!action || (!action->isEnabled() && !action->isVisible()),
              "workspace offers an action that cannot perform its advertised task");
    }
}

void testPaletteEnterClosesBeforeCommand() {
    docxstudio::app::MainWindow window;
    auto* tabs = window.findChild<QTabWidget*>("documentTabs");
    auto* palette = window.findChild<QAction*>("edit.commandPalette");
    auto* create = window.findChild<QAction*>("file.new");
    check(tabs && palette && create, "command palette test lacks document commands");
    bool invokedAfterClose = false;
    QObject::connect(create, &QAction::triggered, [&] {
        invokedAfterClose = QApplication::activeModalWidget() == nullptr;
    });
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "command palette did not open");
        auto* search = dialog->findChild<QLineEdit*>();
        check(search != nullptr, "command palette has no search field");
        search->setText("file.new");
        QTest::keyClick(search, Qt::Key_Return);
        if (dialog->isVisible()) dialog->reject();
    });
    palette->trigger();
    check(tabs->count() == 2 && invokedAfterClose,
          "Enter in command search does not execute after closing the palette");
}

void testNarrowWorkspacePanes() {
    docxstudio::app::MainWindow window(nullptr, [] { return std::make_unique<UiCodexProcess>(); });
    window.resize(820, 600);
    window.show();
    QApplication::processEvents();
    auto* find = window.findChild<QAction*>("edit.find");
    auto* toggle = window.findChild<QAction*>("codex.toggle");
    auto* navigation = window.findChild<docxstudio::app::NavigationDock*>();
    auto* codex = window.findChild<docxstudio::app::ChatDock*>();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    check(find && toggle && navigation && codex && canvas,
          "workspace pane controls are missing");
    find->trigger();
    QApplication::processEvents();
    toggle->trigger();
    QApplication::processEvents();
    check(codex->isVisible() && !navigation->isVisible() && toggle->isChecked(),
          "narrow workspace squeezes both auxiliary panes beside the document");
    codex->hide();
    QApplication::processEvents();
    check(!toggle->isChecked(), "Codex toggle disagrees with dock visibility");
    find->trigger();
    QApplication::processEvents();
    check(window.width() == 820 && canvas->viewport()->width() >= 420,
          "a narrow workspace cannot retain a usable writing viewport");
}

void testTaskMenus() {
    docxstudio::app::MainWindow window;
    auto* references = window.findChild<QMenu*>("menu.references");
    check(references != nullptr,
          "academic essentials have no discoverable References menu");
    for (const auto* id : {"references.footnote", "references.endnote",
             "references.caption", "references.crossReference",
             "references.bibliography", "references.citation", "references.toc"}) {
        auto* action = window.findChild<QAction*>(QString::fromLatin1(id));
        check(action && references->actions().contains(action),
              "References menu is missing an academic workflow");
    }
    auto* format = window.findChild<QMenu*>("menu.format");
    check(format && format->actions().contains(window.findChild<QAction*>("format.bold")),
          "formatting cannot be discovered through the menu bar");
    auto* layout = window.findChild<QMenu*>("menu.layout");
    check(layout && layout->actions().contains(window.findChild<QAction*>("layout.orientation")),
          "page layout has no discoverable Layout menu");
}

void testLayoutChoicesReachDocument() {
    docxstudio::app::MainWindow window;
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* margins = window.findChild<QMenu*>("menu.layout.margins");
    auto* paper = window.findChild<QMenu*>("menu.layout.paper");
    check(margins && paper, "Layout has no reachable Margins and Paper Size choices");
    struct MarginChoice { const char* id; double top; double side; };
    for (const auto& choice : {MarginChoice{"normal", 72, 72}, MarginChoice{"narrow", 36, 36},
             MarginChoice{"moderate", 72, 54}, MarginChoice{"wide", 72, 144}, MarginChoice{"office2003", 72, 90}}) {
        auto* action = window.findChild<QAction*>(QStringLiteral("layout.margin.%1").arg(choice.id));
        check(action && margins->actions().contains(action), "a supported margin preset has no Layout menu action");
        action->trigger();
        check(std::abs(canvas->marginTopPoints() - choice.top) < 0.01 &&
                  std::abs(canvas->marginBottomPoints() - choice.top) < 0.01 &&
                  std::abs(canvas->marginLeftPoints() - choice.side) < 0.01 &&
                  std::abs(canvas->marginRightPoints() - choice.side) < 0.01,
              "a margin menu choice does not update the document page");
    }
    auto* custom = window.findChild<QAction*>("layout.margin.custom");
    check(custom && margins->actions().contains(custom), "custom margins have no Layout menu route");
    bool edited = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "customMarginsDialog", "Custom Margins does not open its editor");
        for (const auto& choice : {std::pair{"top", 0.6}, std::pair{"bottom", 0.7},
                                  std::pair{"left", 0.8}, std::pair{"right", 0.9}}) {
            auto* field = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("customMargins.%1").arg(choice.first));
            check(field != nullptr, "custom margins editor lost a supported edge");
            field->setValue(choice.second);
        }
        edited = true;
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    custom->trigger();
    check(edited && std::abs(canvas->marginTopPoints() - 43.2) < 0.01 &&
              std::abs(canvas->marginRightPoints() - 64.8) < 0.01 &&
              std::abs(canvas->marginBottomPoints() - 50.4) < 0.01 &&
              std::abs(canvas->marginLeftPoints() - 57.6) < 0.01,
          "accepted custom margins do not reach the document");
    struct PaperChoice { const char* id; double width; double height; };
    for (const auto& choice : {PaperChoice{"letter", 612, 792}, PaperChoice{"a4", 595.28, 841.89},
                              PaperChoice{"legal", 612, 1008}}) {
        auto* action = window.findChild<QAction*>(QStringLiteral("layout.paper.%1").arg(choice.id));
        check(action && paper->actions().contains(action), "a supported paper size has no Layout menu action");
        action->trigger();
        check(std::abs(canvas->pageWidthPoints() - choice.width) < 0.01 &&
                  std::abs(canvas->pageHeightPoints() - choice.height) < 0.01,
              "a paper-size menu choice does not update document dimensions");
    }
}

void testFitPageWidthCommand() {
    docxstudio::app::MainWindow window;
    window.resize(1280, 860);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* fit = window.findChild<QAction*>("view.pageWidth");
    check(canvas && fit, "Fit page width command is missing");
    const int expected = static_cast<int>(std::floor(
        (canvas->viewport()->width() - 56.0) /
        (canvas->pageWidthPoints() * 96.0 / 72.0) * 100.0));
    fit->trigger();
    check(std::abs(canvas->zoomPercent() - expected) <= 2,
          "Fit page width still uses a fixed zoom percentage");
}

void testFitFromLowZoom() {
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    canvas.setZoomPercent(25);
    QApplication::processEvents();
    check(canvas.verticalScrollBar()->maximum() == 0,
          "low-zoom fit fixture already has a vertical scrollbar");
    canvas.fitToWidth();
    QApplication::processEvents();
    check(canvas.verticalScrollBar()->maximum() > 0 &&
              canvas.horizontalScrollBar()->maximum() == 0,
          "fitting from low zoom leaves horizontal overflow after the vertical scrollbar appears");
}

void testOutlineJumpsToHeading() {
    docxstudio::app::MainWindow window;
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* navigation = window.findChild<docxstudio::app::NavigationDock*>();
    auto* show = window.findChild<QAction*>("view.navigation");
    check(canvas && navigation && show, "outline workflow lacks its document controls");
    canvas->insertText("Introduction");
    canvas->applyParagraphStyle("Heading1");
    QTest::keyClick(canvas, Qt::Key_Return);
    canvas->insertText("Body text");
    QTest::keyClick(canvas, Qt::Key_Return);
    canvas->insertText("Methods");
    canvas->applyParagraphStyle("Heading2");
    const auto target = canvas->snapshot().document.paragraphs().back().id();
    show->trigger();
    QApplication::processEvents();
    auto* outline = navigation->findChild<QListWidget*>("navigation.outline");
    check(navigation->mode() == docxstudio::app::NavigationDock::Mode::outline &&
              outline && outline->count() == 2,
          "Navigation does not open a live document outline");
    outline->setCurrentRow(1);
    QTest::keyClick(outline, Qt::Key_Return);
    check(canvas->selection().anchor.paragraph_id == target,
          "outline activation does not jump to its heading");
}

void testInactiveDocumentCannotChangeChrome() {
    docxstudio::app::MainWindow window;
    auto* first = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* create = window.findChild<QAction*>("file.new");
    auto* tabs = window.findChild<QTabWidget*>("documentTabs");
    check(first && create && tabs, "active-document fixture is incomplete");
    create->trigger();
    auto* active = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget());
    auto* font = window.findChild<QComboBox*>("ribbon.fontFamily");
    auto* status = window.findChild<QLabel*>("status.pageAndWords");
    auto* bold = window.findChild<QAction*>("format.bold");
    check(active && active != first && font && status && bold,
          "active-document controls are missing");
    const QString family = font->currentText();
    const QString pageStatus = status->text();
    first->cursorFormatChanged("DejaVu Serif", 42, QColor(Qt::red));
    first->cursorStyleChanged(true, false, false, false, false, false);
    first->pageStatusChanged(99, 99, 9999);
    check(font->currentText() == family && status->text() == pageStatus &&
              !bold->isChecked(),
          "inactive document events overwrite the active document's toolbar or status");
}

void testEditFeedbackDoesNotBlockWriting() {
    docxstudio::app::MainWindow window;
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    check(canvas != nullptr, "edit feedback fixture lacks a canvas");
    bool blocked = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            blocked = true;
            dialog->reject();
        }
    });
    canvas->operationFailed(QStringLiteral("This selection cannot be changed."));
    check(!blocked && window.statusBar()->currentMessage().contains("This selection cannot be changed"),
          "routine editing feedback blocks writing with a modal popup");
}

void testManualSaveStateFollowsActiveDocument() {
    docxstudio::app::MainWindow window;
    auto* tabs = window.findChild<QTabWidget*>("documentTabs");
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* state = window.findChild<QLabel*>("status.saveState");
    auto* create = window.findChild<QAction*>("file.new");
    check(tabs && canvas && state && create, "save-state fixture lacks controls");
    canvas->insertText("Changes waiting for Ctrl+S");
    check(state->text() == QStringLiteral("Unsaved changes"),
          "dirty document is presented as saved or ready");
    create->trigger();
    check(state->text() == QStringLiteral("New document"),
          "an untitled document is incorrectly presented as written to disk");
    tabs->setCurrentWidget(canvas);
    check(state->text() == QStringLiteral("Unsaved changes"),
          "manual save status does not follow the active document");
}

void testBackgroundSaveKeepsActiveDirtyStatus() {
    QTemporaryDir fixtures;
    check(fixtures.isValid(), "could not create background save fixtures");
    const QString activePath = fixtures.filePath(QStringLiteral("active.docx"));
    const QString backgroundPath = fixtures.filePath(QStringLiteral("background.docx"));
    for (const auto& path : {activePath, backgroundPath}) {
        docxstudio::ooxml::NewParagraph paragraph;
        paragraph.runs.emplace_back("Initial text", docxstudio::ooxml::BasicRunFormat{});
        check(docxstudio::ooxml::DocxDocument::writeNew(
                  std::filesystem::path(QFile::encodeName(path).constData()), {paragraph}).saved,
              "could not write background save fixture");
    }
    docxstudio::app::MainWindow window;
    check(window.openPaths({activePath, backgroundPath}) == 2,
          "could not open background save fixtures");
    QFile beforeSave(backgroundPath);
    check(beforeSave.open(QIODevice::ReadOnly), "could not read background save fixture");
    const QByteArray originalBytes = beforeSave.readAll();
    beforeSave.close();
    auto* tabs = window.findChild<QTabWidget*>("documentTabs");
    auto* state = window.findChild<QLabel*>("status.saveState");
    check(tabs && state && tabs->count() == 2, "background save lacks document tabs");
    auto* active = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->widget(0));
    auto* background = qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->widget(1));
    check(active && background, "background save lacks document canvases");
    active->insertText("Still waiting for Ctrl+S");
    background->insertText("Save this tab while closing it");
    tabs->setCurrentWidget(active);
    QTimer::singleShot(0, &window, [&] {
        auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "closing a modified background tab did not ask to save");
        prompt->button(QMessageBox::Save)->click();
    });
    tabs->tabCloseRequested(1);
    check(tabs->count() == 1 && tabs->currentWidget() == active && active->isModified(),
          "background save changed the active document or failed to close its tab");
    QFile afterSave(backgroundPath);
    check(afterSave.open(QIODevice::ReadOnly) && afterSave.readAll() != originalBytes,
          "closing the background tab did not actually save its changed content");
    check(state->text() == QStringLiteral("Unsaved changes"),
          "saving a background tab presents the active dirty document as saved");
}

void captureRealPickers() {
    const QString suffix = qEnvironmentVariable("OWL_DOCS_CAPTURE_SUFFIX");
    docxstudio::app::MainWindow window(nullptr, [] { return std::make_unique<UiCodexProcess>(); });
    window.show();
    for (const QSize size : {QSize(1280, 860), QSize(820, 600)}) {
        window.resize(size);
        for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
        bool capturedTable = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->objectName() == "insertTableDialog", "Insert Table did not open its real dimension picker");
            for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
            check(dialog->width() <= size.width() && dialog->height() <= size.height(), "table dimension picker exceeds the supported window");
            check(dialog->grab().save(QStringLiteral("/tmp/owl-docs-062-insert-table-%1%2.png").arg(size.width()).arg(suffix)),
                  "could not capture real table dimension picker");
            capturedTable = true;
            dialog->reject();
        });
        window.findChild<QAction*>("insert.table")->trigger();
        check(capturedTable, "table picker capture did not complete");
        auto* bar = window.findChild<docxstudio::app::RibbonWidget*>();
        auto* color = bar->findChild<QToolButton*>("ribbonButton.format.textColor");
        check(color && color->isVisible() && color->menu(), "real font color picker is not directly reachable");
        bool capturedColor = false;
        QTimer::singleShot(0, color->menu(), [&] {
            auto* menu = color->menu();
            for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
            auto* palette = menu->findChild<QWidget*>("color.palette");
            check(palette && palette->isVisible() && menu->width() <= size.width() && menu->height() <= size.height(),
                  "real font color grid is absent or too large for the supported window");
            for (auto* swatch : palette->findChildren<QToolButton*>()) {
                if (!swatch->isVisible()) continue;
                check(menu->rect().contains(QRect(swatch->mapTo(menu, QPoint()), swatch->size())),
                      "a real font color choice is clipped by its menu");
            }
            check(menu->grab().save(QStringLiteral("/tmp/owl-docs-062-font-color-%1%2.png").arg(size.width()).arg(suffix)),
                  "could not capture real font color menu");
            capturedColor = true;
            menu->hide();
        });
        color->showMenu();
        check(capturedColor, "font color capture did not complete");
    }
}

void captureRibbonContexts() {
    const QString suffix = qEnvironmentVariable("OWL_DOCS_CAPTURE_SUFFIX");
    for (const auto* context : {"normal", "list", "table", "picture", "figure", "equation"}) {
        docxstudio::app::MainWindow window(nullptr, [] { return std::make_unique<UiCodexProcess>(); });
        window.resize(1280, 860);
        window.show();
        auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
        auto* bar = window.findChild<docxstudio::app::RibbonWidget*>();
        canvas->insertText("A clearer place to write");
        canvas->applyParagraphStyle("Heading1");
        QTest::keyClick(canvas, Qt::Key_Return);
        canvas->applyParagraphStyle("Normal");
        canvas->insertText("Draft ideas, shape your argument, and keep your document on your device.");
        QTest::keyClick(canvas, Qt::Key_Return);
        const QString kind = QString::fromLatin1(context);
        QString expectedPage;
        if (kind == "list") {
            canvas->insertText("Research question");
            canvas->toggleBullets();
            expectedPage = "ribbon.listTab";
        } else if (kind == "table") {
            check(canvas->insertTable(3, 3, true), "capture could not insert a native table");
            const auto snapshot = canvas->snapshot();
            const auto id = snapshot.document.tables().front().id();
            for (std::size_t column = 0; column < 3; ++column) {
                check(canvas->activateTableCell(id, 0, column), "capture could not activate a table cell");
                canvas->insertText(QStringList{"Question", "Evidence", "Finding"}[static_cast<qsizetype>(column)]);
            }
            check(canvas->selectTable(id), "capture could not select its table");
            expectedPage = "ribbon.tableTab";
        } else if (kind == "equation") {
            check(canvas->insertEquation(QStringLiteral("\\frac{a}{b} + x^2"), true), "capture could not insert a native equation");
            const auto snapshot = canvas->snapshot();
            const auto id = snapshot.document.paragraphs().back().equations().front().id;
            check(canvas->selectEquation(id), "capture could not select its equation");
            expectedPage = "ribbon.equationTab";
        } else if (kind == "picture" || kind == "figure") {
            QImage image(360, 120, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor("#f4f5f3"));
            QPainter painter(&image);
            painter.setPen(QPen(QColor("#245648"), 3));
            painter.drawRoundedRect(QRect(8, 8, 344, 104), 12, 12);
            painter.setFont(QFont("DejaVu Sans", 15));
            painter.drawText(image.rect(), Qt::AlignCenter, "Question → Evidence → Insight");
            painter.end();
            QByteArray png;
            QBuffer buffer(&png);
            check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "capture could not encode picture");
            if (kind == "figure") {
                QString error;
                png = docxstudio::app::pngWithExcalidrawScene(png,
                    R"({"type":"excalidraw","version":2,"source":"OwlDocs UI fixture","elements":[],"appState":{},"files":{}})", error);
                check(!png.isEmpty(), "capture could not embed editable figure source");
            }
            check(canvas->insertInlineImage(std::vector<std::uint8_t>(png.begin(), png.end()), "Research process"),
                  "capture could not insert a semantic picture");
            expectedPage = "ribbon.pictureTab";
        }
        for (const QSize size : {QSize(1280, 860), QSize(820, 600)}) {
            window.resize(size);
            for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
            canvas->fitToWidth();
            for (int pass = 0; pass < 4; ++pass) QApplication::processEvents();
            canvas->verticalScrollBar()->setValue(0);
            canvas->horizontalScrollBar()->setValue(0);
            QApplication::processEvents();
            auto* home = bar->findChild<QWidget*>("workspace.homeControls");
            check(home && home->isVisible(), "settled object selection replaced primary formatting");
            if (!expectedPage.isEmpty()) {
                auto* page = bar->findChild<QWidget*>(expectedPage);
                check(page && page->isVisible() && page->mapToGlobal(QPoint()).y() >= home->mapToGlobal(home->rect().bottomLeft()).y(),
                      "settled real document selection has no expanded context below primary formatting");
                for (auto* button : page->findChildren<QToolButton*>()) {
                    if (!button->isVisible()) continue;
                    check(bar->rect().contains(QRect(button->mapTo(bar, QPoint()), button->size())),
                          "settled contextual command is clipped in the real workspace");
                }
            }
            auto* more = bar->findChild<QToolButton*>("workspace.moreFormatting");
            std::cout << "AUDIT " << context << " window=" << size.width() << " bar=" << bar->width() << " direct:";
            for (auto* button : home->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (button->isVisible() && button != more) std::cout << ' ' << button->objectName().toStdString();
            }
            std::cout << " overflow:";
            for (auto* action : more->menu()->actions()) std::cout << ' ' << action->text().toStdString();
            std::cout << '\n';
            check(window.grab().save(QStringLiteral("/tmp/owl-docs-062-%1-%2%3.png").arg(kind).arg(size.width()).arg(suffix)),
                  "could not save settled real context capture");
            if (kind == "table") {
                auto* page = bar->findChild<QWidget*>("ribbon.tableTab");
                check(page && page->height() <= 48, "real table ribbon is taller than one row");
                for (const auto* group : {"alignment", "size", "borders", "insert", "delete"}) {
                    auto* button = page->findChild<QToolButton*>(QStringLiteral("ribbonButton.table.%1").arg(group));
                    check(button && button->isVisible() && button->menu(), "real table dropdown is missing at a supported window size");
                    button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
                    QApplication::processEvents();
                    check(button->menu()->grab().save(QStringLiteral("/tmp/owl-docs-064-table-%1-%2%3.png")
                        .arg(group).arg(size.width()).arg(suffix)), "could not capture real table dropdown");
                    button->menu()->hide();
                }
            }
        }
    }
}

void captureWorkspace() {
    const QString suffix = qEnvironmentVariable("OWL_DOCS_CAPTURE_SUFFIX");
    UiCodexProcess* process = nullptr;
    docxstudio::app::MainWindow window(nullptr, [&] {
        auto adapter = std::make_unique<UiCodexProcess>();
        process = adapter.get();
        return adapter;
    });
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    check(canvas != nullptr, "capture workspace lacks a document canvas");
    canvas->insertText("A clearer place to write");
    canvas->applyParagraphStyle("Heading1");
    QTest::keyClick(canvas, Qt::Key_Return);
    canvas->insertText("Draft ideas, shape your argument, and keep your document on your device. Your writing stays at the center of a calmer workspace.");
    QTest::keyClick(canvas, Qt::Key_Return);
    canvas->insertText("Research notes");
    canvas->applyParagraphStyle("Heading2");
    QTest::keyClick(canvas, Qt::Key_Return);
    canvas->insertText("Use headings to navigate a long document. Add equations, tables, references, and editable local figures from the Insert and References menus.");
    QTest::keyClick(canvas, Qt::Key_Return);
    check(canvas->insertEquation(QStringLiteral("\\frac{a}{b} + x^2"), true),
          "capture could not insert a sample equation");
    window.resize(1280, 860);
    window.show();
    window.findChild<QAction*>("codex.toggle")->trigger();
    check(process != nullptr, "capture chat did not create a synthetic connection");
    finishCodexConnection(*process);
    QApplication::processEvents();
    canvas->fitToWidth();
    QApplication::processEvents();
    canvas->verticalScrollBar()->setValue(0);
    canvas->horizontalScrollBar()->setValue(0);
    QApplication::processEvents();
    check(window.grab().save(QStringLiteral("/tmp/owl-docs-workspace-wide%1.png").arg(suffix)),
          "could not save wide workspace capture");
    window.resize(820, 600);
    QApplication::processEvents();
    canvas->fitToWidth();
    QApplication::processEvents();
    canvas->verticalScrollBar()->setValue(0);
    canvas->horizontalScrollBar()->setValue(0);
    check(window.grab().save(QStringLiteral("/tmp/owl-docs-workspace-compact-chat%1.png").arg(suffix)),
          "could not save compact Codex workspace capture");
    window.findChild<QAction*>("view.navigation")->trigger();
    QApplication::processEvents();
    canvas->fitToWidth();
    QApplication::processEvents();
    canvas->verticalScrollBar()->setValue(0);
    canvas->horizontalScrollBar()->setValue(0);
    QApplication::processEvents();
    check(window.grab().save(QStringLiteral("/tmp/owl-docs-workspace-compact%1.png").arg(suffix)),
          "could not save compact workspace capture");
}
}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir profile;
    check(profile.isValid(), "could not isolate workspace preferences");
    QCoreApplication::setOrganizationName("Owl Docs Workspace Tests");
    QCoreApplication::setApplicationName("Workspace");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       profile.path());
    qputenv("XDG_DATA_HOME", profile.path().toUtf8());
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("capture-pickers")) {
        captureRealPickers();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("capture-contexts")) {
        captureRibbonContexts();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("capture")) {
        captureWorkspace();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("fit-scrollbars")) {
        testFitFromLowZoom();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("outline")) {
        testOutlineJumpsToHeading();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("active-tab")) {
        testInactiveDocumentCannotChangeChrome();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("edit-feedback")) {
        testEditFeedbackDoesNotBlockWriting();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("save-state")) {
        testManualSaveStateFollowsActiveDocument();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("inactive-save")) {
        testBackgroundSaveKeepsActiveDirtyStatus();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("layout-choices")) {
        testLayoutChoicesReachDocument();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("menus")) {
        testTaskMenus();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("brand-row")) {
        testMenusShareBrandRow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("menu-type")) {
        testMenuTextIsCenteredAndReadable();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("inline-list")) {
        testBulletClickExpandsInlineControls();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("chat-opening")) {
        testChatOpeningConnectsOnce();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("inactive-error")) {
        testInactiveEditFailureKeepsActiveFeedback();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("story-controls")) {
        testStoryModeDisablesBodyOnlyControls();
        return 0;
    }
    testAvailableCommandSearch();
    std::cout << "PASS: available command search\n";
    testCompactWorkspace();
    std::cout << "PASS: persistent document header\n";
    testMenusShareBrandRow();
    std::cout << "PASS: logo, brand and menus share a row\n";
    testMenuTextIsCenteredAndReadable();
    testBulletClickExpandsInlineControls();
    std::cout << "PASS: bullets expand direct list controls\n";
    testChatOpeningConnectsOnce();
    std::cout << "PASS: chat opening connects once and explicit disconnect persists\n";
    testInactiveEditFailureKeepsActiveFeedback();
    std::cout << "PASS: inactive edit failure preserves active feedback\n";
    testStoryModeDisablesBodyOnlyControls();
    std::cout << "PASS: story editing disables unsupported body controls\n";
    testNoPlaceholderCommands();
    std::cout << "PASS: no placeholder commands\n";
    testPaletteEnterClosesBeforeCommand();
    std::cout << "PASS: palette Enter after close\n";
    testNarrowWorkspacePanes();
    std::cout << "PASS: narrow workspace panes\n";
    testTaskMenus();
    testLayoutChoicesReachDocument();
    std::cout << "PASS: academic and formatting task menus\n";
    testFitPageWidthCommand();
    std::cout << "PASS: fit page width\n";
    testFitFromLowZoom();
    std::cout << "PASS: fit from low zoom\n";
    testOutlineJumpsToHeading();
    std::cout << "PASS: live document outline\n";
    testInactiveDocumentCannotChangeChrome();
    std::cout << "PASS: active document chrome isolation\n";
    testEditFeedbackDoesNotBlockWriting();
    std::cout << "PASS: nonmodal editing feedback\n";
    testManualSaveStateFollowsActiveDocument();
    std::cout << "PASS: persistent manual save state\n";
    testBackgroundSaveKeepsActiveDirtyStatus();
    std::cout << "PASS: background save preserves active dirty state\n";
    return 0;
}
