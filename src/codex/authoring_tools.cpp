#include "docxstudio/codex/authoring_tools.hpp"

#include <utility>

namespace docxstudio::codex {
namespace {
Json reference(const char* name) { return {{"$ref", std::string("#/$defs/") + name}}; }
Json identifier() { return {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}; }
Json count(std::size_t maximum, std::size_t minimum = 0) {
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}
Json text(std::size_t maximum = 32768) { return {{"type", "string"}, {"maxLength", maximum}}; }
Json choices(std::initializer_list<const char*> values) {
    Json entries = Json::array();
    for (const auto* value : values) entries.push_back(value);
    return {{"type", "string"}, {"enum", std::move(entries)}};
}
Json points(double maximum = 1584, double minimum = 0) { return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}; }
Json array(Json items, std::size_t maximum) { return {{"type", "array"}, {"maxItems", maximum}, {"items", std::move(items)}}; }
Json object(Json properties, Json required) {
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}
Json operation(const char* kind, const char* group, const char* description, Json properties, Json required) {
    properties["kind"] = {{"const", kind}};
    required.push_back("kind");
    auto result = object(std::move(properties), std::move(required));
    result["description"] = description;
    result["x-group"] = group;
    return result;
}
}
Json authoringOperationSchemas() {
    Json result = Json::array();
    result.push_back(operation("insert_table", "tables", "Insert a bounded rectangular table at a body paragraph position. Optional tableId and rectangular cellIds let later operations in the same preview address the new table/cells.",
        {{"target", reference("semanticTarget")}, {"rows", count(50, 1)}, {"columns", count(20, 1)},
         {"headerRow", {{"type", "boolean"}}}, {"tableId", identifier()},
         {"cellIds", array(array(identifier(), 20), 50)},
         {"cells", {{"type", "array"}, {"maxItems", 50}, {"items", {{"type", "array"}, {"maxItems", 20}, {"items", text()}}}}}},
        Json::array({"target", "rows", "columns"})));
    result.push_back(operation("replace_table_cell_range", "tables", "Replace one UTF-16 range in a stable table cell, retaining unrelated content and formatting.",
        {{"target", reference("cellRangeTarget")}, {"text", text()}}, Json::array({"target", "text"})));
    result.push_back(operation("set_table_cell_text", "tables", "Replace the text of one stable cell; unrelated cells are retained.",
        {{"target", reference("cellTarget")}, {"text", text()}}, Json::array({"target", "text"})));
    result.push_back(operation("set_table_cell_style", "tables", "Format a UTF-16 range within one stable cell.",
        {{"target", reference("cellRangeTarget")}, {"style", reference("semanticTextStyle")}}, Json::array({"target", "style"})));
    result.push_back(operation("set_table_cell_paragraph_format", "tables", "Set supported paragraph alignment, indents, line/paragraph spacing and keep flags in one stable cell.",
        {{"target", reference("cellTarget")}, {"style", reference("semanticParagraphFormat")}}, Json::array({"target", "style"})));
    result.push_back(operation("set_table_cell_alignment", "tables", "Set horizontal and vertical positioning of one stable cell.",
        {{"target", reference("cellTarget")}, {"horizontal", choices({"left", "center", "right", "justify"})},
         {"vertical", choices({"top", "center", "bottom"})}}, Json::array({"target", "horizontal", "vertical"})));
    result.push_back(operation("set_table_style", "tables", "Apply one of OwlDocs's supported native table styles.",
        {{"tableId", identifier()}, {"style", choices({"plain", "grid", "light-gray", "light-blue", "light-orange", "medium-blue", "medium-green", "medium-orange", "aubergine", "orange-accent", "banded-blue", "banded-aubergine", "dark-header"})}}, Json::array({"tableId", "style"})));
    result.push_back(operation("set_table_geometry", "tables", "Set point-based column widths and minimum row heights. Empty arrays restore automatic dimensions; row minimum zero is Auto.",
        {{"tableId", identifier()}, {"columnWidthsPoints", array(points(1584, 0.05), 20)},
         {"rowMinimumHeightsPoints", array(points(), 50)}}, Json::array({"tableId"})));
    for (const auto* axis : {"row", "column"}) {
        const auto kind = std::string("insert_table_") + axis;
        result.push_back(operation(kind.c_str(), "tables", "Insert one table row or column, retaining stable identities and inheriting an adjacent row/column.",
            {{"tableId", identifier()}, {"index", count(axis == std::string("row") ? 50 : 20)}, {"inheritFrom", choices({"preceding", "following"})}}, Json::array({"tableId", "index"})));
    }
    for (const auto* axis : {"rows", "columns"}) {
        const auto kind = std::string("delete_table_") + axis;
        result.push_back(operation(kind.c_str(), "tables", "Delete contiguous table rows or columns, retaining at least one.",
            {{"tableId", identifier()}, {"index", count(axis == std::string("rows") ? 49 : 19)}, {"count", count(axis == std::string("rows") ? 49 : 19, 1)}}, Json::array({"tableId", "index", "count"})));
    }
    result.push_back(operation("distribute_table_columns", "tables", "Evenly distribute column widths. For an automatic grid, supply totalWidthPoints from the available page width.",
        {{"tableId", identifier()}, {"totalWidthPoints", points(1584, 1)}}, Json::array({"tableId"})));
    result.push_back(operation("distribute_table_rows", "tables", "Evenly distribute authored row minima; optional minimumHeightPoints sets their common minimum. Auto rows remain content-growing.",
        {{"tableId", identifier()}, {"minimumHeightPoints", points()}}, Json::array({"tableId"})));
    result.push_back(operation("set_table_borders", "tables", "Apply solid/none borders to the whole table or a rectangular selection of stable cell IDs. Shared edges are updated together.",
        {{"tableId", identifier()}, {"cellIds", array(identifier(), 1000)}, {"preset", choices({"all", "outside", "inside", "none", "top", "bottom", "left", "right"})},
         {"widthPoints", points(12, 0.125)}, {"color", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}}}, Json::array({"tableId", "preset"})));
    result.push_back(operation("delete_table", "tables", "Delete one semantic table.", {{"tableId", identifier()}}, Json::array({"tableId"})));
    result.push_back(operation("insert_raster_image", "pictures", "Insert bounded base64 PNG/JPEG bytes at a body, stable-cell or header/footer story position. No file paths are accepted.",
        {{"target", reference("imageTarget")}, {"base64Data", text(5592408)}, {"mimeType", choices({"image/png", "image/jpeg"})},
         {"accessibleName", text(4096)}, {"imageId", identifier()}, {"widthPoints", points(20000, 0.01)}, {"heightPoints", points(20000, 0.01)}},
        Json::array({"target", "base64Data", "mimeType", "accessibleName", "widthPoints", "heightPoints"})));
    result.push_back(operation("resize_image", "pictures", "Resize an existing native body, cell or story picture by stable identity.",
        {{"imageId", identifier()}, {"widthPoints", points(20000, 0.01)}, {"heightPoints", points(20000, 0.01)}}, Json::array({"imageId", "widthPoints", "heightPoints"})));
    result.push_back(operation("set_image_alt_text", "pictures", "Set accessible text without changing picture identity or source bytes.",
        {{"imageId", identifier()}, {"text", text(4096)}}, Json::array({"imageId", "text"})));
    result.push_back(operation("set_image_layout", "pictures", "Set supported body picture wrapping. Cell and story pictures remain inline.",
        {{"imageId", identifier()}, {"placement", choices({"inline", "square", "topBottom"})}, {"moveWithText", {{"type", "boolean"}}},
         {"distanceTopPoints", points(20000)}, {"distanceRightPoints", points(20000)}, {"distanceBottomPoints", points(20000)}, {"distanceLeftPoints", points(20000)}}, Json::array({"imageId", "placement"})));
    result.push_back(operation("delete_image", "pictures", "Delete one native picture from its containing body, table cell, header or footer.",
        {{"imageId", identifier()}}, Json::array({"imageId"})));
    result.push_back(operation("set_story_text", "stories", "Set default header/footer text, or one left/center/right region. PAGE and PAGES fields use {PAGE} and {PAGES}; other regions and their pictures retain identity.",
        {{"story", choices({"header", "footer"})}, {"region", choices({"left", "center", "right"})}, {"text", text()}}, Json::array({"story", "text"})));
    result.push_back(operation("split_paragraph", "structure", "Split a body paragraph at a UTF-16 boundary. Optional newParagraphId lets a later operation in the same preview address the new paragraph.",
        {{"target", reference("semanticTarget")}, {"newParagraphId", identifier()}}, Json::array({"target"})));
    result.push_back(operation("merge_paragraph", "structure", "Merge a body paragraph with its immediately following paragraph, preserving supported atoms and formatting.",
        {{"blockId", identifier()}}, Json::array({"blockId"})));
    result.push_back(operation("move_block", "structure", "Move a paragraph, table or TOC before another body block; omitted/null beforeBlockId appends.",
        {{"blockId", identifier()}, {"beforeBlockId", {{"oneOf", Json::array({identifier(), Json{{"type", "null"}}})}}}}, Json::array({"blockId"})));
    result.push_back(operation("set_academic_field", "references", "Edit an existing citation locator or reference target/format, retaining the field identity. Computed numbering cannot be assigned.",
        {{"fieldId", identifier()}, {"targetId", identifier()}, {"locator", text(512)}, {"format", choices({"label_number", "full_text"})}}, Json::array({"fieldId"})));
    result.push_back(operation("set_caption_metadata", "references", "Change an existing caption label or its pictured object while preserving its paragraph and numbering field identities.",
        {{"blockId", identifier()}, {"label", choices({"Figure", "Table", "Equation"})}, {"objectId", identifier()}}, Json::array({"blockId"})));
    result.push_back(operation("set_bibliography_entry", "references", "Enable or remove a paragraph's manual bibliography role and generated number prefix. Citations continue referring to its stable identity.",
        {{"blockId", identifier()}, {"enabled", {{"type", "boolean"}}}}, Json::array({"blockId", "enabled"})));
    return result;
}
Json authoringSchemaDefinitions() {
    return {
        {"semanticTarget", object({{"blockId", identifier()}, {"start", count(10000000)}}, Json::array({"blockId", "start"}))},
        {"cellTarget", object({{"tableId", identifier()}, {"cellId", identifier()}, {"start", count(10000000)}, {"end", count(10000000)}}, Json::array({"tableId", "cellId"}))},
        {"cellRangeTarget", object({{"tableId", identifier()}, {"cellId", identifier()}, {"start", count(10000000)}, {"end", count(10000000)}}, Json::array({"tableId", "cellId", "start", "end"}))},
        {"imageTarget", {{"oneOf", Json::array({reference("semanticTarget"),
            object({{"tableId", identifier()}, {"cellId", identifier()}, {"start", count(10000000)}}, Json::array({"tableId", "cellId", "start"})),
            object({{"story", choices({"header", "footer"})}, {"start", count(32768)}}, Json::array({"story", "start"}))})}}},
        {"semanticParagraphFormat", {{"type", "object"}, {"additionalProperties", false}, {"minProperties", 1}, {"properties", {
            {"alignment", choices({"left", "center", "right", "justify"})},
            {"leftIndentPoints", points(20000, -20000)}, {"rightIndentPoints", points(20000, -20000)}, {"firstLineIndentPoints", points(20000, -20000)},
            {"spaceBeforePoints", points(20000)}, {"spaceAfterPoints", points(20000)}, {"lineSpacing", points(20, 0.01)},
            {"lineSpacingPoints", points(20000, 0.01)}, {"lineSpacingRule", choices({"automatic", "atLeast", "exact"})},
            {"keepWithNext", {{"type", "boolean"}}}, {"keepLines", {{"type", "boolean"}}}, {"pageBreakBefore", {{"type", "boolean"}}}
        }}}},
        {"semanticTextStyle", {{"type", "object"}, {"additionalProperties", false}, {"minProperties", 1}, {"properties", {
            {"bold", {{"type", "boolean"}}}, {"italic", {{"type", "boolean"}}}, {"strike", {{"type", "boolean"}}}, {"underline", {{"type", "boolean"}}},
            {"fontFamily", text(1024)}, {"fontSizePoints", points(1000, 0.5)},
            {"foregroundColor", {{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}},
            {"highlightColor", {{"oneOf", Json::array({Json{{"type", "null"}}, Json{{"type", "string"}, {"pattern", "^#[0-9A-Fa-f]{6}$"}}})}}},
            {"verticalAlign", choices({"baseline", "superscript", "subscript"})}
        }}}}
    };
}
Json authoringCapabilities() {
    Json result = Json::array();
    for (const auto& schema : authoringOperationSchemas()) {
        result.push_back({{"kind", schema.at("properties").at("kind").at("const")}, {"group", schema.at("x-group")}, {"description", schema.at("description")}});
    }
    return result;
}
}  // namespace docxstudio::codex
