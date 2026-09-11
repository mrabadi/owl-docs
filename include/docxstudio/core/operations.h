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

struct DeleteRange {
    Range range;
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

struct SetParagraphFormat {
    std::vector<NodeId> paragraph_ids;
    ParagraphFormatDelta delta;
};

struct SplitParagraph {
    Position position;
    // Callers may retain this ID and refer to the new paragraph in a later
    // operation in the same sequential batch.
    NodeId new_paragraph_id{NodeId::generate()};
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

using Operation = std::variant<InsertText, InsertEquation, DeleteRange, ReplaceRange,
                               SetCharacterFormat, SetParagraphFormat, SplitParagraph,
                               MergeWithNextParagraph, InsertTable, SetTableCellText,
                               SetTableCellCharacterFormat,
                               SetTableCellParagraphFormat, AppendTableRow,
                               InsertTableRow, DeleteTableRows, InsertTableColumn,
                               DeleteTableColumns, SetTableStyle, MoveTable,
                               DeleteTable>;

}  // namespace docxstudio::core
