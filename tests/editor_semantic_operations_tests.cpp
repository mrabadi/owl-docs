#include "docxstudio/app/EditorSemanticOperations.h"
#include "docxstudio/codex/authoring_tools.hpp"
#include "docxstudio/core/academic.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QImage>
#include <cstdlib>
#include <iostream>

using namespace docxstudio;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void tables() {
    core::Document document;
    const auto paragraph = document.paragraphs().front().id();
    std::vector<core::Operation> operations;
    std::set<std::string> affected;
    QString error;
    check(app::handlesSemanticOperation("insert_table"), "agent cannot create tables");
    check(app::appendSemanticOperation(document,
        {{"kind", "insert_table"}, {"target", {{"blockId", paragraph.toString()}, {"start", 0}}},
         {"rows", 2}, {"columns", 2}, {"headerRow", true},
         {"cells", codex::Json::array({codex::Json::array({"Name", "Value"}), codex::Json::array({"First", "42"})})}},
        operations, affected, error), "agent table insertion failed");
    check(document.tables().size() == 1 && document.tables().front().hasHeaderRow(), "table insertion lost shape/header");
    const auto& table = document.tables().front();
    const auto tableId = table.id();
    const auto cellId = table.cell(1, 0)->id;
    check(table.cell(0, 1)->text == u"Value", "table insertion lost cells");
    check(app::appendSemanticOperation(document,
        {{"kind", "replace_table_cell_range"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}, {"start", 0}, {"end", 5}}}, {"text", "Second"}},
        operations, affected, error), "stable cell range replacement failed");
    check(document.findTable(tableId)->cell(1, 0)->text == u"Second", "stable cell replacement changed wrong content");
    const auto before = document;
    const auto previousOperations = operations.size();
    check(!app::appendSemanticOperation(document,
        {{"kind", "replace_table_cell_range"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}, {"start", 0}, {"end", 500}}}, {"text", "bad"}},
        operations, affected, error) && document == before && operations.size() == previousOperations,
        "invalid table range partially mutated plan");
    const auto schemas = codex::authoringOperationSchemas();
    check(!schemas.empty() && !codex::authoringCapabilities().empty(), "agent table schema/capability absent");
    core::Document rejectedDocument;
    const auto rejectedParagraph = rejectedDocument.paragraphs().front().id();
    check(bool(rejectedDocument.insertText({rejectedParagraph, 0}, u"LeadTail")), "atomic insertion fixture");
    const auto beforeRejected = rejectedDocument;
    const auto beforeAffected = affected;
    check(!app::appendSemanticOperation(rejectedDocument,
        {{"kind", "insert_table"}, {"target", {{"blockId", rejectedParagraph.toString()}, {"start", 4}}},
         {"rows", 2}, {"columns", 2}, {"cells", codex::Json::array({codex::Json::array({"Only", "one row"})})}},
        operations, affected, error) && rejectedDocument == beforeRejected && affected == beforeAffected,
        "failed table insertion leaked its paragraph split or affected identities");
    check(!app::appendSemanticOperation(rejectedDocument,
        {{"kind", "set_story_text"}, {"story", "header"}, {"text", "Safe"}, {"filePath", "/tmp/unsolicited"}},
        operations, affected, error) && rejectedDocument == beforeRejected, "unknown operation properties accepted");
    check(!app::appendSemanticOperation(rejectedDocument,
        {{"kind", "set_story_text"}, {"story", "header"}, {"text", std::string(1, static_cast<char>(0xff))}},
        operations, affected, error) && rejectedDocument == beforeRejected, "invalid UTF-8 was silently repaired");
}
void tableFormatting() {
    core::Document document;
    auto made = core::Table::create(2, 2);
    check(bool(made), "formatting fixture");
    const auto tableId = made.value().id();
    const auto cellId = made.value().cell(0, 0)->id;
    check(bool(document.insertTable(std::nullopt, std::move(made.value()))), "formatting table fixture");
    std::vector<core::Operation> operations;
    std::set<std::string> affected;
    QString error;
    const auto apply = [&](const codex::Json& source) {
        const bool result = app::appendSemanticOperation(document, source, operations, affected, error);
        if (!result) std::cerr << error.toStdString() << '\n';
        return result;
    };
    check(app::handlesSemanticOperation("set_table_geometry"), "agent cannot format tables");
    check(apply({{"kind", "set_table_geometry"}, {"tableId", tableId.toString()}, {"columnWidthsPoints", {90, 180}}, {"rowMinimumHeightsPoints", {45, 0}}}), "table geometry preview failed");
    check(apply({{"kind", "set_table_style"}, {"tableId", tableId.toString()}, {"style", "medium-green"}}), "table style failed");
    check(apply({{"kind", "set_table_cell_text"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}}}, {"text", "Aligned"}}), "table cell text failed");
    check(apply({{"kind", "set_table_cell_style"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}, {"start", 0}, {"end", 7}}}, {"style", {{"bold", true}, {"foregroundColor", "#184C3F"}}}}), "table cell style failed");
    check(apply({{"kind", "set_table_cell_alignment"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}}}, {"horizontal", "right"}, {"vertical", "bottom"}}), "table cell alignment failed");
    check(apply({{"kind", "set_table_cell_paragraph_format"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}}}, {"style", {{"lineSpacing", 1.5}, {"spaceAfterPoints", 8}, {"leftIndentPoints", 10}}}}), "table cell paragraph formatting failed");
    check(apply({{"kind", "set_table_borders"}, {"tableId", tableId.toString()}, {"cellIds", {cellId.toString()}}, {"preset", "right"}, {"widthPoints", 1.5}, {"color", "#184C3F"}}), "table borders failed");
    const auto* table = document.findTable(tableId);
    check(table->geometry().column_widths_twips == std::vector<std::uint32_t>({1800, 3600}) && table->geometry().row_minimum_heights_twips == std::vector<std::uint32_t>({900, 0}), "table geometry units incorrect");
    check(table->style() == core::TableStyle::medium_green && table->cell(0, 0)->layout.vertical_alignment == core::TableCellVerticalAlignment::bottom && table->cell(0, 0)->paragraph_format.alignment == core::ParagraphAlignment::right, "table appearance lost");
    check(table->cell(0, 0)->layout.border_right == table->cell(0, 1)->layout.border_left && table->cell(0, 0)->layout.border_right->width_eighth_points == 12, "shared table border not mirrored");
    check(table->cell(0, 0)->characterFormatAt(0).bold == true, "cell formatting missing");
    check(table->cell(0, 0)->paragraph_format.left_indent_emu == 127000 && table->cell(0, 0)->paragraph_format.space_after_emu == 101600 && table->cell(0, 0)->paragraph_format.line_spacing_emu == 228600, "cell paragraph formatting/units missing");
    check(apply({{"kind", "insert_table_row"}, {"tableId", tableId.toString()}, {"index", 1}, {"inheritFrom", "preceding"}}), "insert table row failed");
    check(apply({{"kind", "insert_table_column"}, {"tableId", tableId.toString()}, {"index", 1}, {"inheritFrom", "following"}}), "insert table column failed");
    check(document.findTable(tableId)->rowCount() == 3 && document.findTable(tableId)->columnCount() == 3 && document.findTable(tableId)->cell(0, 0)->id == cellId, "table structure lost stable cells");
    check(apply({{"kind", "delete_table_rows"}, {"tableId", tableId.toString()}, {"index", 1}, {"count", 1}}) && apply({{"kind", "delete_table_columns"}, {"tableId", tableId.toString()}, {"index", 1}, {"count", 1}}), "delete table structure failed");
    check(apply({{"kind", "distribute_table_columns"}, {"tableId", tableId.toString()}}) && apply({{"kind", "distribute_table_rows"}, {"tableId", tableId.toString()}}), "distribute table dimensions failed");
    check(document.findTable(tableId)->geometry().column_widths_twips == std::vector<std::uint32_t>({2700, 2700}) && document.findTable(tableId)->geometry().row_minimum_heights_twips == std::vector<std::uint32_t>({450, 450}), "table dimensions not distributed");
    const auto before = document;
    const auto oldCount = operations.size();
    check(!apply({{"kind", "set_table_geometry"}, {"tableId", tableId.toString()}, {"columnWidthsPoints", {0, 100}}}) && document == before && operations.size() == oldCount, "invalid geometry mutated working plan");
    check(!apply({{"kind", "set_table_cell_alignment"}, {"target", {{"tableId", tableId.toString()}, {"cellId", cellId.toString()}}}, {"horizontal", "bogus"}, {"vertical", "bottom"}}) && document == before, "invalid alignment accepted");
    check(apply({{"kind", "delete_table"}, {"tableId", tableId.toString()}}) && document.tables().empty(), "table deletion unavailable");
}
void imagesAndStories() {
    core::Document document;
    const auto paragraphId = document.paragraphs().front().id();
    auto made = core::Table::create(1, 1);
    check(bool(made), "picture table fixture");
    const auto tableId = made.value().id();
    const auto cellId = made.value().cell(0, 0)->id;
    check(bool(document.insertTable(std::nullopt, std::move(made.value()))), "picture table insertion fixture");
    check(bool(document.setHeaderText(u"Left\tCenter\tRight")), "story fixture");
    QImage image(4, 2, QImage::Format_ARGB32);
    image.fill(QColor(QStringLiteral("#184C3F")));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "picture fixture encoding");
    const std::string base64 = bytes.toBase64().toStdString();
    std::vector<core::Operation> operations;
    std::set<std::string> affected;
    QString error;
    const auto apply = [&](const codex::Json& source) {
        const bool result = app::appendSemanticOperation(document, source, operations, affected, error);
        if (!result) std::cerr << error.toStdString() << '\n';
        return result;
    };
    check(app::handlesSemanticOperation("insert_raster_image"), "agent cannot insert or edit pictures/stories");
    const auto insert = [&](codex::Json target) {
        return apply({{"kind", "insert_raster_image"}, {"target", std::move(target)}, {"base64Data", base64}, {"mimeType", "image/png"}, {"accessibleName", "A green rectangle"}, {"widthPoints", 72}, {"heightPoints", 36}});
    };
    check(insert({{"blockId", paragraphId.toString()}, {"start", 0}}), "body picture insertion failed");
    check(insert({{"tableId", tableId.toString()}, {"cellId", cellId.toString()}, {"start", 0}}), "cell picture insertion failed");
    check(insert({{"story", "header"}, {"start", 0}}), "story picture insertion failed");
    const auto bodyImage = document.findParagraph(paragraphId)->images().front().id;
    const auto cellImage = document.findTable(tableId)->cell(0, 0)->images.front().id;
    const auto storyImage = document.headerImages().front().id;
    check(apply({{"kind", "resize_image"}, {"imageId", bodyImage.toString()}, {"widthPoints", 144}, {"heightPoints", 72}}), "picture resizing failed");
    check(apply({{"kind", "set_image_alt_text"}, {"imageId", bodyImage.toString()}, {"text", "Updated accessible picture"}}), "picture alt text failed");
    check(apply({{"kind", "set_image_layout"}, {"imageId", bodyImage.toString()}, {"placement", "square"}, {"distanceTopPoints", 4}, {"moveWithText", false}}), "picture wrapping failed");
    check(document.findImage(bodyImage)->width_emu == 144 * 12700 && document.findImage(bodyImage)->layout.placement == core::ImagePlacement::square && document.findImage(bodyImage)->accessible_name == "Updated accessible picture", "picture properties lost");
    const auto before = document;
    check(!apply({{"kind", "set_image_layout"}, {"imageId", cellImage.toString()}, {"placement", "square"}}) && document == before, "unsupported floating cell picture accepted");
    check(!apply({{"kind", "insert_raster_image"}, {"target", {{"blockId", paragraphId.toString()}, {"start", 0}}}, {"base64Data", "not base64"}, {"mimeType", "image/png"}, {"accessibleName", "bad"}, {"widthPoints", 72}, {"heightPoints", 36}}) && document == before, "malformed raster payload accepted");
    check(!apply({{"kind", "insert_raster_image"}, {"target", {{"blockId", paragraphId.toString()}, {"start", 0}}}, {"base64Data", std::string(5592408, 'A')}, {"mimeType", "image/png"}, {"accessibleName", "oversize"}, {"widthPoints", 72}, {"heightPoints", 36}}) && document == before && error.contains(QStringLiteral("bounded base64")), "decoded raster payload exceeded the 4 MiB tool budget");
    check(apply({{"kind", "set_story_text"}, {"story", "header"}, {"region", "center"}, {"text", "Page {PAGE} of {PAGES}"}}), "header region editing failed");
    check(document.headerText() == u"\ufffcLeft\tPage {PAGE} of {PAGES}\tRight" && document.headerImages().front().id == storyImage, "region edit damaged neighboring story picture/text");
    check(apply({{"kind", "set_story_text"}, {"story", "footer"}, {"text", "Confidential"}}) && document.footerText() == u"Confidential", "footer editing failed");
    check(apply({{"kind", "set_story_text"}, {"story", "footer"}, {"region", "center"}, {"text", "Second line\n{PAGE}"}}) && document.footerText() == u"Confidential\tSecond line\n{PAGE}\t", "new multiline story region corrupted existing text");
    check(apply({{"kind", "delete_image"}, {"imageId", bodyImage.toString()}}) && apply({{"kind", "delete_image"}, {"imageId", cellImage.toString()}}) && apply({{"kind", "delete_image"}, {"imageId", storyImage.toString()}}), "image deletion failed for one story type");
    check(document.findParagraph(paragraphId)->text().empty() && document.findTable(tableId)->cell(0, 0)->text.empty() && document.headerText() == u"Left\tPage {PAGE} of {PAGES}\tRight", "image deletion removed neighboring content");
}
void structureAndAcademic() {
    auto heading = core::Paragraph::create(u"Heading");
    auto source = core::Paragraph::create(u"Source");
    auto caption = core::Paragraph::create(u"Diagram");
    check(bool(heading) && bool(source) && bool(caption), "structure paragraph fixtures");
    const auto headingId = heading.value().id(), sourceId = source.value().id(), captionId = caption.value().id();
    auto made = core::Document::create({heading.value(), source.value(), caption.value()});
    check(bool(made), "structure document fixture");
    auto document = std::move(made.value());
    check(bool(document.setParagraphStyle({headingId}, "Heading1")) && bool(document.setBibliographyEntry(sourceId, true)), "academic role fixture");
    const auto referenceId = core::NodeId::generate(), citationId = core::NodeId::generate();
    check(bool(document.insertField({headingId, 0}, {referenceId, 0, core::FieldKind::cross_reference, captionId})) &&
        bool(document.insertField({headingId, 1}, {citationId, 1, core::FieldKind::citation, sourceId, u"p. 1"})), "academic field fixture");
    check(bool(document.setCaption(captionId, core::CaptionMetadata{core::CaptionLabel::figure, headingId})), "caption role fixture");
    std::vector<core::Operation> operations;
    std::set<std::string> affected;
    QString error;
    const auto apply = [&](const codex::Json& operation) {
        const bool result = app::appendSemanticOperation(document, operation, operations, affected, error);
        if (!result) std::cerr << error.toStdString() << '\n';
        return result;
    };
    check(app::handlesSemanticOperation("split_paragraph"), "agent cannot edit document structure/academic metadata");
    const auto newId = core::NodeId::generate();
    check(apply({{"kind", "split_paragraph"}, {"target", {{"blockId", sourceId.toString()}, {"start", 3}}}, {"newParagraphId", newId.toString()}}), "paragraph split failed");
    check(document.findParagraph(sourceId)->text() == u"Sou" && document.findParagraph(newId)->text() == u"rce", "paragraph split lost content/identity");
    check(apply({{"kind", "move_block"}, {"blockId", captionId.toString()}, {"beforeBlockId", headingId.toString()}}) && document.bodyBlocks().front().id == captionId, "body block move failed");
    check(apply({{"kind", "merge_paragraph"}, {"blockId", sourceId.toString()}}) && document.findParagraph(sourceId)->text() == u"Source" && !document.findParagraph(newId), "paragraph merge failed");
    check(apply({{"kind", "set_academic_field"}, {"fieldId", citationId.toString()}, {"locator", "pp. 2–3"}}), "citation editing failed");
    check(apply({{"kind", "set_academic_field"}, {"fieldId", referenceId.toString()}, {"format", "full_text"}}), "cross reference editing failed");
    check(document.findField(citationId)->locator == u"pp. 2–3" && document.findField(referenceId)->reference_format == core::CrossReferenceFormat::full_text, "academic edit changed field identity/value");
    check(apply({{"kind", "set_caption_metadata"}, {"blockId", captionId.toString()}, {"label", "Table"}}) && document.findParagraph(captionId)->caption()->label == core::CaptionLabel::table, "caption label editing failed");
    check(apply({{"kind", "set_bibliography_entry"}, {"blockId", sourceId.toString()}, {"enabled", false}}) && !document.findParagraph(sourceId)->isBibliographyEntry(), "bibliography role editing failed");
    check(apply({{"kind", "set_bibliography_entry"}, {"blockId", sourceId.toString()}, {"enabled", true}}) && document.findParagraph(sourceId)->isBibliographyEntry(), "bibliography role reenable failed");
    check(document.findParagraph(sourceId)->fields().size() == 1 && document.findParagraph(sourceId)->fields().front().kind == core::FieldKind::bibliography_number && document.findParagraph(sourceId)->fields().front().target_id == sourceId, "bibliography role did not create native numbering field");
    const auto before = document;
    check(!apply({{"kind", "set_academic_field"}, {"fieldId", citationId.toString()}, {"targetId", "bogus"}}) && document == before, "invalid academic target mutated plan");
}
void createdCellIdentities() {
    core::Document document;
    const auto paragraph = document.paragraphs().front().id();
    const auto table = core::NodeId::generate(), first = core::NodeId::generate(), second = core::NodeId::generate();
    std::vector<core::Operation> operations;
    std::set<std::string> affected;
    QString error;
    check(app::appendSemanticOperation(document,
        {{"kind", "insert_table"}, {"target", {{"blockId", paragraph.toString()}, {"start", 0}}},
         {"rows", 1}, {"columns", 2}, {"tableId", table.toString()},
         {"cellIds", codex::Json::array({codex::Json::array({first.toString(), second.toString()})})},
         {"cells", codex::Json::array({codex::Json::array({"Label", "Value"})})}},
        operations, affected, error), "agent cannot address newly created cells in the same preview");
    check(app::appendSemanticOperation(document,
        {{"kind", "set_table_cell_style"}, {"target", {{"tableId", table.toString()}, {"cellId", first.toString()}, {"start", 0}, {"end", 5}}}, {"style", {{"bold", true}}}},
        operations, affected, error) && document.findTable(table)->cell(0, 0)->id == first && document.findTable(table)->cell(0, 0)->characterFormatAt(0).bold == true,
        "new-cell identity was lost before formatting");
    const auto before = document;
    const auto count = operations.size();
    check(!app::appendSemanticOperation(document,
        {{"kind", "insert_table"}, {"target", {{"blockId", paragraph.toString()}, {"start", 0}}}, {"rows", 1}, {"columns", 2},
         {"cellIds", codex::Json::array({codex::Json::array({first.toString(), first.toString()})})}},
        operations, affected, error) && document == before && operations.size() == count, "duplicate requested cell identities mutated plan");
}
}
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "format") tableFormatting();
    else if (argc > 1 && std::string(argv[1]) == "images") imagesAndStories();
    else if (argc > 1 && std::string(argv[1]) == "structure") structureAndAcademic();
    else if (argc > 1 && std::string(argv[1]) == "new_cells") createdCellIdentities();
    else { tables(); tableFormatting(); imagesAndStories(); structureAndAcademic(); createdCellIdentities(); }
    std::cout << "Editor semantic operation tests passed\n";
}
