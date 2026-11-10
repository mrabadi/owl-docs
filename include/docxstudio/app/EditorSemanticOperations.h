#pragma once

#include "docxstudio/codex/types.hpp"
#include "docxstudio/core/operations.h"

#include <QString>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace docxstudio::app {
[[nodiscard]] bool parseEditorCharacterFormat(
    const codex::Json& style, core::CharacterFormatDelta& delta, QString& error);
// Parses shared body/cell paragraph properties. styleId is handled by the
// provenance-aware body style planner and is ignored here.
[[nodiscard]] bool parseEditorParagraphFormat(
    const codex::Json& style, core::ParagraphFormatDelta& delta, QString& error);
[[nodiscard]] bool handlesSemanticOperation(std::string_view kind);
// Validates and plans against an isolated working document. Failure leaves
// the document, operation list and affected identities unchanged.
[[nodiscard]] bool appendSemanticOperation(
    core::Document& working, const codex::Json& source,
    std::vector<core::Operation>& operations, std::set<std::string>& affected,
    QString& error);
}  // namespace docxstudio::app
