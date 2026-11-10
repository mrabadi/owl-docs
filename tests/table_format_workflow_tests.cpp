#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMenu>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {
using namespace docxstudio;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void saveAs(app::MainWindow& window, const QString& path) {
    auto* action = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(action != nullptr, "find table Save As command");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "table Save As opens file chooser");
        dialog->selectFile(path);
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "accept table Save As destination");
    });
    action->trigger();
    check(QFileInfo::exists(path), "table Save As writes native package");
}

void dropdownFormattingWorkflow() {
    app::MainWindow window;
    window.resize(820, 600);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    auto* ribbon = window.findChild<app::RibbonWidget*>();
    check(canvas && ribbon && canvas->insertTable(2, 2, false), "create table dropdown workflow");
    const auto tableId = canvas->snapshot().document.tables().front().id();
    check(canvas->activateTableCell(tableId, 0, 0), "activate table dropdown fixture");
    canvas->insertText(QStringLiteral("Keep this cell text"));
    auto fixture = canvas->snapshot().document;
    check(bool(fixture.setTableGeometry(tableId, {{1440, 2880}, {0, 480}})),
          "set unequal table dropdown geometry");
    canvas->setDocument(std::move(fixture));
    check(canvas->selectTableCells(tableId, 0, 0, 1, 0), "select dropdown alignment rectangle");
    QApplication::processEvents();
    const auto chooser = [&](const char* key) {
        auto* button = ribbon->findChild<QToolButton*>(QStringLiteral("ribbonButton.table.%1").arg(key));
        check(button && button->isVisible() && button->isEnabled() && button->menu(),
              "actual table selection has no available contextual dropdown");
        return button;
    };
    const auto open = [](QToolButton* button) {
        button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
        QApplication::processEvents();
        check(button->menu()->isVisible(), "table contextual dropdown does not open");
    };

    auto* alignment = chooser("alignment");
    auto* bottomRight = alignment->menu()->findChild<QToolButton*>("ribbonButton.table.align.bottomRight");
    check(bottomRight && !bottomRight->isVisible(), "table alignment grid is permanently expanded");
    const auto beforeAlignment = canvas->snapshot();
    open(alignment);
    check(bottomRight->isVisible() && bottomRight->isEnabled(), "table alignment popup omits bottom-right choice");
    bottomRight->setFocus();
    QTest::keyClick(bottomRight, Qt::Key_Return);
    QApplication::processEvents();
    const auto aligned = canvas->snapshot();
    check(aligned.revision.value() == beforeAlignment.revision.value() + 1 && !alignment->menu()->isVisible(),
          "keyboard table positioning does not close and commit exactly once");
    for (std::size_t row = 0; row < 2; ++row) {
        const auto* table = aligned.document.findTable(tableId);
        check(table->cell(row, 0)->paragraph_format.alignment == core::ParagraphAlignment::right &&
                  table->cell(row, 0)->layout.vertical_alignment == core::TableCellVerticalAlignment::bottom &&
                  !table->cell(row, 1)->layout.vertical_alignment,
              "popup positioning changes the wrong cells or omits one alignment axis");
    }
    canvas->undo();
    check(canvas->snapshot().document == beforeAlignment.document,
          "popup horizontal and vertical alignment require more than one Undo");

    check(canvas->activateTableCell(tableId, 0, 0), "activate dropdown size cell");
    QApplication::processEvents();
    auto* size = chooser("size");
    auto* column = size->menu()->findChild<QDoubleSpinBox*>("ribbon.tableColumnWidth");
    auto* row = size->menu()->findChild<QDoubleSpinBox*>("ribbon.tableRowMinimumHeight");
    check(column && row && !column->isVisible() && !row->isVisible(),
          "actual table size controls occupy permanent ribbon space");
    const auto beforeWidth = canvas->snapshot();
    open(size);
    check(column->value() == 72 && row->value() == 0 && row->text().contains("Auto") &&
              canvas->snapshot().revision == beforeWidth.revision,
          "opening Size changes the document or loses selected width and Auto height");
    column->setFocus();
    column->selectAll();
    QTest::keyClicks(column, "96");
    check(canvas->snapshot().revision == beforeWidth.revision,
          "unfinished popup width typing mutates the document");
    QTest::keyClick(column, Qt::Key_Return);
    QApplication::processEvents();
    const auto resized = canvas->snapshot();
    check(resized.revision.value() == beforeWidth.revision.value() + 1 &&
              resized.document.findTable(tableId)->geometry().column_widths_twips ==
                  std::vector<std::uint32_t>({1920, 2880}),
          "committed popup width fails to resize only the selected column in one transaction");
    size->menu()->hide();
    canvas->undo();
    check(canvas->snapshot().document == beforeWidth.document, "popup column width is not one Undo transaction");
    const auto beforeSelectionSync = canvas->snapshot();
    check(canvas->selectTable(tableId), "select mixed table dropdown sizes");
    QApplication::processEvents();
    check(column->text().contains("Mixed") && row->text().contains("Mixed") &&
              canvas->snapshot().revision == beforeSelectionSync.revision,
          "whole-table selection loses mixed dimensions or creates a document edit");
    check(canvas->activateTableCell(tableId, 0, 0), "restore uniform table dropdown sizes");
    QApplication::processEvents();
    check(column->value() == 72 && row->text().contains("Auto") &&
              canvas->snapshot().revision == beforeSelectionSync.revision,
          "cell selection does not restore uniform width and Auto height without editing");

    check(canvas->selectTable(tableId), "select whole table for popup borders");
    QApplication::processEvents();
    auto* borders = chooser("borders");
    auto* weight = borders->menu()->findChild<QDoubleSpinBox*>("ribbon.tableBorderWidth");
    auto* color = borders->menu()->findChild<QToolButton*>("ribbonButton.table.borderColor");
    check(weight && color && color->menu(), "Borders popup has no line settings");
    const auto beforeBorders = canvas->snapshot();
    open(borders);
    weight->setFocus();
    weight->selectAll();
    QTest::keyClicks(weight, "1.25");
    QTest::keyClick(weight, Qt::Key_Return);
    check(ribbon->tableBorderWidthPoints() == 1.25 && canvas->snapshot().revision == beforeBorders.revision,
          "setting border line weight applies formatting before choosing a preset");
    open(color);
    QToolButton* red = nullptr;
    for (auto* swatch : color->menu()->findChildren<QToolButton*>())
        if (swatch->property("paletteColor").value<QColor>() == QColor(Qt::red)) red = swatch;
    check(red && red->isVisible(), "Borders popup has no usable red color swatch");
    red->setFocus();
    QTest::keyClick(red, Qt::Key_Return);
    check(ribbon->tableBorderColor() == QColor(Qt::red) &&
              canvas->snapshot().revision == beforeBorders.revision &&
              canvas->snapshot().document == beforeBorders.document,
          "preparing border color mutates the document before a preset is chosen");
    if (!borders->menu()->isVisible()) open(borders);
    auto* outside = window.findChild<QAction*>("table.borders.outside");
    check(outside && outside->isEnabled() && borders->menu()->actions().contains(outside),
          "actual Borders menu omits the native Outside Borders command");
    borders->menu()->setActiveAction(outside);
    QTest::keyClick(borders->menu(), Qt::Key_Return);
    QApplication::processEvents();
    const auto outlined = canvas->snapshot();
    const auto* outlinedTable = outlined.document.findTable(tableId);
    check(outlined.revision.value() == beforeBorders.revision.value() + 1 && !borders->menu()->isVisible() &&
              outlinedTable->cell(0, 0)->layout.border_top &&
              outlinedTable->cell(0, 0)->layout.border_top->style == core::TableBorderStyle::single &&
              outlinedTable->cell(0, 0)->layout.border_top->width_eighth_points == 10 &&
              outlinedTable->cell(0, 0)->layout.border_top->argb == 0xffff0000U &&
              outlinedTable->cell(1, 1)->layout.border_right &&
              outlinedTable->cell(1, 1)->layout.border_right->argb == 0xffff0000U,
          "keyboard border preset does not apply the prepared whole-table border exactly once");
    canvas->undo();
    check(canvas->snapshot().document == beforeBorders.document, "popup border preset is not one Undo transaction");
    canvas->markSaved();
}

void authoredRoundTrip(const QString& directory) {
    app::MainWindow window;
    window.show();
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    check(canvas && canvas->insertTable(2, 2, true), "create authored table workflow");
    auto document = canvas->snapshot().document;
    const auto tableId = document.tables().front().id();
    check(bool(document.setTableGeometry(tableId, {{1800, 3600}, {900, 0}})),
          "set authored table geometry");
    core::TableCellLayout first;
    first.vertical_alignment = core::TableCellVerticalAlignment::bottom;
    first.border_top = core::TableBorder{core::TableBorderStyle::none, 0, 0xff000000U};
    first.border_right = core::TableBorder{core::TableBorderStyle::single, 12, 0xff173b32U};
    check(bool(document.setTableCellLayout(tableId, 0, 0, first)), "set authored cell layout");
    canvas->setDocument(std::move(document));
    check(canvas->activateTableCell(tableId, 0, 0), "activate authored cell");
    canvas->insertText(QStringLiteral("One"));
    const auto original = canvas->snapshot().document;
    const auto path = directory + QStringLiteral("/authored.docx");
    saveAs(window, path);
    check(!canvas->isModified(), "native table layout save completes");
    const auto native = ooxml::DocxDocument::open(path.toStdString());
    check(native && native->academicRecovery(), "authored table layout has verified semantic recovery registry");
    const auto tableBlock = std::find_if(native->bodyBlocks().begin(), native->bodyBlocks().end(),
        [](const auto& block) { return std::holds_alternative<ooxml::ImportedTableBlock>(block); });
    const auto* imported = tableBlock == native->bodyBlocks().end() ? nullptr
        : std::get_if<ooxml::ImportedTableBlock>(&*tableBlock);
    check(imported && imported->column_widths_twips == std::vector<std::uint32_t>({1800, 3600}) &&
              imported->row_minimum_heights_twips == std::vector<std::uint32_t>({900, 0}) &&
              imported->cells[0].vertical_alignment == ooxml::BasicVerticalAlignment::bottom &&
              imported->cells[0].border_top && imported->cells[0].border_top->style == ooxml::BasicTableBorderStyle::none,
          "desktop converter omitted authored geometry, alignment, or explicit no-border");
    app::MainWindow reopened;
    check(reopened.openPath(path), "reopen authored native table layout");
    auto* restored = reopened.findChild<app::DocumentCanvas*>();
    check(restored && restored->snapshot().document == original,
          "native layout reopen changes table/cell identity or semantic properties");
    check(restored->activateTableCell(tableId, 0, 0), "activate restored table cell");
    restored->insertText(QStringLiteral("Edited "));
    const auto edited = restored->snapshot().document;
    reopened.findChild<QAction*>(QStringLiteral("file.save"))->trigger();
    check(!restored->isModified(), "native layout edit passes full package save proof");
    app::MainWindow again;
    check(again.openPath(path), "reopen edited table layout");
    check(again.findChild<app::DocumentCanvas*>()->snapshot().document == edited,
          "second native layout roundtrip changes semantic metadata");
}

void oldDefaultCompatibility(const QString& directory) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/owldocs-0.6.2-desktop-table.docx";
    const auto path = directory + QStringLiteral("/old-default.docx");
    check(QFile::copy(QString::fromStdString(fixture.string()), path), "copy frozen 0.6.2 native table");
    app::MainWindow window;
    check(window.openPath(path), "open frozen 0.6.2 table");
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    const auto tableId = canvas->snapshot().document.tables().front().id();
    check(canvas->snapshot().document.tables().front().geometry().empty(),
          "old automatic default grid became a new explicit layout");
    check(canvas->activateTableCell(tableId, 0, 0), "activate old table cell");
    canvas->insertText(QStringLiteral("Edited "));
    window.findChild<QAction*>(QStringLiteral("file.save"))->trigger();
    check(!canvas->isModified(), "old native table text edit now requires a conversion");
    app::MainWindow again;
    check(again.openPath(path), "reopen edited old table");
    check(again.findChild<app::DocumentCanvas*>()->snapshot().document.tables().front().cell(0, 0)->text.starts_with(u"Edited Prefix Before "),
          "old native table text edit was not saved");
}

void genericNativeImport(const QString& directory) {
    ooxml::NewTable table{2, 2, false, {"One", "Two", "Three", "Four"}};
    table.column_widths_twips = {1700, 3500};
    table.row_minimum_heights_twips = {800, 0};
    table.cell_layouts.resize(4);
    table.cell_layouts[0].vertical_alignment = ooxml::BasicVerticalAlignment::center;
    table.cell_layouts[0].border_bottom = ooxml::ImportedTableBorder{0, 0, ooxml::BasicTableBorderStyle::none};
    table.cell_layouts[0].border_right = ooxml::ImportedTableBorder{0x356078, 8};
    const auto path = directory + QStringLiteral("/generic-native.docx");
    check(ooxml::DocxDocument::writeNew(path.toStdString(), ooxml::NewDocumentBody{{table}}).saved,
          "write generic native table metadata fixture");
    app::MainWindow window;
    check(window.openPath(path), "open generic native layout metadata");
    const auto document = window.findChild<app::DocumentCanvas*>()->snapshot().document;
    const auto& imported = document.tables().front();
    check(imported.geometry().column_widths_twips == table.column_widths_twips &&
              imported.geometry().row_minimum_heights_twips == table.row_minimum_heights_twips &&
              imported.cell(0, 0)->layout.vertical_alignment == core::TableCellVerticalAlignment::center &&
              imported.cell(0, 0)->layout.border_bottom &&
              imported.cell(0, 0)->layout.border_bottom->style == core::TableBorderStyle::none &&
              imported.cell(0, 0)->layout.border_right &&
              imported.cell(0, 0)->layout.border_right->argb == 0xff356078U,
          "native import kept editable table layout only in a transient presentation cache");
}
}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir directory;
    check(directory.isValid(), "create private table layout workflow directory");
    if (qEnvironmentVariableIsSet("OWL_DOCS_CAPTURE_TABLE_FORMAT")) directory.setAutoRemove(false);
    qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", directory.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc, argv);
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) message->accept();
    });
    dismiss.start(1);
    if (application.arguments().contains(QStringLiteral("--dropdown-only"))) {
        dropdownFormattingWorkflow();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--import-only"))) {
        genericNativeImport(directory.path());
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--old-only"))) {
        oldDefaultCompatibility(directory.path());
        return 0;
    }
    dropdownFormattingWorkflow();
    authoredRoundTrip(directory.path());
    genericNativeImport(directory.path());
    oldDefaultCompatibility(directory.path());
    if (qEnvironmentVariableIsSet("OWL_DOCS_CAPTURE_TABLE_FORMAT"))
        std::cout << "Native table layout fixture: " << directory.filePath(QStringLiteral("authored.docx")).toStdString() << '\n';
    std::cout << "Table format desktop workflow tests passed\n";
}
