#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/app/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QMouseEvent>
#include <QInputMethodQueryEvent>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace docxstudio;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void mouse(app::DocumentCanvas& canvas, QEvent::Type type, const QPointF& point,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, canvas.viewport()->mapToGlobal(point.toPoint()),
                      button, buttons, Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &event);
}
void columnDrag(int zoom = 100) {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.resize(1000, 760); canvas.show();
    check(canvas.insertTable(2, 2, false), "drag table fixture");
    const auto table = canvas.snapshot().document.tables().front().id();
    check(canvas.activateTableCell(table, 0, 0), "drag cell fixture");
    canvas.insertText(QStringLiteral("Keep text"));
    canvas.setZoomPercent(zoom);
    QApplication::processEvents();
    const auto before = canvas.snapshot();
    const auto left = canvas.tableCellViewportRect(table, 0, 0);
    const auto right = canvas.tableCellViewportRect(table, 0, 1);
    check(!left.isEmpty() && !right.isEmpty(), "table cell viewport geometry");
    const QPointF boundary(left.right(), left.center().y());
    mouse(canvas, QEvent::MouseMove, boundary, Qt::NoButton, Qt::NoButton);
    check(canvas.viewport()->cursor().shape() == Qt::SizeHorCursor,
          "hovering a column boundary does not offer direct resizing");
    mouse(canvas, QEvent::MouseButtonPress, boundary, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, boundary + QPointF(48, 0), Qt::NoButton, Qt::LeftButton);
    check(canvas.snapshot().document == before.document, "resize preview mutates document before release");
    mouse(canvas, QEvent::MouseButtonRelease, boundary + QPointF(48, 0), Qt::LeftButton, Qt::NoButton);
    const auto resized = canvas.tableCellViewportRect(table, 0, 0);
    check(std::abs(resized.width() - left.width() - 48) < 1.5,
          "dragging a column boundary does not change the selected width");
    check(std::abs(canvas.tableCellViewportRect(table, 0, 1).right() - right.right()) < 1.0,
          "interior boundary resize changes total table width");
    check(canvas.snapshot().document.findTable(table)->cell(0, 0)->text == u"Keep text",
          "resizing edits cell content");
    canvas.undo();
    check(canvas.snapshot().document == before.document, "column drag is not one Undo transaction");
    canvas.redo();
    check(std::abs(canvas.tableCellViewportRect(table, 0, 0).width() - resized.width()) < 1.0,
          "column resize Redo loses geometry");
}
void rowDrag() {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.resize(1000, 760); canvas.show();
    check(canvas.insertTable(2, 2, false), "row drag fixture");
    const auto table = canvas.snapshot().document.tables().front().id();
    QApplication::processEvents();
    const auto before = canvas.snapshot();
    const auto top = canvas.tableCellViewportRect(table, 0, 0);
    const auto bottom = canvas.tableCellViewportRect(table, 1, 0);
    const QPointF boundary(top.center().x(), top.bottom());
    mouse(canvas, QEvent::MouseMove, boundary, Qt::NoButton, Qt::NoButton);
    check(canvas.viewport()->cursor().shape() == Qt::SizeVerCursor,
          "hovering a row boundary does not offer direct height resizing");
    mouse(canvas, QEvent::MouseButtonPress, boundary, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, boundary + QPointF(0, 48), Qt::NoButton, Qt::LeftButton);
    check(canvas.snapshot().document == before.document, "row resize preview commits before release");
    mouse(canvas, QEvent::MouseButtonRelease, boundary + QPointF(0, 48), Qt::LeftButton, Qt::NoButton);
    check(std::abs(canvas.tableCellViewportRect(table, 0, 0).height() - top.height() - 48) < 1.5,
          "dragging a row boundary does not change its minimum height");
    check(std::abs(canvas.tableCellViewportRect(table, 1, 0).height() - bottom.height()) < 1.0,
          "dragging one row changes another row height");
    canvas.undo();
    check(canvas.snapshot().document == before.document, "row resize is not one Undo transaction");
}
void narrowDrag() {
    core::Document document;
    auto made = core::Table::create(1, 2); check(bool(made), "narrow table fixture");
    const auto id = made.value().id();
    check(bool(document.insertTable(std::nullopt, std::move(made.value()))) &&
          bool(document.setTableGeometry(id, {{100, 100}, {}})), "narrow table geometry");
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling); canvas.resize(1000, 760); canvas.show();
    canvas.setDocument(document); QApplication::processEvents();
    const auto cell = canvas.tableCellViewportRect(id, 0, 0);
    const QPointF boundary(cell.right(), cell.center().y());
    const auto before = canvas.snapshot();
    QSignalSpy failures(&canvas, &app::DocumentCanvas::operationFailed);
    mouse(canvas, QEvent::MouseButtonPress, boundary, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, boundary, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, boundary, Qt::LeftButton, Qt::NoButton);
    check(failures.isEmpty() && canvas.snapshot().document == before.document,
          "clicking a narrow valid column boundary creates an invalid resize or changes widths");
}
void lowZoomCellClick() {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling); canvas.resize(1000, 760); canvas.show();
    check(canvas.insertTable(2, 2, false), "low zoom cell fixture");
    const auto id = canvas.snapshot().document.tables().front().id();
    check(canvas.activateTableCell(id, 0, 0), "initial low zoom cell"); canvas.insertText(QStringLiteral("First"));
    check(canvas.activateTableCell(id, 1, 1), "target low zoom cell"); canvas.insertText(QStringLiteral("Target"));
    check(canvas.activateTableCell(id, 0, 0), "restore initial low zoom cell");
    canvas.setZoomPercent(25); QApplication::processEvents();
    const auto point = canvas.tableCellViewportRect(id, 1, 1).center();
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    QInputMethodQueryEvent query(Qt::ImSurroundingText); QApplication::sendEvent(&canvas, &query);
    check(query.value(Qt::ImSurroundingText).toString() == QStringLiteral("Target"),
          "row resize hit targets consume ordinary cell clicks at minimum zoom");
}
void cancelledDrag() {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling); canvas.resize(1000, 760); canvas.show();
    check(canvas.insertTable(2, 2, false), "cancel drag fixture");
    const auto id = canvas.snapshot().document.tables().front().id();
    const auto before = canvas.snapshot();
    for (int mode : {0, 1, 2, 3}) {
        const auto rect = canvas.tableCellViewportRect(id, 0, 0);
        const QPointF boundary(rect.right(), rect.center().y());
        mouse(canvas, QEvent::MouseButtonPress, boundary, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, boundary + QPointF(32, 0), Qt::NoButton, Qt::LeftButton);
        if (mode == 1) canvas.setZoomPercent(150);
        else if (mode == 2) canvas.verticalScrollBar()->setValue(canvas.verticalScrollBar()->value() + 16);
        else if (mode == 3) canvas.resize(900, 700);
        else QTest::keyClick(&canvas, Qt::Key_Escape);
        mouse(canvas, QEvent::MouseButtonRelease, boundary + QPointF(32, 0), Qt::LeftButton, Qt::NoButton);
        check(canvas.snapshot().document == before.document,
              "Escape or a changed view transform during resize commits stale preview geometry");
    }
}
void formattingWorkflow() {
    app::MainWindow window;
    window.resize(1280, 860); window.show(); QApplication::processEvents();
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    check(canvas->insertTable(2, 2, false), "table formatting fixture");
    const auto id = canvas->snapshot().document.tables().front().id();
    check(canvas->selectTableCells(id, 0, 0, 1, 0), "formatting rectangle");
    auto* align = window.findChild<QAction*>(QStringLiteral("table.align.bottomRight"));
    check(align && align->isEnabled(), "selected cells have no direct nine-position text command");
    const auto before = canvas->snapshot();
    align->trigger();
    const auto after = canvas->snapshot();
    const auto* table = after.document.findTable(id);
    for (std::size_t row = 0; row < 2; ++row) {
        check(table->cell(row, 0)->paragraph_format.alignment == core::ParagraphAlignment::right &&
              table->cell(row, 0)->layout.vertical_alignment == core::TableCellVerticalAlignment::bottom,
              "cell text positioning does not apply both axes to the selected rectangle");
        check(!table->cell(row, 1)->layout.vertical_alignment, "cell alignment changes unselected cells");
    }
    canvas->undo();
    check(canvas->snapshot().document == before.document, "both cell alignment axes are not one Undo transaction");
    check(canvas->selectTable(id), "whole table formatting selection");
    auto* none = window.findChild<QAction*>(QStringLiteral("table.borders.none"));
    auto* outside = window.findChild<QAction*>(QStringLiteral("table.borders.outside"));
    check(none && outside && none->isEnabled(), "table border presets are unavailable");
    none->trigger();
    const auto cleared = canvas->snapshot();
    for (const auto& cell : cleared.document.findTable(id)->cells()) {
        check(cell.layout.border_top && cell.layout.border_top->style == core::TableBorderStyle::none &&
              cell.layout.border_right && cell.layout.border_right->style == core::TableBorderStyle::none &&
              cell.layout.border_bottom && cell.layout.border_bottom->style == core::TableBorderStyle::none &&
              cell.layout.border_left && cell.layout.border_left->style == core::TableBorderStyle::none,
              "No Borders does not override all four inherited cell borders");
    }
    outside->trigger();
    const auto outlined = canvas->snapshot();
    table = outlined.document.findTable(id);
    check(table->cell(0, 0)->layout.border_top->style == core::TableBorderStyle::single &&
          table->cell(0, 0)->layout.border_left->style == core::TableBorderStyle::single &&
          table->cell(1, 1)->layout.border_right->style == core::TableBorderStyle::single &&
          table->cell(1, 1)->layout.border_bottom->style == core::TableBorderStyle::single &&
          table->cell(0, 0)->layout.border_right->style == core::TableBorderStyle::none,
          "Outside Borders does not respect the selected table perimeter");
    canvas->undo();
    check(canvas->snapshot().document == cleared.document, "border preset is not one Undo transaction");
    canvas->markSaved();
}
}
int main(int argc, char** argv) {
    QTemporaryDir state;
    qputenv("XDG_CONFIG_HOME", state.path().toUtf8());
    qputenv("XDG_DATA_HOME", state.path().toUtf8());
    QApplication application(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("low-zoom-cell")) lowZoomCellClick();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("cancel")) cancelledDrag();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("narrow")) narrowDrag();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("formatting")) formattingWorkflow();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("row")) rowDrag();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("column")) {
        for (int zoom : {25, 75, 100, 150}) columnDrag(zoom);
    }
    else { for (int zoom : {25, 75, 100, 150}) columnDrag(zoom); rowDrag(); narrowDrag(); lowZoomCellClick(); cancelledDrag(); formattingWorkflow(); }
    std::cout << "Table editing UI passed\n";
}
