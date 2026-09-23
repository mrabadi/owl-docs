#pragma once

#include "docxstudio/core/document.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace docxstudio::core {

struct InsertText {
    Position position;
    std::u16string text;
    std::optional<CharacterFormat> format;
};

struct InsertEquation {
    Position position;
    std::string canonical_latex;
    bool display{false};
    NodeId equation_id{NodeId::generate()};
    std::optional<CharacterFormat> format;
};

struct InsertImage {
    Position position;
    EncodedImagePayload encoded_payload;
    ImageFormat image_format{ImageFormat::png};
    std::string accessible_name;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
    NodeId image_id{NodeId::generate()};
    std::optional<CharacterFormat> character_format;
    ImageLayout layout{};

    InsertImage(
        Position target_position, EncodedImagePayload payload,
        ImageFormat format, std::string name, std::int64_t width,
        std::int64_t height, NodeId id = NodeId::generate(),
        std::optional<CharacterFormat> character_formatting = std::nullopt,
        ImageLayout image_layout = {})
        : position(target_position), encoded_payload(std::move(payload)),
          image_format(format), accessible_name(std::move(name)),
          width_emu(width), height_emu(height), image_id(id),
          character_format(std::move(character_formatting)),
          layout(image_layout) {}
};

struct ResizeImage {
    NodeId image_id;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
};

// Replaces the rendered bytes of an existing image while retaining its
// semantic identity, anchor, alt text, and wrapping. This is used by embedded
// figure editors so a save is one undoable document transaction.
struct ReplaceImagePayload {
    NodeId image_id;
    EncodedImagePayload encoded_payload;
    ImageFormat image_format{ImageFormat::png};
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
};

struct SetImageLayout {
    NodeId image_id;
    ImageLayout layout;
};

struct SetImageAccessibleName {
    NodeId image_id;
    std::string accessible_name;
};

struct DeleteRange {
    Range range;
    // When deletion leaves its surviving paragraph empty, callers may supply
    // the active insertion format that cannot be inferred from deleted text.
    std::optional<CharacterFormat> empty_paragraph_format;

    DeleteRange(
        Range deleted_range,
        std::optional<CharacterFormat> resulting_empty_format = std::nullopt)
        : range(deleted_range),
          empty_paragraph_format(std::move(resulting_empty_format)) {}
};

struct ReplaceRange {
    Range range;
    std::u16string text;
    std::optional<CharacterFormat> format;
};

struct SetCharacterFormat {
    Range range;
    CharacterFormatDelta delta;
};

struct SetParagraphMarkCharacterFormat {
    NodeId paragraph_id;
    CharacterFormatDelta delta;
};

struct SetParagraphFormat {
    std::vector<NodeId> paragraph_ids;
    ParagraphFormatDelta delta;
};

// Style identity is distinct from the effective/direct formatting currently
// stored on the paragraph. UI callers can combine this with catalog baseline
// deltas in one atomic batch, while importers can retain an unknown custom ID.
struct SetParagraphStyle {
    std::vector<NodeId> paragraph_ids;
    std::optional<std::string> style_id;
};

// Style changes are assembled as atomic batches: effective formatting is
// rebased first, then the target baseline and exact direct-override masks are
// attached. Keeping this as an operation makes that provenance participate in
// preview, undo, and revision handling just like the visible style change.
struct SetParagraphStyleProvenance {
    NodeId paragraph_id;
    std::optional<ParagraphStyleProvenance> provenance;
};

struct SplitParagraph {
    Position position;
    // Callers may retain this ID and refer to the new paragraph in a later
    // operation in the same sequential batch.
    NodeId new_paragraph_id{NodeId::generate()};
    // When present, this is the caller's active typing format at the split.
    // Otherwise the core derives the new mark from the caret context.
    std::optional<CharacterFormat> new_paragraph_mark_format;

    SplitParagraph(
        Position split_position, NodeId new_id = NodeId::generate(),
        std::optional<CharacterFormat> mark_format = std::nullopt)
        : position(split_position), new_paragraph_id(new_id),
          new_paragraph_mark_format(std::move(mark_format)) {}
};

struct MergeWithNextParagraph {
    NodeId paragraph_id;
};

// A missing before_block_id means append at the end of the body. Callers can
// split the paragraph at the caret earlier in the same batch and use the new
// paragraph ID here, making insertion and its surrounding paragraph split one
// atomic undo transaction.
struct InsertTable {
    std::optional<NodeId> before_block_id;
    Table table;
};

struct SetTableCellText {
    NodeId table_id;
    std::size_t row{0};
    std::size_t column{0};
    std::u16string text;
    std::optional<CharacterFormat> inserted_format;

    SetTableCellText(NodeId target_table_id, std::size_t target_row,
                     std::size_t target_column, std::u16string replacement,
                     std::optional<CharacterFormat> format = std::nullopt)
        : table_id(target_table_id), row(target_row),
          column(target_column), text(std::move(replacement)),
          inserted_format(std::move(format)) {}
};

// Replaces one UTF-16 range without rebuilding the rest of the cell. This is
// the table-cell analogue of ReplaceRange and preserves unrelated sparse run
// formatting when several search matches are replaced in one atomic batch.
struct ReplaceTableCellRange {
    NodeId table_id;
    std::size_t row{0};
    std::size_t column{0};
    std::size_t start{0};
    std::size_t end{0};
    std::u16string text;
    std::optional<CharacterFormat> format;
};

struct SetTableCellCharacterFormat {
    NodeId table_id;
    std::size_t row{0};
    std::size_t column{0};
    std::size_t start{0};
    std::size_t end{0};
    CharacterFormatDelta delta;
};

struct SetTableCellParagraphFormat {
    NodeId table_id;
    std::size_t row{0};
    std::size_t column{0};
    ParagraphFormatDelta delta;
};

// Cell identities are supplied by the caller so a requested edit is fully
// deterministic and can be validated before it reaches the live document.
struct AppendTableRow {
    NodeId table_id;
    std::vector<NodeId> cell_ids;
};

struct InsertTableRow {
    NodeId table_id;
    std::size_t index{0};
    std::vector<NodeId> cell_ids;
    TableInsertionSource inheritance_source{TableInsertionSource::preceding};
};

struct DeleteTableRows {
    NodeId table_id;
    std::size_t index{0};
    std::size_t count{1};
};

struct InsertTableColumn {
    NodeId table_id;
    std::size_t index{0};
    std::vector<NodeId> cell_ids;
    TableInsertionSource inheritance_source{TableInsertionSource::preceding};
};

struct DeleteTableColumns {
    NodeId table_id;
    std::size_t index{0};
    std::size_t count{1};
};

struct SetTableStyle {
    NodeId table_id;
    TableStyle style{TableStyle::grid};
};

struct MoveTable {
    NodeId table_id;
    std::optional<NodeId> before_block_id;
};

struct DeleteTable {
    NodeId table_id;
};

struct SetHeaderText {
    std::u16string text;
};

struct SetFooterText {
    std::u16string text;
};

struct InsertHeaderFooterImage {
    bool footer{false};
    std::size_t utf16_offset{0};
    EncodedImagePayload encoded_payload;
    ImageFormat image_format{ImageFormat::png};
    std::string accessible_name;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
    NodeId image_id{NodeId::generate()};
};

using Operation = std::variant<InsertText, InsertEquation, InsertImage,
                               ResizeImage, ReplaceImagePayload, SetImageLayout,
                               SetImageAccessibleName, DeleteRange, ReplaceRange,
                               SetCharacterFormat,
                               SetParagraphMarkCharacterFormat,
                               SetParagraphFormat, SetParagraphStyle,
                               SetParagraphStyleProvenance,
                               SplitParagraph, MergeWithNextParagraph,
                               InsertTable, SetTableCellText,
                               ReplaceTableCellRange,
                               SetTableCellCharacterFormat,
                               SetTableCellParagraphFormat, AppendTableRow,
                               InsertTableRow, DeleteTableRows,
                               InsertTableColumn, DeleteTableColumns,
                               SetTableStyle, MoveTable, DeleteTable,
                               SetHeaderText, SetFooterText,
                               InsertHeaderFooterImage>;

}  // namespace docxstudio::core
