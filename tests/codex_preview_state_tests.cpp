#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace docxstudio;
using codex::Json;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
Json preview(app::DocumentCanvas& canvas, Json operations, QString& error) {
    QString summary;
    return app::invokeEditorTool(canvas, QStringLiteral("preview-state"), QStringLiteral("editor_v1_preview"),
        {{"documentId", "preview-state"}, {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Review a structural change"}, {"operations", std::move(operations)}}, summary, error);
}
void tableCursor(std::string_view kind) {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    check(canvas.insertTable(3, 2, false), "create table selection fixture");
    const auto table = canvas.snapshot().document.tables().front().id();
    check(canvas.activateTableCell(table, 2, 1), "activate last selected table cell");
    canvas.insertText(QStringLiteral("Kept"));
    const auto selected = canvas.selectedTableCellRange();
    check(selected.has_value(), "capture stable active cell");
    const auto cell = selected->cellId;
    const auto before = canvas.snapshot();
    Json operation;
    if (kind == "table-delete") operation = {{"kind", "delete_table"}, {"tableId", table.toString()}};
    else if (kind == "row-insert") operation = {{"kind", "insert_table_row"}, {"tableId", table.toString()}, {"index", 0}};
    else operation = {{"kind", "delete_table_rows"}, {"tableId", table.toString()}, {"index", kind == "row-delete-active" ? 2 : 0}, {"count", 1}};
    QString error;
    check(!preview(canvas, Json::array({operation}), error).empty() && error.isEmpty(), "create table structure preview");
    check(canvas.snapshot().document == before.document, "table preview changed the authoritative document");
    check(canvas.acceptPreview(error), "accept table structure preview");
    const auto accepted = canvas.snapshot();
    const bool removed = kind == "table-delete" || kind == "row-delete-active";
    if (removed) check(!canvas.selectedTableId() && !canvas.selectedTableCellRange(), "accepted deletion leaves a stale table/cell cursor");
    else {
        const auto current = canvas.selectedTableCellRange();
        check(current && current->cellId == cell && current->row == (kind == "row-insert" ? 3U : 1U),
              "accepted row structure change moves the cursor into a different stable cell");
    }
    QString failure;
    QObject::connect(&canvas, &app::DocumentCanvas::operationFailed, [&](const QString& detail) { failure = detail; });
    canvas.insertText(QStringLiteral("+"));
    check(failure.isEmpty() && canvas.snapshot().revision != accepted.revision, "typing after accepted table change fails or loses input");
    if (!removed) {
        const auto typed = canvas.snapshot();
        const auto* afterTable = typed.document.findTable(table);
        check(afterTable && afterTable->cell(kind == "row-insert" ? 3U : 1U, 1)->text == u"Kept+",
              "post-preview typing edits a neighbor instead of the retained cell");
    }
    canvas.undo();
    check(canvas.snapshot().document == accepted.document, "one Undo does not remove only post-preview typing");
    canvas.undo();
    const auto restored = canvas.selectedTableCellRange();
    check(canvas.snapshot().document == before.document && restored && restored->cellId == cell && restored->row == 2,
          "Undo accepted preview does not restore the original stable cell and document together");
}
void pageStoryFit() {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 12, 4);
    auto document = canvas.snapshot().document;
    check(bool(document.setHeaderText(std::u16string(160, u'W'))), "create retained header");
    canvas.setDocument(document);
    QString summary, error;
    const auto metadata = app::invokeEditorTool(canvas, QStringLiteral("preview-state"), QStringLiteral("editor_v1_read"),
        {{"documentId", "preview-state"}, {"scope", "metadata"}}, summary, error);
    const auto before = canvas.snapshot();
    const Json operation = {{"kind", "set_page_layout"}, {"expectedLayoutStateId", metadata.at("pageLayout").at("stateId")},
        {"layout", {{"widthPoints", 72}, {"heightPoints", 72}, {"marginTopPoints", 0}, {"marginRightPoints", 0},
                    {"marginBottomPoints", 0}, {"marginLeftPoints", 0}}}};
    check(preview(canvas, Json::array({operation}), error).empty() && !error.isEmpty() && !canvas.hasPreview() &&
              canvas.snapshot().document == before.document && canvas.pageWidthPoints() == 612,
          "page-only proposal accepts newly hidden unchanged header content");
    document = before.document;
    check(bool(document.setHeaderText(std::u16string(2000, u'W'))), "create already-overflowing imported header");
    canvas.setDocument(document);
    const auto live = canvas.snapshot();
    const auto fresh = app::invokeEditorTool(canvas, QStringLiteral("preview-state"), QStringLiteral("editor_v1_read"),
        {{"documentId", "preview-state"}, {"scope", "metadata"}}, summary, error);
    auto expanded = operation;
    expanded["expectedLayoutStateId"] = fresh.at("pageLayout").at("stateId");
    expanded["layout"] = {{"widthPoints", 700}, {"heightPoints", 792}, {"marginTopPoints", 72}, {"marginRightPoints", 72},
                          {"marginBottomPoints", 72}, {"marginLeftPoints", 72}};
    check(!preview(canvas, Json::array({expanded}), error).empty() && error.isEmpty() && canvas.acceptPreview(error),
          "non-worsening geometry cannot retain already overflowing imported source");
    canvas.undo();
    check(canvas.snapshot().document == live.document && canvas.pageWidthPoints() == 612,
          "page-only Undo loses retained imported story content or layout");
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const std::string_view selector = argc > 1 ? argv[1] : "all";
    if (selector == "layout" || selector == "all") pageStoryFit();
    for (const auto* kind : {"table-delete", "row-insert", "row-delete", "row-delete-active"})
        if (selector == kind || selector == "all") tableCursor(kind);
    std::cout << "Codex preview state tests passed\n";
}
