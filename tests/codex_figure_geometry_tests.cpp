#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
using namespace docxstudio;
namespace {
void check(bool value, const char* message) { if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
QByteArray figurePng(QString& error) {
    QImage raster(800, 400, QImage::Format_RGB32); raster.fill(Qt::green);
    QByteArray png; QBuffer buffer(&png);
    check(buffer.open(QIODevice::WriteOnly) && raster.save(&buffer, "PNG"), "PNG fixture failed");
    const QByteArray scene = R"({"type":"excalidraw","version":2,"elements":[{"id":"box","type":"rectangle","x":0,"y":0,"width":100,"height":50}],"appState":{},"files":{}})";
    return app::pngWithExcalidrawScene(png, scene, error);
}
codex::Json figureOperation(core::NodeId cell, core::NodeId image) {
    return {{"kind", "insert_excalidraw_figure"}, {"imageId", image.toString()},
        {"target", {{"blockId", cell.toString()}, {"start", 0}}},
        {"figure", {{"elements", codex::Json::array({{{"id", "box"}, {"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", 100}, {"height", 50}}})}}}};
}
void geometry(app::SpellChecker& spelling, const std::string& mode) {
    std::cout << "Figure geometry route: " << mode << std::endl;
    app::DocumentCanvas native(spelling), agent(spelling);
    QString error, summary;
    const auto embedded = figurePng(error);
    check(!embedded.isEmpty(), "scene fixture failed");
    const auto* first = reinterpret_cast<const std::uint8_t*>(embedded.constData());
    const std::vector<std::uint8_t> bytes(first, first + embedded.size());
    const auto imageId = core::NodeId::generate();
    core::NodeId tableId, cellId;
    codex::Json operations = codex::Json::array();
    std::int64_t expectedWidth = 0;
    const bool batch = mode != "default" && mode != "unequal";
    codex::Json lateLayout;
    if (!batch) {
        check(native.insertTable(1, 2, false), "native table fixture failed");
        tableId = native.snapshot().document.tables().front().id();
        if (mode == "unequal") {
            check(native.activateTableCell(tableId, 0, 0) && native.setSelectedTableColumnWidth(100), "native unequal columns failed");
            expectedWidth = 90 * 12700;
        }
        const auto empty = native.snapshot();
        cellId = empty.document.findTable(tableId)->cell(0, 0)->id;
        agent.setDocument(empty.document);
        check(native.activateTableCell(tableId, 0, 0) && native.insertInlineImage(bytes, QStringLiteral("Native cell figure")), "native image insertion failed");
        const auto nativeInserted = native.snapshot();
        const auto* nativeImage = nativeInserted.document.findImage(nativeInserted.document.findTable(tableId)->cell(0, 0)->images.front().id);
        check(nativeImage != nullptr, "native image fixture disappeared");
        const auto nativeWidth = nativeImage->width_emu;
        if (mode == "unequal") check(nativeWidth == expectedWidth, "native insertion ignored actual authored column width");
        else expectedWidth = nativeWidth;
    } else {
        tableId = core::NodeId::generate(); cellId = core::NodeId::generate();
        const auto nextCell = core::NodeId::generate();
        if (mode == "layout" || mode == "late-layout") {
            const auto metadata = app::invokeEditorTool(agent, QStringLiteral("cell-fit"), QStringLiteral("editor_v1_read"),
                {{"documentId", "cell-fit"}, {"scope", "metadata"}}, summary, error);
            check(!metadata.empty() && error.isEmpty(), "read layout state failed");
            codex::Json change{{"kind", "set_page_layout"}, {"expectedLayoutStateId", metadata.at("pageLayout").at("stateId")},
                {"layout", {{"widthPoints", 612}, {"heightPoints", 792}, {"marginTopPoints", 72}, {"marginRightPoints", 120}, {"marginBottomPoints", 72}, {"marginLeftPoints", 120}}}};
            if (mode == "late-layout") lateLayout = std::move(change);
            else operations.push_back(std::move(change));
        }
        operations.push_back({{"kind", "insert_table"}, {"tableId", tableId.toString()},
            {"cellIds", codex::Json::array({codex::Json::array({cellId.toString(), nextCell.toString()})})},
            {"rows", 1}, {"columns", 2}, {"target", {{"blockId", agent.snapshot().document.paragraphs().front().id().toString()}, {"start", 0}}}});
        operations.push_back({{"kind", "set_table_geometry"}, {"tableId", tableId.toString()}, {"columnWidthsPoints", {100, 300}}});
        expectedWidth = (mode == "layout" || mode == "late-layout" ? 83 : mode == "late-geometry" ? 290 : 90) * 12700;
    }
    operations.push_back(figureOperation(cellId, imageId));
    if (mode == "late-geometry")
        operations.push_back({{"kind", "set_table_geometry"}, {"tableId", tableId.toString()}, {"columnWidthsPoints", {300, 100}}});
    if (!lateLayout.is_null()) operations.push_back(std::move(lateLayout));
    agent.markSaved(); const auto before = agent.snapshot();
    const app::ExcalidrawRenderer renderer = [&](const QJsonArray&, QString& renderError) -> std::optional<app::ExcalidrawFigureEditor::Result> {
        renderError.clear(); return app::ExcalidrawFigureEditor::Result{embedded, *app::excalidrawSceneFromPng(embedded)};
    };
    const auto preview = [&] {
        error.clear();
        const auto result = app::invokeEditorTool(agent, QStringLiteral("cell-fit"), QStringLiteral("editor_v1_preview"),
            {{"documentId", "cell-fit"}, {"baseRevision", agent.snapshot().revision.value()}, {"summary", "Fit figure to cell"}, {"operations", operations}}, summary, error, renderer);
        if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
        check(!result.empty() && error.isEmpty() && agent.hasPreview() && agent.snapshot().document == before.document && !agent.isModified(),
            "cell geometry preview failed or mutated live content");
    };
    preview(); agent.discardPreview();
    check(agent.snapshot().document == before.document && !agent.isModified(), "cell fit discard changed live document");
    preview(); check(agent.acceptPreview(error), "cell fit acceptance failed");
    const auto accepted = agent.snapshot(); const auto* actual = accepted.document.findImage(imageId);
    check(actual && actual->width_emu == expectedWidth, "default tool cell figure stores oversized dimensions instead of native cell fit");
    check(std::abs(2 * actual->height_emu - actual->width_emu) <= 1, "cell fitting stretched the figure aspect ratio");
    check(app::excalidrawSceneFromPng(QByteArray(reinterpret_cast<const char*>(actual->encoded_payload.bytes().data()),
        static_cast<qsizetype>(actual->encoded_payload.size()))).has_value(), "cell fitting lost editable native source");
    agent.undo(); check(agent.snapshot().document == before.document && !agent.isModified() && agent.marginLeftPoints() == 72,
        "one Undo did not restore cell insertion and proposed geometry");
    agent.redo(); check(agent.snapshot().document == accepted.document, "Redo did not restore fitted dimensions and source");
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv); app::SpellChecker spelling;
    for (const std::string mode : {"default", "unequal", "batch", "layout", "late-geometry", "late-layout"})
        if (argc == 1 || mode == argv[1]) geometry(spelling, mode);
    std::cout << "Codex figure geometry tests passed\n";
}
