#pragma once

#include "docxstudio/core/document.h"

#include <unordered_map>

namespace docxstudio::core {

struct ResolvedField {
    NodeId id;
    NodeId target_id;
    std::u16string text;
    bool unresolved{false};
};
struct ResolvedNote {
    NodeId id;
    NodeId reference_id;
    NoteKind kind{NoteKind::footnote};
    std::size_t number{0};
    std::u16string text;
};
struct TocEntry {
    NodeId paragraph_id;
    std::uint8_t level{1};
    std::u16string text;
};
struct AcademicResolution {
    std::vector<ResolvedField> fields;
    std::vector<ResolvedNote> notes;
    std::vector<TocEntry> toc_entries;
    std::unordered_map<NodeId, std::size_t, NodeIdHash> caption_numbers;
    std::unordered_map<NodeId, std::size_t, NodeIdHash> bibliography_numbers;
    std::vector<std::string> diagnostics;
    [[nodiscard]] const ResolvedField* findField(NodeId id) const noexcept;
    [[nodiscard]] std::u16string fieldText(NodeId id) const;
};

[[nodiscard]] AcademicResolution resolveAcademic(const Document& document);
[[nodiscard]] std::u16string captionLabelText(CaptionLabel label);
[[nodiscard]] std::u16string academicParagraphText(
    const Paragraph& paragraph, const AcademicResolution& resolution);

} // namespace docxstudio::core
