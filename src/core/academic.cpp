#include "docxstudio/core/academic.h"

#include <algorithm>
#include <unordered_set>

namespace docxstudio::core {
namespace {
Result<void> validateField(const FieldAtom& field) {
    if (!field.id.isValid() || !field.target_id.isValid())
        return Error{ErrorCode::invalid_node_id, "Field and target IDs must be nonzero"};
    if (field.kind < FieldKind::note_reference || field.kind > FieldKind::citation ||
        field.reference_format < CrossReferenceFormat::label_number ||
        field.reference_format > CrossReferenceFormat::full_text ||
        field.locator.size() > 512 || !isValidUtf16(field.locator) ||
        field.locator.find(kInlineObjectReplacementCharacter) != std::u16string::npos)
        return Error{ErrorCode::invalid_operation, "Academic field metadata is invalid"};
    return {};
}
}

const FieldAtom* Paragraph::fieldAt(std::size_t offset) const noexcept {
    const auto found = std::find_if(fields_.begin(), fields_.end(),
        [offset](const FieldAtom& field) { return field.utf16_offset == offset; });
    return found == fields_.end() ? nullptr : &*found;
}

Result<void> Paragraph::insertField(std::size_t offset, FieldAtom field,
                                  const std::optional<CharacterFormat>& format) {
    const auto validation = validateField(field);
    if (!validation) return validation.error();
    // Reuse the existing atom insertion and formatting path, then replace its
    // temporary typed metadata. No observer can see the intermediate object.
    const auto inserted = insertEquation(offset, {field.id, offset, "x", false}, format);
    if (!inserted) return inserted.error();
    std::erase_if(equations_, [&field](const EquationAtom& equation) { return equation.id == field.id; });
    field.utf16_offset = offset;
    const auto location = std::lower_bound(fields_.begin(), fields_.end(), offset,
        [](const FieldAtom& existing, std::size_t at) { return existing.utf16_offset < at; });
    fields_.insert(location, std::move(field));
    return {};
}

const FieldAtom* Document::findField(NodeId id) const noexcept {
    for (const auto& paragraph : paragraphs_) for (const auto& field : paragraph.fields())
        if (field.id == id) return &field;
    return nullptr;
}
const Note* Document::findNote(NodeId id) const noexcept {
    const auto found = std::find_if(notes_.begin(), notes_.end(), [id](const Note& note) { return note.id == id; });
    return found == notes_.end() ? nullptr : &*found;
}
const TocBlock* Document::findToc(NodeId id) const noexcept {
    const auto found = std::find_if(tocs_.begin(), tocs_.end(), [id](const TocBlock& toc) { return toc.id == id; });
    return found == tocs_.end() ? nullptr : &*found;
}
Result<void> Document::insertField(const Position& position, FieldAtom field,
                                 const std::optional<CharacterFormat>& format) {
    const auto validation = validatePosition(position);
    if (!validation) return validation.error();
    if (nodeIdInUse(field.id)) return Error{ErrorCode::duplicate_node_id, "Field ID already exists"};
    std::size_t count = 0;
    for (const auto& paragraph : paragraphs_) count += paragraph.fields().size();
    if (count >= kMaximumAcademicFields) return Error{ErrorCode::invalid_operation, "Too many academic fields"};
    if (field.kind == FieldKind::note_reference && !findNote(field.target_id))
        return Error{ErrorCode::invalid_node_id, "Note reference target is missing"};
    return paragraphs_[*paragraphIndex(position.paragraph_id)].insertField(position.utf16_offset, std::move(field), format);
}
Result<void> Document::replaceField(NodeId id, FieldAtom field) {
    field.id = id;
    const auto validation = validateField(field);
    if (!validation) return validation.error();
    if (field.kind == FieldKind::note_reference && !findNote(field.target_id))
        return Error{ErrorCode::invalid_node_id, "Note reference target is missing"};
    for (auto& paragraph : paragraphs_) for (auto& existing : paragraph.fields_) {
        if (existing.id != id) continue;
        field.utf16_offset = existing.utf16_offset;
        existing = std::move(field);
        removeUnreferencedNotes();
        return {};
    }
    return Error{ErrorCode::invalid_node_id, "Field does not exist"};
}
Result<void> Document::setNote(Note note) {
    if (!note.id.isValid()) return Error{ErrorCode::invalid_node_id, "Note ID cannot be zero"};
    if (!isValidUtf16(note.text) || note.text.find(kInlineObjectReplacementCharacter) != std::u16string::npos ||
        note.text.size() > kMaximumNoteCodeUnits || note.kind < NoteKind::footnote || note.kind > NoteKind::endnote)
        return Error{ErrorCode::invalid_operation, "Note text or kind is invalid"};
    std::size_t total = note.text.size();
    for (const auto& existing : notes_) if (existing.id != note.id) total += existing.text.size();
    if (total > kMaximumNoteAggregateCodeUnits) return Error{ErrorCode::invalid_operation, "Note text exceeds document limit"};
    for (auto& existing : notes_) if (existing.id == note.id) { existing = std::move(note); return {}; }
    if (nodeIdInUse(note.id)) return Error{ErrorCode::duplicate_node_id, "Note ID already exists"};
    if (notes_.size() >= kMaximumNotes) return Error{ErrorCode::invalid_operation, "Too many notes"};
    notes_.push_back(std::move(note));
    return {};
}
Result<void> Document::setCaption(NodeId id, std::optional<CaptionMetadata> caption) {
    const auto index = paragraphIndex(id);
    if (!index) return Error{ErrorCode::invalid_node_id, "Caption paragraph does not exist"};
    if (caption && (!caption->target_id.isValid() || caption->label < CaptionLabel::figure || caption->label > CaptionLabel::equation))
        return Error{ErrorCode::invalid_operation, "Caption metadata is invalid"};
    if (caption && paragraphs_[*index].bibliography_entry_)
        return Error{ErrorCode::invalid_operation, "A bibliography entry cannot also be a caption"};
    paragraphs_[*index].caption_ = caption;
    return {};
}
Result<void> Document::setBibliographyEntry(NodeId id, bool enabled) {
    const auto index = paragraphIndex(id);
    if (!index) return Error{ErrorCode::invalid_node_id, "Bibliography paragraph does not exist"};
    if (enabled && paragraphs_[*index].caption_) return Error{ErrorCode::invalid_operation, "A caption cannot also be a bibliography entry"};
    paragraphs_[*index].bibliography_entry_ = enabled;
    return {};
}
Result<void> Document::insertToc(std::optional<NodeId> before, TocBlock toc) {
    if (!toc.id.isValid()) return Error{ErrorCode::invalid_node_id, "TOC ID cannot be zero"};
    if (nodeIdInUse(toc.id)) return Error{ErrorCode::duplicate_node_id, "TOC ID already exists"};
    if (toc.max_level < 1 || toc.max_level > 9 || tocs_.size() >= 16)
        return Error{ErrorCode::invalid_operation, "TOC level or count is invalid"};
    const auto index = before ? bodyBlockIndex(*before) : std::optional<std::size_t>{body_blocks_.size()};
    if (!index) return Error{ErrorCode::invalid_node_id, "TOC insertion target does not exist"};
    body_blocks_.insert(body_blocks_.begin() + static_cast<std::ptrdiff_t>(*index), {BodyBlockKind::table_of_contents, toc.id});
    tocs_.push_back(toc);
    return {};
}
Result<void> Document::configureToc(NodeId id, std::uint8_t level) {
    if (level < 1 || level > 9) return Error{ErrorCode::invalid_operation, "TOC level must be 1–9"};
    for (auto& toc : tocs_) if (toc.id == id) { toc.max_level = level; return {}; }
    return Error{ErrorCode::invalid_node_id, "TOC does not exist"};
}
Result<void> Document::deleteToc(NodeId id) {
    if (!findToc(id)) return Error{ErrorCode::invalid_node_id, "TOC does not exist"};
    std::erase_if(tocs_, [id](const TocBlock& toc) { return toc.id == id; });
    std::erase_if(body_blocks_, [id](const BodyBlockRef& block) { return block.id == id; });
    return {};
}
Result<void> Document::moveBodyBlock(NodeId id, std::optional<NodeId> before) {
    const auto source = bodyBlockIndex(id);
    if (!source || (before && !bodyBlockIndex(*before))) return Error{ErrorCode::invalid_node_id, "Body block does not exist"};
    if (before == id) return {};
    const auto block = body_blocks_[*source];
    body_blocks_.erase(body_blocks_.begin() + static_cast<std::ptrdiff_t>(*source));
    const auto target = before ? *bodyBlockIndex(*before) : body_blocks_.size();
    body_blocks_.insert(body_blocks_.begin() + static_cast<std::ptrdiff_t>(target), block);
    std::vector<Paragraph> ordered;
    ordered.reserve(paragraphs_.size());
    for (const auto& item : body_blocks_) if (item.kind == BodyBlockKind::paragraph) ordered.push_back(*findParagraph(item.id));
    paragraphs_ = std::move(ordered);
    return {};
}
void Document::removeUnreferencedNotes() {
    std::unordered_set<NodeId, NodeIdHash> referenced;
    for (const auto& paragraph : paragraphs_) for (const auto& field : paragraph.fields())
        if (field.kind == FieldKind::note_reference) referenced.insert(field.target_id);
    std::erase_if(notes_, [&referenced](const Note& note) { return !referenced.contains(note.id); });
}

const ResolvedField* AcademicResolution::findField(NodeId id) const noexcept {
    const auto found = std::find_if(fields.begin(), fields.end(), [id](const ResolvedField& field) { return field.id == id; });
    return found == fields.end() ? nullptr : &*found;
}
std::u16string AcademicResolution::fieldText(NodeId id) const {
    const auto* field = findField(id);
    return field ? field->text : u"Reference unavailable";
}
std::u16string captionLabelText(CaptionLabel label) {
    switch (label) {
        case CaptionLabel::figure: return u"Figure";
        case CaptionLabel::table: return u"Table";
        case CaptionLabel::equation: return u"Equation";
    }
    return {};
}
namespace {
std::u16string decimal(std::size_t number) {
    const auto text = std::to_string(number);
    return {text.begin(), text.end()};
}
std::u16string equationSourceUtf16(std::string_view source) {
    // Equation source is validated UTF-8 at the core insertion boundary.
    std::u16string result;
    for (std::size_t index = 0; index < source.size();) {
        const auto first = static_cast<unsigned char>(source[index++]);
        std::uint32_t scalar = first;
        std::size_t remaining = 0;
        if (first >= 0xf0U) { scalar = first & 7U; remaining = 3; }
        else if (first >= 0xe0U) { scalar = first & 15U; remaining = 2; }
        else if (first >= 0xc0U) { scalar = first & 31U; remaining = 1; }
        while (remaining-- > 0 && index < source.size()) scalar =
            (scalar << 6U) | (static_cast<unsigned char>(source[index++]) & 63U);
        if (scalar <= 0xffffU) result += static_cast<char16_t>(scalar);
        else {
            scalar -= 0x10000U;
            result += static_cast<char16_t>(0xd800U + (scalar >> 10U));
            result += static_cast<char16_t>(0xdc00U + (scalar & 1023U));
        }
    }
    return result;
}
std::u16string sourceText(const Paragraph& paragraph) {
    std::u16string text;
    for (const auto code : paragraph.text()) if (code != kInlineObjectReplacementCharacter) text += code;
    while (!text.empty() && (text.front() == u' ' || text.front() == u':' || text.front() == u'.')) text.erase(text.begin());
    return text;
}
}
AcademicResolution resolveAcademic(const Document& document) {
    AcademicResolution result;
    std::size_t caption_counts[3]{};
    std::size_t bibliography_count = 0;
    for (const auto& block : document.bodyBlocks()) {
        if (block.kind != BodyBlockKind::paragraph) continue;
        const auto* paragraph = document.findParagraph(block.id);
        if (!paragraph) continue;
        if (paragraph->caption()) {
            const auto label = static_cast<std::size_t>(paragraph->caption()->label);
            result.caption_numbers.emplace(paragraph->id(), ++caption_counts[label]);
        }
        if (paragraph->isBibliographyEntry()) result.bibliography_numbers.emplace(paragraph->id(), ++bibliography_count);
        if (paragraph->styleId()) {
            const auto* style = findBuiltInParagraphStyle(*paragraph->styleId());
            if (style && style->outline_level) result.toc_entries.push_back(
                {paragraph->id(), static_cast<std::uint8_t>(*style->outline_level + 1), sourceText(*paragraph)});
        }
    }
    std::size_t note_counts[2]{};
    std::unordered_map<NodeId, std::size_t, NodeIdHash> note_numbers;
    for (const auto& block : document.bodyBlocks()) {
        if (block.kind != BodyBlockKind::paragraph) continue;
        const auto* paragraph = document.findParagraph(block.id);
        if (!paragraph) continue;
        for (const auto& field : paragraph->fields()) {
            ResolvedField resolved{field.id, field.target_id, {}, false};
            const auto* target = document.findParagraph(field.target_id);
            if (field.kind == FieldKind::note_reference) {
                const auto* note = document.findNote(field.target_id);
                if (note) {
                    auto [position, inserted] = note_numbers.emplace(note->id, 0);
                    if (inserted) {
                        position->second = ++note_counts[static_cast<std::size_t>(note->kind)];
                        result.notes.push_back({note->id, field.id, note->kind, position->second, note->text});
                    }
                    resolved.text = decimal(position->second);
                }
            } else if (field.kind == FieldKind::citation || field.kind == FieldKind::bibliography_number) {
                const auto number = result.bibliography_numbers.find(field.target_id);
                if (number != result.bibliography_numbers.end()) {
                    resolved.text = decimal(number->second);
                    if (field.kind == FieldKind::citation) {
                        if (!field.locator.empty()) resolved.text += u", " + field.locator;
                        resolved.text = u"[" + resolved.text + u"]";
                    }
                }
            } else if (target && target->caption()) {
                const auto number = result.caption_numbers.find(target->id());
                if (number != result.caption_numbers.end()) {
                    resolved.text = captionLabelText(target->caption()->label) + u" " + decimal(number->second);
                    if (field.kind == FieldKind::cross_reference && field.reference_format == CrossReferenceFormat::full_text)
                        resolved.text += u": " + sourceText(*target);
                }
            } else if (field.kind == FieldKind::cross_reference && target && target->styleId()) {
                const auto* style = findBuiltInParagraphStyle(*target->styleId());
                if (style && style->outline_level) resolved.text = sourceText(*target);
            }
            if (resolved.text.empty()) {
                resolved.text = u"Reference unavailable";
                resolved.unresolved = true;
                result.diagnostics.push_back("Unresolved academic field " + field.id.toString());
            }
            result.fields.push_back(std::move(resolved));
        }
    }
    return result;
}
std::u16string academicParagraphText(const Paragraph& paragraph, const AcademicResolution& resolution) {
    std::u16string text;
    for (std::size_t offset = 0; offset < paragraph.text().size(); ++offset) {
        if (const auto* field = paragraph.fieldAt(offset)) text += resolution.fieldText(field->id);
        else if (const auto* equation = paragraph.equationAt(offset)) {
            text += u"\\(";
            text += equationSourceUtf16(equation->canonical_latex);
            text += u"\\)";
        } else if (paragraph.text()[offset] != kInlineObjectReplacementCharacter) text += paragraph.text()[offset];
    }
    return text;
}
} // namespace docxstudio::core
