#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/codex/authoring_tools.hpp"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace docxstudio;
using codex::Json;
constexpr auto documentId = "authoring-workflow";
std::set<std::string> exercised;

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

Json invoke(app::DocumentCanvas& canvas, const char* tool, Json arguments, QString& error) {
    QString summary;
    arguments["documentId"] = documentId;
    error.clear();
    return app::invokeEditorTool(canvas, QString::fromLatin1(documentId), QString::fromLatin1(tool), arguments, summary, error);
}

Json propose(app::DocumentCanvas& canvas, const Json& operations, QString& error) {
    const auto metadata = invoke(canvas, "editor_v1_read", {{"scope", "metadata"}}, error);
    check(!metadata.empty() && error.isEmpty() && metadata.contains("revision"), "read current revision before an authoring proposal");
    return invoke(canvas, "editor_v1_preview", {{"baseRevision", metadata.at("revision")},
        {"summary", "Create and edit a native document"}, {"operations", operations}}, error);
}

void accept(app::DocumentCanvas& canvas, const Json& operations) {
    const auto before = canvas.snapshot();
    QString error;
    const auto result = propose(canvas, operations, error);
    if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
    check(!result.empty() && error.isEmpty() && canvas.hasPreview() && result.at("committed") == false,
          "a supported authoring operation did not produce a reviewable Bridge preview");
    check(canvas.snapshot().document == before.document && canvas.snapshot().revision == before.revision,
          "preview changed the authoritative document before acceptance");
    check(canvas.acceptPreview(error), "accept compound authoring preview");
    const auto accepted = canvas.snapshot();
    check(accepted.revision.value() == before.revision.value() + 1, "compound preview was not exactly one transaction");
    canvas.undo();
    check(canvas.snapshot().document == before.document, "Undo did not restore the complete pre-preview document");
    canvas.redo();
    check(canvas.snapshot().document == accepted.document, "Redo did not restore native identities and payloads");
    for (const auto& operation : operations) exercised.insert(operation.at("kind").get<std::string>());
}

Json read(app::DocumentCanvas& canvas, const char* scope) {
    QString error;
    auto result = invoke(canvas, "editor_v1_read", {{"scope", scope}, {"includeFormatting", true}}, error);
    if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
    check(!result.empty() && error.isEmpty(), "read authoring targets through the real Bridge");
    return result;
}

std::string cellIdFromRead(const Json& result, core::NodeId table, std::size_t row, std::size_t column) {
    for (const auto& block : result.at("blocks")) {
        if (block.at("type") != "tableCell") continue;
        const auto& attributes = block.at("attributes");
        if (attributes.at("tableId") == table.toString() && attributes.at("row") == row && attributes.at("column") == column)
            return block.at("id").get<std::string>();
    }
    check(false, "agent cannot discover a newly created stable table cell");
    return {};
}

std::string pngBase64() {
    QImage image(6, 3, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#184C3F")));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "encode workflow picture");
    return bytes.toBase64().toStdString();
}

void saveAs(app::MainWindow& window, const QString& path) {
    auto* action = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(action, "find manual Save As action");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog, "Save As opens a file chooser");
        dialog->selectFile(path);
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection), "select native save destination");
    });
    action->trigger();
    check(QFileInfo::exists(path), "manual Save As did not write a native package");
}

void workflow(const QString& directory) {
    auto heading = core::Paragraph::create(u"Methods").value();
    auto prose = core::Paragraph::create(u"Results").value();
    auto captionAnchor = core::Paragraph::create(u"").value();
    auto sourceAnchor = core::Paragraph::create(u"").value();
    auto tail = core::Paragraph::create(u"Tail").value();
    auto initial = core::Document::create({heading, prose, captionAnchor, sourceAnchor, tail}).value();
    check(bool(initial.setParagraphStyle({heading.id()}, "Heading1")), "create discoverable heading fixture");
    app::MainWindow window;
    window.resize(1280, 860);
    window.show();
    auto* canvas = window.findChild<app::DocumentCanvas*>();
    check(canvas, "find active authoring canvas");
    canvas->setDocument(initial);
    QString error;
    const auto capabilities = invoke(*canvas, "editor_v1_capabilities", Json::object(), error);
    check(!capabilities.empty() && error.isEmpty() && capabilities.at("catalogVersion") == codex::kEditorToolCatalogVersion &&
              capabilities.at("revision") == canvas->snapshot().revision.value() && capabilities.at("groups").contains("tables") &&
              capabilities.at("workflow").at("manualSkillActivationRequired") == false,
          "compound workflow cannot discover authoritative built-in tools without skill activation");
    const auto headerOperation = Json::array({{{"kind", "set_story_text"}, {"story", "header"}, {"text", "Left\tCenter\tRight"}}});
    canvas->beginHeaderFooterEditing(false);
    check(canvas->isHeaderFooterEditing(), "open inline header before an agent edit");
    check(!propose(*canvas, headerOperation, error).empty() && error.isEmpty(), "agent header preview while inline editing");
    check(!canvas->isHeaderFooterEditing() && canvas->snapshot().document == initial,
          "agent preview left a live header editor or committed the story");
    canvas->discardPreview();
    check(!canvas->hasPreview() && canvas->snapshot().document == initial, "discarded agent story changed the live document");
    accept(*canvas, headerOperation);
    const auto beforeOversize = canvas->snapshot();
    check(propose(*canvas, Json::array({{{"kind", "set_story_text"}, {"story", "header"}, {"text", std::string(2048, 'W')}}}), error).empty() &&
              !error.isEmpty() && !canvas->hasPreview() && canvas->snapshot().document == beforeOversize.document,
          "oversized header accepted or partially changed the document");

    const auto tableId = core::NodeId::generate();
    accept(*canvas, Json::array({{{"kind", "insert_table"}, {"target", {{"blockId", tail.id().toString()}, {"start", 0}}},
        {"tableId", tableId.toString()}, {"rows", 2}, {"columns", 2}, {"headerRow", true},
        {"cells", Json::array({Json::array({"Name", "Value"}), Json::array({"First", "42"})})}}}));
    const auto tables = read(*canvas, "tables");
    const auto cellId = cellIdFromRead(tables, tableId, 1, 0);
    const auto otherCellId = cellIdFromRead(tables, tableId, 0, 1);
    const Json target{{"tableId", tableId.toString()}, {"cellId", cellId}};
    Json range = target; range["start"] = 0; range["end"] = 6;
    Json replaced = target; replaced["start"] = 0; replaced["end"] = 5;
    accept(*canvas, Json::array({
        {{"kind", "replace_table_cell_range"}, {"target", replaced}, {"text", "Second"}},
        {{"kind", "set_table_cell_text"}, {"target", {{"tableId", tableId.toString()}, {"cellId", otherCellId}}}, {"text", "Measurement"}},
        {{"kind", "set_table_cell_style"}, {"target", range}, {"style", {{"bold", true}, {"foregroundColor", "#184C3F"}}}},
        {{"kind", "set_table_cell_paragraph_format"}, {"target", target}, {"style", {{"leftIndentPoints", 5}, {"spaceAfterPoints", 3}, {"lineSpacing", 1.5}}}},
        {{"kind", "set_table_cell_alignment"}, {"target", target}, {"horizontal", "right"}, {"vertical", "bottom"}},
        {{"kind", "set_table_style"}, {"tableId", tableId.toString()}, {"style", "medium-green"}},
        {{"kind", "insert_table_row"}, {"tableId", tableId.toString()}, {"index", 1}},
        {{"kind", "insert_table_column"}, {"tableId", tableId.toString()}, {"index", 1}, {"inheritFrom", "following"}},
        {{"kind", "delete_table_rows"}, {"tableId", tableId.toString()}, {"index", 1}, {"count", 1}},
        {{"kind", "delete_table_columns"}, {"tableId", tableId.toString()}, {"index", 1}, {"count", 1}},
        {{"kind", "set_table_geometry"}, {"tableId", tableId.toString()}, {"columnWidthsPoints", {90, 180}}, {"rowMinimumHeightsPoints", {24, 0}}},
        {{"kind", "distribute_table_columns"}, {"tableId", tableId.toString()}},
        {{"kind", "distribute_table_rows"}, {"tableId", tableId.toString()}},
        {{"kind", "set_table_borders"}, {"tableId", tableId.toString()}, {"preset", "all"}, {"widthPoints", 1.25}, {"color", "#184C3F"}}
    }));
    const auto formatted = canvas->snapshot().document;
    const auto* table = formatted.findTable(tableId);
    check(table && table->rowCount() == 2 && table->columnCount() == 2 && table->cell(1, 0)->id.toString() == cellId &&
              table->cell(1, 0)->text == u"Second" && table->cell(1, 0)->characterFormatAt(0).bold.value_or(false) &&
              table->cell(1, 0)->layout.vertical_alignment == core::TableCellVerticalAlignment::bottom &&
              table->geometry().column_widths_twips == std::vector<std::uint32_t>({2700, 2700}),
          "compound table operations lost cell identity, text, formatting or geometry");

    const auto pictureId = core::NodeId::generate(), cellPictureId = core::NodeId::generate(), storyPictureId = core::NodeId::generate();
    const auto picture = [&](Json destination, core::NodeId id, double width, double height) {
        return Json{{"kind", "insert_raster_image"}, {"target", std::move(destination)}, {"imageId", id.toString()},
            {"base64Data", pngBase64()}, {"mimeType", "image/png"}, {"accessibleName", "A green rectangle"},
            {"widthPoints", width}, {"heightPoints", height}};
    };
    accept(*canvas, Json::array({
        picture({{"blockId", prose.id().toString()}, {"start", 0}}, pictureId, 36, 18),
        picture({{"tableId", tableId.toString()}, {"cellId", otherCellId}, {"start", 0}}, cellPictureId, 24, 12),
        picture({{"story", "header"}, {"start", 0}}, storyPictureId, 24, 12),
        {{"kind", "set_story_text"}, {"story", "header"}, {"region", "center"}, {"text", "Page {PAGE} of {PAGES}"}},
        {{"kind", "set_story_text"}, {"story", "footer"}, {"text", "Draft"}},
        {{"kind", "resize_image"}, {"imageId", pictureId.toString()}, {"widthPoints", 60}, {"heightPoints", 30}},
        {{"kind", "set_image_alt_text"}, {"imageId", pictureId.toString()}, {"text", "An accessible result diagram"}},
        {{"kind", "set_image_layout"}, {"imageId", pictureId.toString()}, {"placement", "square"}, {"distanceRightPoints", 4}},
        {{"kind", "insert_equation"}, {"target", {{"blockId", prose.id().toString()}, {"start", 1}}}, {"latex", "x"}},
        {{"kind", "insert_note"}, {"target", {{"blockId", prose.id().toString()}, {"start", 2}}}, {"noteKind", "footnote"}, {"text", "A useful academic note"}},
        {{"kind", "insert_caption"}, {"target", {{"blockId", captionAnchor.id().toString()}, {"start", 0}}}, {"label", "Figure"}, {"objectId", pictureId.toString()}, {"text", "An authoring result"}},
        {{"kind", "insert_bibliography_entry"}, {"target", {{"blockId", sourceAnchor.id().toString()}, {"start", 0}}}, {"text", "Rabadi. A manual source. 2026."}},
        {{"kind", "insert_toc"}, {"target", {{"blockId", heading.id().toString()}, {"start", 0}}}, {"maxLevel", 3}}
    }));
    auto academic = canvas->snapshot().document;
    check(academic.findImage(pictureId) && academic.findImage(cellPictureId) && academic.findImage(storyPictureId) &&
              academic.headerImages().size() == 1 && academic.headerText().find(u"Page {PAGE} of {PAGES}") != std::u16string::npos,
          "compound picture and regional story preview lost native payloads or neighboring content");
    const auto equationId = academic.findParagraph(prose.id())->equations().front().id;
    const auto noteId = academic.notes().front().id;
    const auto caption = std::find_if(academic.paragraphs().begin(), academic.paragraphs().end(), [](const auto& paragraph) { return paragraph.caption().has_value(); });
    const auto entry = std::find_if(academic.paragraphs().begin(), academic.paragraphs().end(), [](const auto& paragraph) { return paragraph.isBibliographyEntry(); });
    check(caption != academic.paragraphs().end() && entry != academic.paragraphs().end() && academic.tablesOfContents().size() == 1,
          "agent did not create native caption/source/TOC semantics");
    const auto captionId = caption->id(), entryId = entry->id();
    accept(*canvas, Json::array({
        {{"kind", "insert_citation"}, {"target", {{"blockId", prose.id().toString()}, {"start", 0}}}, {"entryId", entryId.toString()}, {"locator", "p. 1"}},
        {{"kind", "insert_cross_reference"}, {"target", {{"blockId", prose.id().toString()}, {"start", 1}}}, {"targetId", captionId.toString()}},
        {{"kind", "replace_equation"}, {"equationId", equationId.toString()}, {"latex", "x^2"}, {"display", true}},
        {{"kind", "set_note_text"}, {"noteId", noteId.toString()}, {"text", "An edited academic note"}}
    }));
    academic = canvas->snapshot().document;
    const auto& fields = academic.findParagraph(prose.id())->fields();
    const auto citation = std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.kind == core::FieldKind::citation; });
    check(citation != fields.end(), "accepted citation has no stable field identity");
    const auto splitId = core::NodeId::generate();
    const auto firstBlock = academic.bodyBlocks().front().id;
    accept(*canvas, Json::array({
        {{"kind", "split_paragraph"}, {"target", {{"blockId", tail.id().toString()}, {"start", 2}}}, {"newParagraphId", splitId.toString()}},
        {{"kind", "merge_paragraph"}, {"blockId", tail.id().toString()}},
        {{"kind", "move_block"}, {"blockId", tableId.toString()}, {"beforeBlockId", firstBlock.toString()}},
        {{"kind", "set_academic_field"}, {"fieldId", citation->id.toString()}, {"locator", "pp. 2–3"}},
        {{"kind", "set_caption_metadata"}, {"blockId", captionId.toString()}, {"label", "Table"}, {"objectId", tableId.toString()}},
        {{"kind", "set_bibliography_entry"}, {"blockId", entryId.toString()}, {"enabled", false}},
        {{"kind", "set_bibliography_entry"}, {"blockId", entryId.toString()}, {"enabled", true}}
    }));
    const auto temporaryTable = core::NodeId::generate(), temporaryImage = core::NodeId::generate();
    accept(*canvas, Json::array({{{"kind", "insert_table"}, {"tableId", temporaryTable.toString()}, {"target", {{"blockId", tail.id().toString()}, {"start", 0}}}, {"rows", 1}, {"columns", 1}},
        picture({{"blockId", tail.id().toString()}, {"start", 0}}, temporaryImage, 12, 6)}));
    accept(*canvas, Json::array({{{"kind", "delete_table"}, {"tableId", temporaryTable.toString()}},
        {{"kind", "delete_image"}, {"imageId", temporaryImage.toString()}}}));
    const auto final = canvas->snapshot().document;
    check(final.findParagraph(tail.id())->text() == u"Tail" && !final.findParagraph(splitId) &&
              final.bodyBlocks().front().id == tableId && final.findEquation(equationId)->canonical_latex == "x^{2}" &&
              final.findNote(noteId)->text == u"An edited academic note" && final.findParagraph(captionId)->caption()->label == core::CaptionLabel::table,
          "structure/academic edits did not preserve their native identities and content");
    for (const auto& capability : codex::authoringCapabilities())
        check(exercised.contains(capability.at("kind").get<std::string>()), "a newly advertised authoring operation lacks a real Bridge workflow check");

    // A later invalid operation must not publish the earlier valid operation.
    const auto valid = canvas->snapshot();
    check(propose(*canvas, Json::array({{{"kind", "set_story_text"}, {"story", "footer"}, {"text", "Must not appear"}},
        {{"kind", "resize_image"}, {"imageId", core::NodeId::generate().toString()}, {"widthPoints", 12}, {"heightPoints", 6}}}), error).empty() &&
              !error.isEmpty() && !canvas->hasPreview() && canvas->snapshot().document == valid.document,
          "failed compound Bridge authoring left a partial edit or preview");

    std::string codecError;
    app::RecoveryPageLayout page{canvas->pageWidthPoints(), canvas->pageHeightPoints(), canvas->marginTopPoints(),
        canvas->marginRightPoints(), canvas->marginBottomPoints(), canvas->marginLeftPoints()};
    const auto encoded = app::RecoveryCodec::encode({final, page}, codecError);
    check(encoded.has_value(), "encode compound agent recovery snapshot");
    const auto decoded = app::RecoveryCodec::decode(*encoded, codecError);
    check(decoded && decoded->document == final && decoded->page == page,
          "recovery changed compound agent IDs, layouts, academic fields or image bytes");
    const auto path = directory + QStringLiteral("/agent-authored.docx");
    saveAs(window, path);
    check(!canvas->isModified(), "manual save did not complete the compound agent document");
    const auto package = ooxml::DocxDocument::open(path.toStdString());
    check(package && package->academicRecovery(), "native package has no verified semantic recovery proof");
    app::MainWindow reopened;
    check(reopened.openPath(path), "reopen compound agent-authored DOCX");
    auto* restored = reopened.findChild<app::DocumentCanvas*>();
    check(restored && restored->snapshot().document == final,
          "native DOCX reopen changed compound agent content, stable identities or image bytes");
    window.findChild<QAction*>(QStringLiteral("file.save"))->trigger();
    check(!canvas->isModified(), "a repeated manual save introduced an agent document error");
    canvas->markSaved();
}
}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir directory;
    check(directory.isValid(), "create private authoring workflow directory");
    qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME", directory.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("OwlDocsTests");
    QCoreApplication::setApplicationName("CodexAuthoringWorkflow");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible()) message->accept();
    });
    dismiss.start(1);
    workflow(directory.path());
    std::cout << "Codex authoring Bridge/native/recovery workflow passed\n";
}
