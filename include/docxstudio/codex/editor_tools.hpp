#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace docxstudio::codex {

using Json = nlohmann::json;

inline constexpr std::string_view kEditorSchemaVersion = "editor.v1";
// Bump this whenever a persisted app-server thread must be recreated to pick
// up a changed dynamic-tool catalog. thread/resume does not accept dynamicTools.
inline constexpr std::string_view kEditorToolCatalogVersion =
    "editor.v1.catalog.7";
inline constexpr std::string_view kEditorReadTool = "editor_v1_read";
inline constexpr std::string_view kEditorSearchTool = "editor_v1_search";
inline constexpr std::string_view kEditorPreviewTool = "editor_v1_preview";
inline constexpr std::string_view kEditorFileCapabilityTool =
    "editor_v1_file_capability_read";

struct EditorToolDefinition {
    std::string name;
    std::string description;
    Json inputSchema;
    Json outputSchema;
    Json annotations;

    // Shape accepted by Codex app-server dynamic tool registration. The same
    // name/description/inputSchema fields can also seed an MCP implementation.
    [[nodiscard]] Json toDynamicToolSpec() const;
};

[[nodiscard]] std::vector<EditorToolDefinition> editorV1ToolDefinitions();
[[nodiscard]] Json editorV1ToolManifest();
[[nodiscard]] bool isEditorV1Tool(std::string_view name) noexcept;

}  // namespace docxstudio::codex
