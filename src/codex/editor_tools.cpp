#include "docxstudio/codex/editor_tools.hpp"

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
      "enum": ["selection", "document", "outline", "blocks"],
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
    return parseSchema(R"json(
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
}

Json fileCapabilitySchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "capabilityId": {
      "type": "string",
      "minLength": 16,
      "maxLength": 128,
      "pattern": "^[A-Za-z0-9_-]+$",
      "description": "Opaque, app-minted identifier; never a filesystem path."
    },
    "offset": {"type": "integer", "minimum": 0, "default": 0},
    "maxBytes": {
      "type": "integer",
      "minimum": 1,
      "maximum": 1048576,
      "default": 262144
    },
    "expectedSha256": {"type": "string", "pattern": "^[0-9A-Fa-f]{64}$"}
  },
  "required": ["capabilityId"]
}
)json");
}

Json readOutputSchema() {
    return parseSchema(R"json(
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

Json fileCapabilityOutputSchema() {
    return parseSchema(R"json(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "capabilityId": {"type": "string"},
    "offset": {"type": "integer", "minimum": 0},
    "nextOffset": {"type": "integer", "minimum": 0},
    "eof": {"type": "boolean"},
    "base64Data": {"type": "string", "contentEncoding": "base64"},
    "mimeType": {"type": "string"},
    "sizeBytes": {"type": "integer", "minimum": 0},
    "sha256": {"type": "string", "pattern": "^[0-9A-Fa-f]{64}$"}
  },
  "required": ["capabilityId", "offset", "nextOffset", "eof", "base64Data", "mimeType", "sizeBytes", "sha256"]
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
         "selection and table-cell targets returned by search. Request "
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
         "keep-with-next), equations, and page breaks. When the user asks "
         "for a diagram, insert_excalidraw_figure and "
         "replace_excalidraw_figure create or revise a native editable "
         "Professional-mode scene using the bundled offline renderer. "
         "When the user asks "
         "you to edit or format the document, use this tool instead of only "
         "describing the steps. This never commits changes; the user reviews "
         "the preview in Owl Docs before it enters the undo stack.",
         previewSchema(), previewOutputSchema(), previewAnnotations()});
    tools.push_back(
        {std::string(kEditorFileCapabilityTool),
         "Read a bounded byte range from an opaque, user-granted file "
         "capability. Filesystem paths are never accepted.",
         fileCapabilitySchema(), fileCapabilityOutputSchema(),
         readAnnotations()});
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
    return {{"schemaVersion", kEditorSchemaVersion}, {"tools", std::move(tools)}};
}

bool isEditorV1Tool(const std::string_view name) noexcept {
    return name == kEditorReadTool || name == kEditorSearchTool ||
           name == kEditorPreviewTool ||
           name == kEditorFileCapabilityTool;
}

}  // namespace docxstudio::codex
