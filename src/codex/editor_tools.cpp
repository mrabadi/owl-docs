#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/codex/authoring_tools.hpp"

#include <utility>

namespace docxstudio::codex {
namespace {

Json parseSchema(const char* source) {
    return Json::parse(source);
}

Json readSchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "expectedRevision": {"type": "integer", "minimum": 0},
    "scope": {
      "type": "string",
      "enum": ["selection", "document", "outline", "blocks", "stories", "tables", "metadata", "images"],
      "default": "selection"
    },
    "blockIds": {
      "type": "array",
      "items": {"type": "string", "minLength": 1},
      "maxItems": 256,
      "uniqueItems": true
    },
    "tableCellTargets": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
          "tableId": {"type": "string", "minLength": 1},
          "cellId": {"type": "string", "minLength": 1}
        },
        "required": ["tableId", "cellId"]
      },
      "maxItems": 256,
      "uniqueItems": true
    },
    "includeFormatting": {"type": "boolean", "default": true},
    "imageIds": {"type": "array", "items": {"type": "string", "minLength": 1}, "maxItems": 64, "uniqueItems": true},
    "maxBlocks": {"type": "integer", "minimum": 1, "maximum": 2048, "default": 512},
    "blockOffset": {"type": "integer", "minimum": 0, "default": 0},
    "maxCharacters": {
      "type": "integer",
      "minimum": 1,
      "maximum": 200000,
      "default": 50000
    }
  },
  "required": ["scope"]
}
)json");
}

Json searchSchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "expectedRevision": {"type": "integer", "minimum": 0},
    "query": {
      "type": "string",
      "minLength": 1,
      "maxLength": 1024,
      "description": "Length is measured in Unicode code points; result offsets are UTF-16."
    },
    "caseSensitive": {"type": "boolean", "default": false},
    "wholeWord": {"type": "boolean", "default": false},
    "maxResults": {
      "type": "integer",
      "minimum": 1,
      "maximum": 500,
      "default": 100
    }
  },
  "required": ["query"]
}
)json");
}

Json previewSchema() {
    auto schema = parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "$defs": {
    "target": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "blockId": {"type": "string", "minLength": 1},
        "start": {"type": "integer", "minimum": 0},
        "end": {"type": "integer", "minimum": 0}
      },
      "required": ["blockId", "start"]
    },
    "textStyle": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "bold": {"type": "boolean"},
        "italic": {"type": "boolean"},
        "underline": {"type": "boolean"},
        "strike": {"type": "boolean"},
        "fontFamily": {"type": "string", "minLength": 1},
        "fontSizePoints": {"type": "number", "exclusiveMinimum": 0, "maximum": 1000},
        "foregroundColor": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$"},
        "highlightColor": {"type": "string", "pattern": "^#[0-9A-Fa-f]{6}$"},
        "verticalAlign": {"type": "string", "enum": ["baseline", "superscript", "subscript"]}
      },
      "minProperties": 1
    },
    "paragraphStyle": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "styleId": {
          "oneOf": [
            {"type": "string", "minLength": 1, "maxLength": 1024},
            {"type": "null"}
          ],
          "description": "Set a bounded semantic paragraph-style ID, or null to clear it. IDs are additionally limited to 1024 UTF-8 bytes. Recognized built-ins also apply their catalog baseline; unknown IDs retain identity without invented formatting."
        },
        "alignment": {"type": "string", "enum": ["left", "center", "right", "justify"]},
        "lineSpacing": {"type": "number", "exclusiveMinimum": 0, "maximum": 20},
        "spaceBeforePoints": {"type": "number", "minimum": 0, "maximum": 10000},
        "spaceAfterPoints": {"type": "number", "minimum": 0, "maximum": 10000},
        "keepWithNext": {"type": "boolean"}
      },
      "minProperties": 1
    },
    "excalidrawColor": {
      "type": "string",
      "pattern": "^#[0-9A-Fa-f]{6}$"
    },
    "excalidrawLabel": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "text": {"type": "string", "minLength": 1, "maxLength": 2000},
        "fontSize": {"type": "number", "minimum": 8, "maximum": 96}
      },
      "required": ["text"]
    },
    "excalidrawElement": {
      "oneOf": [
        {
          "type": "object", "additionalProperties": false,
          "properties": {
            "id": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$"},
            "type": {"enum": ["rectangle", "ellipse", "diamond"]},
            "x": {"type": "number", "minimum": -10000, "maximum": 10000},
            "y": {"type": "number", "minimum": -10000, "maximum": 10000},
            "width": {"type": "number", "exclusiveMinimum": 0, "maximum": 10000},
            "height": {"type": "number", "exclusiveMinimum": 0, "maximum": 10000},
            "strokeColor": {"$ref": "#/$defs/excalidrawColor"},
            "backgroundColor": {"$ref": "#/$defs/excalidrawColor"},
            "strokeWidth": {"type": "number", "minimum": 1, "maximum": 4},
            "opacity": {"type": "integer", "minimum": 0, "maximum": 100},
            "label": {"$ref": "#/$defs/excalidrawLabel"}
          },
          "required": ["id", "type", "x", "y", "width", "height"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {
            "id": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$"},
            "type": {"const": "text"},
            "x": {"type": "number", "minimum": -10000, "maximum": 10000},
            "y": {"type": "number", "minimum": -10000, "maximum": 10000},
            "text": {"type": "string", "minLength": 1, "maxLength": 4000},
            "fontSize": {"type": "number", "minimum": 8, "maximum": 96},
            "strokeColor": {"$ref": "#/$defs/excalidrawColor"},
            "opacity": {"type": "integer", "minimum": 0, "maximum": 100}
          },
          "required": ["id", "type", "x", "y", "text"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {
            "id": {"type": "string", "pattern": "^[A-Za-z0-9_-]{1,64}$"},
            "type": {"enum": ["line", "arrow"]},
            "x": {"type": "number", "minimum": -10000, "maximum": 10000},
            "y": {"type": "number", "minimum": -10000, "maximum": 10000},
            "points": {
              "type": "array", "minItems": 2, "maxItems": 16,
              "items": {
                "type": "array", "prefixItems": [
                  {"type": "number", "minimum": -10000, "maximum": 10000},
                  {"type": "number", "minimum": -10000, "maximum": 10000}
                ], "items": false
              }
            },
            "strokeColor": {"$ref": "#/$defs/excalidrawColor"},
            "strokeWidth": {"type": "number", "minimum": 1, "maximum": 4},
            "opacity": {"type": "integer", "minimum": 0, "maximum": 100},
            "label": {"$ref": "#/$defs/excalidrawLabel"}
          },
          "required": ["id", "type", "x", "y", "points"]
        }
      ]
    },
    "excalidrawFigure": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "elements": {
          "type": "array", "minItems": 1, "maxItems": 128,
          "items": {"$ref": "#/$defs/excalidrawElement"}
        },
        "accessibleName": {"type": "string", "minLength": 1, "maxLength": 500},
        "widthPoints": {"type": "number", "minimum": 36, "maximum": 936, "description": "Optional display width, or bounding-box width when heightPoints is also supplied. The rendered figure always keeps its aspect ratio."},
        "heightPoints": {"type": "number", "minimum": 36, "maximum": 936, "description": "Optional display height, or bounding-box height when widthPoints is also supplied. The rendered figure always keeps its aspect ratio."}
      },
      "required": ["elements", "accessibleName"]
    },
    "operation": {
      "oneOf": [
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "replace_equation"}, "equationId": {"type": "string", "minLength": 1}, "latex": {"type": "string", "minLength": 1, "maxLength": 8192}, "display": {"type": "boolean"}}, "required": ["kind", "equationId", "latex"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "set_note_text"}, "noteId": {"type": "string", "minLength": 1}, "text": {"type": "string", "maxLength": 32768}}, "required": ["kind", "noteId", "text"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "insert_caption"}, "target": {"$ref": "#/$defs/target"}, "label": {"type": "string", "enum": ["Figure", "Table", "Equation"], "default": "Figure"}, "objectId": {"type": "string", "minLength": 1}, "text": {"type": "string", "maxLength": 32768}}, "required": ["kind", "target", "text"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "insert_bibliography_entry"}, "target": {"$ref": "#/$defs/target"}, "text": {"type": "string", "maxLength": 32768}}, "required": ["kind", "target", "text"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "insert_cross_reference"}, "target": {"$ref": "#/$defs/target"}, "targetId": {"type": "string", "minLength": 1}, "format": {"type": "string", "enum": ["label_number", "full_text"], "default": "label_number"}}, "required": ["kind", "target", "targetId"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "insert_citation"}, "target": {"$ref": "#/$defs/target"}, "entryId": {"type": "string", "minLength": 1}, "locator": {"type": "string", "maxLength": 512}}, "required": ["kind", "target", "entryId"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "insert_toc"}, "target": {"$ref": "#/$defs/target"}, "maxLevel": {"type": "integer", "minimum": 1, "maximum": 9, "default": 3}}, "required": ["kind", "target"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "configure_toc"}, "tocId": {"type": "string", "minLength": 1}, "maxLevel": {"type": "integer", "minimum": 1, "maximum": 9}}, "required": ["kind", "tocId", "maxLevel"]},
        {"type": "object", "additionalProperties": false, "properties": {"kind": {"const": "delete_toc"}, "tocId": {"type": "string", "minLength": 1}}, "required": ["kind", "tocId"]},
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "insert_note"}, "target": {"$ref": "#/$defs/target"}, "text": {"type": "string", "maxLength": 32768}, "noteKind": {"type": "string", "enum": ["footnote", "endnote"], "default": "footnote"}},
          "required": ["kind", "target", "text"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "replace_text"}, "target": {"$ref": "#/$defs/target"}, "text": {"type": "string"}},
          "required": ["kind", "target", "text"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "insert_text"}, "target": {"$ref": "#/$defs/target"}, "text": {"type": "string"}},
          "required": ["kind", "target", "text"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "delete_range"}, "target": {"$ref": "#/$defs/target"}},
          "required": ["kind", "target"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "set_text_style"}, "target": {"$ref": "#/$defs/target"}, "style": {"$ref": "#/$defs/textStyle"}},
          "required": ["kind", "target", "style"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "set_paragraph_style"}, "target": {"$ref": "#/$defs/target"}, "style": {"$ref": "#/$defs/paragraphStyle"}},
          "required": ["kind", "target", "style"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "insert_equation"}, "target": {"$ref": "#/$defs/target"}, "latex": {"type": "string", "minLength": 1}, "display": {"type": "boolean", "default": false}},
          "required": ["kind", "target", "latex"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "insert_page_break"}, "target": {"$ref": "#/$defs/target"}},
          "required": ["kind", "target"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "insert_excalidraw_figure"}, "target": {"$ref": "#/$defs/target"}, "figure": {"$ref": "#/$defs/excalidrawFigure"}},
          "required": ["kind", "target", "figure"]
        },
        {
          "type": "object", "additionalProperties": false,
          "properties": {"kind": {"const": "replace_excalidraw_figure"}, "imageId": {"type": "string", "minLength": 1}, "figure": {"$ref": "#/$defs/excalidrawFigure"}},
          "required": ["kind", "imageId", "figure"]
        }
      ]
    }
  },
  "properties": {
    "baseRevision": {"type": "integer", "minimum": 0},
    "summary": {"type": "string", "minLength": 1, "maxLength": 512},
    "operations": {
      "type": "array",
      "items": {"$ref": "#/$defs/operation"},
      "minItems": 1,
      "maxItems": 256
    }
  },
  "required": ["baseRevision", "summary", "operations"]
}
)json");
    const auto authoredDefinitions = authoringSchemaDefinitions();
    for (const auto& [name, definition] : authoredDefinitions.items())
        schema["$defs"][name] = definition;
    const auto styleId = schema["$defs"]["paragraphStyle"]["properties"]["styleId"];
    schema["$defs"]["paragraphStyle"] = authoredDefinitions.at("semanticParagraphFormat");
    schema["$defs"]["paragraphStyle"]["properties"]["styleId"] = styleId;
    schema["$defs"]["textStyle"] = authoredDefinitions.at("semanticTextStyle");
    for (const auto& operation : authoringOperationSchemas())
        schema["$defs"]["operation"]["oneOf"].push_back(operation);
    Json pageProperties = Json::object();
    for (const auto* name : {"widthPoints", "heightPoints", "marginTopPoints", "marginRightPoints", "marginBottomPoints", "marginLeftPoints"})
        pageProperties[name] = {{"type", "number"}, {"minimum", 0}, {"maximum", 100000}};
    pageProperties["widthPoints"]["exclusiveMinimum"] = 0;
    pageProperties["heightPoints"]["exclusiveMinimum"] = 0;
    schema["$defs"]["pageLayout"] = {{"type", "object"}, {"additionalProperties", false}, {"properties", pageProperties},
        {"required", {"widthPoints", "heightPoints", "marginTopPoints", "marginRightPoints", "marginBottomPoints", "marginLeftPoints"}}};
    schema["$defs"]["operation"]["oneOf"].push_back({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"kind", {{"const", "set_page_layout"}}}, {"layout", {{"$ref", "#/$defs/pageLayout"}}},
            {"expectedLayoutStateId", {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}}}},
        {"required", {"kind", "layout", "expectedLayoutStateId"}}});
    schema["$defs"]["excalidrawLabel"]["properties"]["id"] = {{"type", "string"}, {"pattern", "^[A-Za-z0-9_-]{1,64}$"}};
    auto& elements = schema["$defs"]["excalidrawElement"]["oneOf"];
    auto line = elements.back();
    line["properties"]["type"] = {{"const", "line"}};
    line["properties"].erase("label");
    auto arrow = elements.back();
    arrow["properties"]["type"] = {{"const", "arrow"}};
    const Json binding = {{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"id", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_-]{1,64}$"}}}}}, {"required", {"id"}}};
    arrow["properties"]["start"] = binding;
    arrow["properties"]["end"] = binding;
    elements.back() = std::move(line);
    elements.push_back(std::move(arrow));
    schema["$defs"]["excalidrawTarget"] = {{"oneOf", Json::array({
        {{"$ref", "#/$defs/target"}},
        {{"type", "object"}, {"additionalProperties", false},
         {"properties", {{"story", {{"enum", {"header", "footer"}}}},
                         {"start", {{"type", "integer"}, {"minimum", 0}}}}},
         {"required", {"story", "start"}}}
    })}};
    for (auto& operation : schema["$defs"]["operation"]["oneOf"]) {
        if (operation["properties"]["kind"].value("const", std::string()) == "insert_excalidraw_figure") {
            operation["properties"]["target"] = {{"$ref", "#/$defs/excalidrawTarget"}};
            operation["properties"]["imageId"] = {{"type", "string"},
                {"pattern", "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"}};
        } else if (operation["properties"]["kind"].value("const", std::string()) == "replace_excalidraw_figure") {
            operation["properties"]["reconstructScene"] = {{"type", "boolean"}, {"default", false},
                {"description", "Use only when the user explicitly requests a full redraw/reconstruction. It permits replacing original scene/label IDs and unsupported objects, with a visible loss warning. Prefer native patches for partial edits."}};
        }
    }
    const Json coordinate = {{"type", "number"}, {"minimum", -10000}, {"maximum", 10000}};
    const Json point = {{"type", "array"}, {"items", coordinate}, {"minItems", 2}, {"maxItems", 2}};
    schema["$defs"]["excalidrawPatchChanges"] = {{"type", "object"}, {"additionalProperties", false}, {"minProperties", 1},
        {"properties", {
            {"x", coordinate}, {"y", coordinate},
            {"width", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 10000}}},
            {"height", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 10000}}},
            {"text", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4000}}},
            {"fontSize", {{"type", "number"}, {"minimum", 8}, {"maximum", 96}}},
            {"strokeColor", {{"$ref", "#/$defs/excalidrawColor"}}},
            {"backgroundColor", {{"oneOf", Json::array({{{"$ref", "#/$defs/excalidrawColor"}}, {{"const", "transparent"}}})}}},
            {"strokeWidth", {{"type", "number"}, {"minimum", 1}, {"maximum", 4}}},
            {"opacity", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}},
            {"points", {{"type", "array"}, {"items", point}, {"minItems", 2}, {"maxItems", 64}}},
            {"angle", {{"type", "number"}, {"minimum", -6.283185307179586}, {"maximum", 6.283185307179586}}}
        }}};
    schema["$defs"]["operation"]["oneOf"].push_back({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"kind", {{"const", "patch_excalidraw_figure"}}}, {"imageId", {{"type", "string"}, {"minLength", 1}}},
            {"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 128},
                {"items", {{"type", "object"}, {"additionalProperties", false},
                    {"properties", {{"id", {{"type", "string"}, {"pattern", "^[A-Za-z0-9_-]{1,64}$"}}},
                        {"changes", {{"$ref", "#/$defs/excalidrawPatchChanges"}}}}}, {"required", {"id", "changes"}}}}}}}},
        {"required", {"kind", "imageId", "edits"}},
        {"description", "Patch existing native element IDs while preserving all unrelated objects, groups, bindings and assets. Width/height apply only to dimension-bearing objects; line/arrow geometry uses points. Text/fontSize target text elements. No creation, deletion or rebinding."}});
    const Json listBlocks = {{"type", "array"}, {"minItems", 1}, {"maxItems", 2048}, {"uniqueItems", true},
        {"items", {{"type", "string"}, {"pattern", "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"}}}};
    const Json listLevel = {{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"bulletIndentSpaces", {{"type", "integer"}, {"minimum", 0}, {"maximum", 400}}},
            {"textIndentSpaces", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}}}},
        {"required", {"bulletIndentSpaces", "textIndentSpaces"}}};
    schema["$defs"]["operation"]["oneOf"].push_back({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"kind", {{"const", "set_list"}}}, {"blockIds", listBlocks},
            {"listKind", {{"enum", {"bullets", "numbered", "none"}}}}}}, {"required", {"kind", "blockIds", "listKind"}}});
    schema["$defs"]["operation"]["oneOf"].push_back({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"kind", {{"const", "set_list_level"}}}, {"blockIds", listBlocks},
            {"level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}}}}, {"required", {"kind", "blockIds", "level"}}});
    schema["$defs"]["operation"]["oneOf"].push_back({{"type", "object"}, {"additionalProperties", false},
        {"properties", {{"kind", {{"const", "set_list_layout"}}}, {"blockIds", listBlocks},
            {"levels", {{"type", "array"}, {"minItems", 10}, {"maxItems", 10}, {"items", listLevel}}}}},
        {"required", {"kind", "blockIds", "levels"}}});
    return schema;
}

Json readOutputSchema() {
    auto schema = parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "documentId": {"type": "string"},
    "revision": {"type": "integer", "minimum": 0},
    "scope": {"type": "string"},
    "truncated": {"type": "boolean"},
    "blocks": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
          "id": {"type": "string"},
          "type": {"type": "string"},
          "text": {"type": "string"},
          "attributes": {"type": "object", "additionalProperties": true}
        },
        "required": ["id", "type", "text"]
      }
    }
  },
  "required": ["documentId", "revision", "scope", "truncated", "blocks"]
}
)json");
    auto layout = previewSchema()["$defs"]["pageLayout"];
    layout["properties"]["stateId"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}};
    layout["required"].push_back("stateId");
    schema["properties"]["pageLayout"] = std::move(layout);
    schema["properties"]["nextBlockOffset"] = {{"type", "integer"}, {"minimum", 0}};
    return schema;
}

Json searchOutputSchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "documentId": {"type": "string"},
    "revision": {"type": "integer", "minimum": 0},
    "query": {"type": "string"},
    "caseSensitive": {"type": "boolean"},
    "wholeWord": {"type": "boolean"},
    "truncated": {"type": "boolean"},
    "results": {
      "type": "array",
      "maxItems": 500,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
          "blockId": {
            "type": "string",
            "minLength": 1,
            "description": "Paragraph ID or, for tableCell results, the cell ID."
          },
          "blockType": {"type": "string", "enum": ["paragraph", "tableCell"]},
          "tableId": {"type": "string", "minLength": 1},
          "cellId": {"type": "string", "minLength": 1},
          "row": {"type": "integer", "minimum": 0},
          "column": {"type": "integer", "minimum": 0},
          "start": {"type": "integer", "minimum": 0},
          "end": {"type": "integer", "minimum": 0},
          "context": {"type": "string", "maxLength": 1216},
          "contextStart": {"type": "integer", "minimum": 0},
          "contextEnd": {"type": "integer", "minimum": 0}
        },
        "required": ["blockId", "blockType", "start", "end", "context", "contextStart", "contextEnd"],
        "allOf": [
          {
            "if": {
              "properties": {"blockType": {"const": "tableCell"}},
              "required": ["blockType"]
            },
            "then": {"required": ["tableId", "cellId", "row", "column"]}
          }
        ]
      }
    }
  },
  "required": ["documentId", "revision", "query", "caseSensitive", "wholeWord", "truncated", "results"]
}
)json");
}

Json previewOutputSchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "previewId": {"type": "string", "minLength": 1},
    "documentId": {"type": "string", "minLength": 1},
    "baseRevision": {"type": "integer", "minimum": 0},
    "committed": {"const": false},
    "summary": {"type": "string"},
    "affectedBlockIds": {"type": "array", "items": {"type": "string"}, "uniqueItems": true},
    "warnings": {"type": "array", "items": {"type": "string"}}
  },
  "required": ["previewId", "documentId", "baseRevision", "committed", "summary", "affectedBlockIds", "warnings"]
}
)json");
}

Json readAnnotations() {
    return {{"readOnlyHint", true},
            {"destructiveHint", false},
            {"idempotentHint", true},
            {"openWorldHint", false}};
}

Json previewAnnotations() {
    return {{"readOnlyHint", false},
            {"destructiveHint", false},
            {"idempotentHint", false},
            {"openWorldHint", false}};
}

}  // namespace

Json EditorToolDefinition::toDynamicToolSpec() const {
    return {{"type", "function"},
            {"name", name},
            {"description", description},
            {"inputSchema", inputSchema}};
}

std::vector<EditorToolDefinition> editorV1ToolDefinitions() {
    std::vector<EditorToolDefinition> tools;
    tools.reserve(4);
    tools.push_back(
        {std::string(kEditorReadTool),
         "Read a bounded snapshot of the active document, including explicit "
         "selection and table-cell targets returned by search. Academic reads include offset-safe field metadata, plain note stories, caption/source identities and TOC settings. Request "
         "includeFormatting when a user asks about or requests formatting. "
         "Use the returned revision, stable block identifiers, and UTF-16 "
         "offsets when preparing edits.",
         readSchema(), readOutputSchema(), readAnnotations()});
    tools.push_back(
        {std::string(kEditorSearchTool),
         "Search body paragraphs and table cells in deterministic document "
         "order. Results contain stable paragraph/cell identifiers, UTF-16 "
         "grapheme-aligned offsets, and bounded context without changing the "
         "document.",
         searchSchema(), searchOutputSchema(), readAnnotations()});
    tools.push_back(
        {std::string(kEditorPreviewTool),
         "Create an atomic, revision-checked preview of semantic document "
         "operations, including text replacement, character formatting with "
         "set_text_style (font, size, bold, italic, underline, colors, "
         "highlight, superscript/subscript), paragraph formatting with "
         "set_paragraph_style (style, alignment, line/paragraph spacing, "
         "keep-with-next), insert_equation/replace_equation, plain footnotes/endnotes, numbered captions, manual numeric bibliography entries/citations, heading/caption cross-references, heading TOCs, and page breaks. Use insert_caption and insert_bibliography_entry to create independent paragraphs, and use returned source IDs for insert_citation and target IDs for insert_cross_reference. TOCs include Heading 1–3 by default, configurable through level 9. When the user asks "
         "for a diagram, insert_excalidraw_figure and "
         "replace_excalidraw_figure create or revise a native editable "
         "scene using the bundled offline renderer: new figures and reconstructions use Professional mode with a white canvas; native patches retain source styling and background. "
         "When the user asks "
         "you to edit or format the document, use this tool instead of only "
         "describing the steps. This never commits changes; the user reviews "
         "the preview in OwlDocs before it enters the undo stack.",
         previewSchema(), previewOutputSchema(), previewAnnotations()});
    tools.push_back({std::string(kEditorCapabilitiesTool),
        "Discover built-in OwlDocs editing capabilities, exact operation schemas, grouped features, limits, units and examples. "
        "Call before planning an edit; the result also reports current document revision and runtime availability. "
        "No skill installation or activation is required. All document writes use editor_v1_preview and user acceptance.",
        {{"type", "object"}, {"additionalProperties", false},
         {"properties", {{"expectedRevision", {{"type", "integer"}, {"minimum", 0}}}}}},
        {{"type", "object"}, {"additionalProperties", true},
         {"properties", {{"schemaVersion", {{"type", "string"}}}, {"catalogVersion", {{"type", "string"}}},
             {"documentId", {{"type", "string"}}}, {"revision", {{"type", "integer"}, {"minimum", 0}}},
             {"runtime", {{"type", "object"}, {"additionalProperties", true}}}}},
         {"required", {"schemaVersion", "catalogVersion"}}}, readAnnotations()});
    return tools;
}

Json editorV1ToolManifest() {
    Json tools = Json::array();
    for (const EditorToolDefinition& definition : editorV1ToolDefinitions()) {
        Json tool = definition.toDynamicToolSpec();
        tool["outputSchema"] = definition.outputSchema;
        tool["annotations"] = definition.annotations;
        tools.push_back(std::move(tool));
    }
    return {{"schemaVersion", kEditorSchemaVersion}, {"catalogVersion", kEditorToolCatalogVersion}, {"tools", std::move(tools)}};
}

Json editorV1Capabilities() {
    const auto preview = previewSchema();
    Json groups = Json::object();
    Json descriptions = Json::object();
    for (const auto& item : authoringCapabilities()) {
        const auto kind = item.value("kind", std::string());
        if (!kind.empty()) descriptions[kind] = item;
    }
    for (const auto& operation : preview.at("$defs").at("operation").at("oneOf")) {
        const auto kind = operation.at("properties").at("kind").at("const").get<std::string>();
        std::string group = "text";
        if (descriptions.contains(kind)) group = descriptions.at(kind).value("group", std::string("text"));
        else if (kind.find("excalidraw") != std::string::npos) group = "figures";
        else if (kind.find("equation") != std::string::npos) group = "equations";
        else if (kind.find("note") != std::string::npos) group = "notes";
        else if (kind == "set_text_style" || kind == "set_paragraph_style") group = "formatting";
        else if (kind == "set_list" || kind == "set_list_level" || kind == "set_list_layout") group = "lists";
        else if (kind == "insert_page_break" || kind == "set_page_layout") group = "pageLayout";
        else if (kind.find("caption") != std::string::npos || kind.find("citation") != std::string::npos ||
                 kind.find("bibliography") != std::string::npos || kind.find("toc") != std::string::npos || kind.find("reference") != std::string::npos) group = "references";
        if (!groups.contains(group)) groups[group] = Json::array();
        groups[group].push_back(kind);
    }
    Json features = Json::array();
    for (const auto& [name, operations] : groups.items()) features.push_back({{"group", name}, {"operations", operations}});
    const Json target = {{"blockId", "<blockId from editor_v1_read>"}, {"start", 0}};
    const Json figure = {{"accessibleName", "Question and evidence"}, {"elements", Json::array({
        {{"id", "question"}, {"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", 180}, {"height", 80}, {"label", {{"id", "question-label"}, {"text", "Question"}}}},
        {{"id", "evidence"}, {"type", "rectangle"}, {"x", 260}, {"y", 0}, {"width", 180}, {"height", 80}, {"label", {{"id", "evidence-label"}, {"text", "Evidence"}}}},
        {{"id", "relation"}, {"type", "arrow"}, {"x", 180}, {"y", 40}, {"points", {{0, 0}, {80, 0}}},
            {"start", {{"id", "question"}}}, {"end", {{"id", "evidence"}}}, {"label", {{"id", "relation-label"}, {"text", "supports"}}}}
    })}};
    return {{"schemaVersion", kEditorSchemaVersion}, {"catalogVersion", kEditorToolCatalogVersion},
        {"groups", groups}, {"supportedFeatures", features}, {"operationSchemas", preview.at("$defs").at("operation").at("oneOf")},
        {"$defs", preview.at("$defs")},
        {"unavailableFeatures", Json::array({
            {{"feature", "automatic_preview_acceptance"}, {"reason", "Only the user can accept or discard a proposed document edit."}},
            {{"feature", "file_and_system_commands"}, {"reason", "File open/save/export, local file selection and other desktop UI/system commands remain user actions. No file grant UI exists; use validated base64 raster insertion or edit an existing image ID. Arbitrary paths, shell commands and remote URLs are unavailable."}},
            {{"feature", "implicit_figure_reconstruction"}, {"reason", "Default replacement retains all existing scene/label IDs and rejects incomplete scenes. Full redraw requires an explicit user request, reconstructScene=true and a visible loss warning."}}
        })},
        {"bounds", {{"previewOperations", 256}, {"readCharacters", 200000}, {"searchResults", 500},
            {"tableRows", 50}, {"tableColumns", 20}, {"encodedImageBytes", 4194304},
            {"aggregateImageBytes", 4194304}, {"desktopEncodedImageBytes", 16777216},
            {"desktopAggregateImageBytes", 33554432}, {"maxRequestJsonBytes", 8388608},
            {"storyUtf16Units", 32768}, {"figureElements", 128}, {"figurePatchEdits", 128}, {"readBlocks", 2048}}},
        {"units", {{"textOffsets", "UTF-16 code units"}, {"geometry", "points"},
            {"pointsToTwips", 20}, {"pointsToEmu", 12700}, {"colors", "#RRGGBB"}}},
        {"workflow", {{"manualSkillActivationRequired", false}, {"writesCommitted", false},
            {"steps", {"Discover current capabilities and runtime availability.", "Read the target, revision, stable IDs and pageLayout.stateId.",
                "Prepare one atomic editor_v1_preview at baseRevision; re-read after conflicts.", "The user reviews and accepts or discards; never claim a preview is already applied."}},
            {"figureMode", "Professional with a white canvas for new figures and reconstructions (host-enforced)."},
            {"figureEditing", "Existing native patches retain source styling and background. Read editableElementsComplete, editableElementsUnsupported and editableElementsTruncated. Default replacement requires a complete source and all original element/label IDs. For partial edits or incomplete scenes inspect bounded nativeScene/nativeSceneTruncated and use patch_excalidraw_figure on known IDs. Only an explicit user request for a full redraw permits reconstructScene=true with its visible loss warning. Plain line labels are unsupported."},
            {"imageInput", "Codex raster input is limited to 4 MiB encoded data per image and per preview; the full JSONL request must remain below 8 MiB. Larger pictures may be inserted through the desktop and then edited by image ID."}}},
        {"examples", {{"insertFigure", {{"baseRevision", 7}, {"summary", "Add a professional diagram"},
            {"operations", Json::array({{{"kind", "insert_excalidraw_figure"}, {"target", target}, {"figure", figure}}})}}},
            {"replaceFigure", {{"baseRevision", 7}, {"summary", "Revise an existing editable figure"},
                {"operations", Json::array({{{"kind", "replace_excalidraw_figure"}, {"imageId", "<imageId from editor_v1_read>"}, {"figure", figure}}})}}},
            {"insertTable", {{"baseRevision", 7}, {"summary", "Add a table"},
                {"operations", Json::array({{{"kind", "insert_table"}, {"target", target}, {"rows", 2}, {"columns", 2}, {"headerRow", true},
                    {"cells", Json::array({Json::array({"Question", "Evidence"}), Json::array({"Hypothesis", "Observation"})})}}})}}},
            {"patchFigure", {{"baseRevision", 7}, {"summary", "Update a native diagram label without replacing the scene"},
                {"operations", Json::array({{{"kind", "patch_excalidraw_figure"}, {"imageId", "<imageId from editor_v1_read>"},
                    {"edits", Json::array({{{"id", "relation-label"}, {"changes", {{"text", "supports"}}}}})}}})}}}}}};
}

bool isEditorV1Tool(const std::string_view name) noexcept {
    return name == kEditorReadTool || name == kEditorSearchTool ||
           name == kEditorPreviewTool ||
           name == kEditorFileCapabilityTool || name == kEditorCapabilitiesTool;
}

}  // namespace docxstudio::codex
