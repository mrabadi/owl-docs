#pragma once

#include "docxstudio/codex/types.hpp"

#include <QString>

namespace docxstudio::app {

class DocumentCanvas;

// Executes one app-server dynamic editor tool against a single open document.
// Write tools only create an isolated DocumentSession preview.
[[nodiscard]] codex::Json invokeEditorTool(
    DocumentCanvas& canvas,
    const QString& documentId,
    const QString& tool,
    const codex::Json& arguments,
    QString& previewSummary,
    QString& error);

}  // namespace docxstudio::app
