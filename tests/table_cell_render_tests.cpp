#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using namespace docxstudio;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
QRectF caret(app::DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRectF();
}
int darkBorderPixels(app::DocumentCanvas& canvas, const QPointF& point) {
    const auto image = canvas.viewport()->grab().toImage();
    const double ratio = image.devicePixelRatio();
    int dark = 0;
    for (int y = -3; y <= 3; ++y) for (int x = -2; x <= 2; ++x) {
        const auto p = ((point + QPointF(x, y)) * ratio).toPoint();
        if (image.rect().contains(p)) {
            const auto color = image.pixelColor(p);
            if (color.red() < 170 && color.green() < 170 && color.blue() < 170) ++dark;
        }
    }
    return dark;
}
void cellLayout() {
    core::Document document;
    auto made = core::Table::create(1, 1); check(bool(made), "cell layout fixture");
    const auto id = made.value().id();
    check(bool(document.insertTable(std::nullopt, std::move(made.value()))) &&
          bool(document.setTableGeometry(id, {{3600}, {2000}})) &&
          bool(document.setTableCellText(id, 0, 0, u"Position")), "sized cell fixture");
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling); canvas.resize(1000, 760); canvas.show();
    canvas.setDocument(document); check(canvas.activateTableCell(id, 0, 0), "top cell caret");
    QApplication::processEvents();
    const auto top = caret(canvas);
    auto layout = core::TableCellLayout{};
    layout.vertical_alignment = core::TableCellVerticalAlignment::bottom;
    core::ParagraphFormatDelta horizontal;
    horizontal.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(core::ParagraphAlignment::right);
    check(bool(document.setTableCellLayout(id, 0, 0, layout)) &&
          bool(document.applyTableCellParagraphFormat(id, 0, 0, horizontal)), "bottom right cell fixture");
    canvas.setDocument(document); check(canvas.activateTableCell(id, 0, 0), "bottom cell caret");
    QApplication::processEvents();
    const auto bottom = caret(canvas);
    check(bottom.x() > top.x() + 100 && bottom.y() > top.y() + 80,
          "authored bottom-right cell layout does not move the rendered text and insertion caret");
}
void noBorders() {
    core::Document document;
    auto made = core::Table::create(1, 1); check(bool(made), "border fixture");
    const auto id = made.value().id();
    check(bool(document.insertTable(std::nullopt, std::move(made.value()))), "border table fixture");
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling); canvas.resize(1000, 760); canvas.show(); canvas.setDocument(document);
    QApplication::processEvents();
    const auto rect = canvas.tableCellViewportRect(id, 0, 0);
    check(darkBorderPixels(canvas, QPointF(rect.left(), rect.center().y())) > 0, "inherited grid border fixture has no visible stroke");
    core::TableCellLayout layout;
    const core::TableBorder none{core::TableBorderStyle::none, 0, 0xff000000U};
    layout.border_top = none; layout.border_right = none; layout.border_bottom = none; layout.border_left = none;
    check(bool(document.setTableCellLayout(id, 0, 0, layout)), "explicit no-border fixture");
    canvas.setDocument(document); QApplication::processEvents();
    check(darkBorderPixels(canvas, QPointF(rect.left(), rect.center().y())) == 0,
          "No Borders leaves the inherited grid stroke on the page");
}
}
int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_DATA_HOME", settings.path().toUtf8()); qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QApplication application(argc, argv); QStandardPaths::setTestModeEnabled(true);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("borders")) noBorders();
    else if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("alignment")) cellLayout();
    else { cellLayout(); noBorders(); }
    std::cout << "Table cell render tests passed\n";
}
