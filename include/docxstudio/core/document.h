#pragma once

#include "docxstudio/core/formatting.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace docxstudio::core {

// Inline semantic objects occupy exactly one UTF-16 position in paragraph
// text. Ordinary text APIs reserve this character so an object placeholder can
// never exist without matching metadata.
inline constexpr char16_t kInlineObjectReplacementCharacter = u'\ufffc';

struct FormatRun {
    std::size_t start{0};
    std::size_t end{0};
    CharacterFormat format;

    auto operator<=>(const FormatRun&) const = default;
};

struct Position {
    NodeId paragraph_id;
    std::size_t utf16_offset{0};

    auto operator<=>(const Position&) const = default;
};

// Anchor/focus preserve selection direction. Document::normalizeRange returns
// positions ordered in document order for editing operations.
struct Range {
    Position anchor;
    Position focus;

    auto operator<=>(const Range&) const = default;
};

struct NormalizedRange {
    Position start;
    Position end;
    std::size_t start_paragraph_index{0};
    std::size_t end_paragraph_index{0};

    [[nodiscard]] bool empty() const noexcept { return start == end; }
};

enum class TableStyle {
    plain,
    grid,
    light_gray,
    light_blue,
    light_orange,
    medium_blue,
    medium_green,
    medium_orange,
    aubergine,
    orange_accent,
    banded_blue,
    banded_aubergine,
    dark_header,
};

// Selects which existing neighbor supplies formatting for cells created at a
// row/column insertion boundary. A following source models Insert Above/Left;
// a preceding source models Insert Below/Right and append.
enum class TableInsertionSource { preceding, following };

struct TableCell {
    NodeId id;
    std::u16string text;
    std::vector<FormatRun> character_formats;
    ParagraphFormat paragraph_format;
    // Formatting applied to the end-of-cell marker. It is the insertion
    // format for an empty cell and the inherited format outside sparse runs.
    CharacterFormat default_character_format;

    TableCell(NodeId cell_id, std::u16string cell_text,
              std::vector<FormatRun> formats = {},
              ParagraphFormat format = {},
              CharacterFormat default_format = {})
        : id(cell_id), text(std::move(cell_text)),
          character_formats(std::move(formats)),
          paragraph_format(std::move(format)),
          default_character_format(std::move(default_format)) {}

    [[nodiscard]] CharacterFormat characterFormatAt(
        std::size_t utf16_offset) const;

    auto operator<=>(const TableCell&) const = default;
};

// The current semantic table slice deliberately models a rectangular grid
// with independently formatted cells. More advanced structures can be layered
// on later without representing a table as tab-delimited body text.
class Table {
public:
    static constexpr std::size_t maximum_rows = 50;
    static constexpr std::size_t maximum_columns = 20;

    [[nodiscard]] static Result<Table> create(
        std::size_t rows, std::size_t columns, bool header_row = false,
        NodeId id = NodeId::generate(),
        std::optional<TableStyle> style = TableStyle::grid);
    // Recovery/import paths may restore stable cell identities. Normal table
    // insertion should use create(), which mints fresh cell IDs.
    [[nodiscard]] static Result<Table> restore(
        std::size_t rows, std::size_t columns, bool header_row, NodeId id,
        std::vector<TableCell> cells,
        std::optional<TableStyle> style = TableStyle::grid);

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] std::size_t rowCount() const noexcept { return rows_; }
    [[nodiscard]] std::size_t columnCount() const noexcept { return columns_; }
    [[nodiscard]] bool hasHeaderRow() const noexcept { return header_row_; }
    [[nodiscard]] std::optional<TableStyle> style() const noexcept {
        return style_;
    }
    [[nodiscard]] const std::vector<TableCell>& cells() const noexcept { return cells_; }
    [[nodiscard]] const TableCell* cell(std::size_t row, std::size_t column) const noexcept;

    auto operator<=>(const Table&) const = default;

private:
    Table(NodeId id, std::size_t rows, std::size_t columns, bool header_row,
          std::vector<TableCell> cells, std::optional<TableStyle> style);
    [[nodiscard]] Result<void> setCellText(std::size_t row, std::size_t column,
                                           std::u16string text,
                                           const std::optional<CharacterFormat>&
                                               inserted_format);
    [[nodiscard]] Result<void> applyCellCharacterFormat(
        std::size_t row, std::size_t column, std::size_t start,
        std::size_t end, const CharacterFormatDelta& delta);
    [[nodiscard]] Result<void> applyCellParagraphFormat(
        std::size_t row, std::size_t column,
        const ParagraphFormatDelta& delta);
    [[nodiscard]] Result<void> appendRow(std::vector<NodeId> cell_ids);
    [[nodiscard]] Result<void> insertRow(std::size_t index,
                                         std::vector<NodeId> cell_ids,
                                         TableInsertionSource source);
    [[nodiscard]] Result<void> deleteRows(std::size_t index,
                                          std::size_t count);
    [[nodiscard]] Result<void> insertColumn(std::size_t index,
                                            std::vector<NodeId> cell_ids,
                                            TableInsertionSource source);
    [[nodiscard]] Result<void> deleteColumns(std::size_t index,
                                             std::size_t count);
    void setStyle(TableStyle style) noexcept { style_ = style; }

    NodeId id_;
    std::size_t rows_{0};
    std::size_t columns_{0};
    bool header_row_{false};
    std::optional<TableStyle> style_{TableStyle::grid};
    std::vector<TableCell> cells_;

    friend class Document;
};

enum class BodyBlockKind { paragraph, table };

// BodyBlockRef supplies stable document order without disrupting the current
// paragraph APIs. Paragraphs and tables remain value-owned by Document.
struct BodyBlockRef {
    BodyBlockKind kind{BodyBlockKind::paragraph};
    NodeId id;

    auto operator<=>(const BodyBlockRef&) const = default;
};

struct EquationAtom {
    NodeId id;
    std::size_t utf16_offset{0};
    // Canonical source produced by the inert LaTeX parser. The core validates
    // that this is non-empty, bounded UTF-8; it intentionally does not parse
    // or execute it.
    std::string canonical_latex;
    bool display{false};

    auto operator<=>(const EquationAtom&) const = default;
};

inline constexpr std::size_t kMaximumInlineImagesPerDocument = 512;
inline constexpr std::size_t kMaximumEncodedImageBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaximumDocumentEncodedImageBytes =
    32U * 1024U * 1024U;
inline constexpr std::size_t kMaximumImageAccessibleNameBytes = 4U * 1024U;
inline constexpr std::int64_t kMaximumInlineImageDimensionEmu = 254000000;
inline constexpr std::int64_t kMaximumImageWrapDistanceEmu = 254000000;

enum class ImageFormat : std::uint8_t { png, jpeg };

// Inline images participate in the text line. Square and top-and-bottom images
// are anchored to their containing paragraph and affect surrounding text using
// the corresponding wrap mode. Additional OOXML wrap modes can extend this
// enum without changing the transport-neutral layout record.
enum class ImagePlacement : std::uint8_t {
    inline_with_text,
    square,
    top_and_bottom,
};

struct ImageLayout {
    ImagePlacement placement{ImagePlacement::inline_with_text};
    std::int64_t distance_top_emu{0};
    std::int64_t distance_right_emu{0};
    std::int64_t distance_bottom_emu{0};
    std::int64_t distance_left_emu{0};
    bool move_with_text{true};

    [[nodiscard]] Result<void> validate() const;

    auto operator<=>(const ImageLayout&) const = default;
};

[[nodiscard]] constexpr std::string_view imageContentType(
    ImageFormat format) noexcept {
    switch (format) {
        case ImageFormat::png:
            return "image/png";
        case ImageFormat::jpeg:
            return "image/jpeg";
    }
    return {};
}

// Immutable, byte-owning encoded image data. Copies share storage so copying a
// Document for edits, previews, and undo history never copies the media bytes.
// Equality and ordering remain value-based for Document's value semantics.
class EncodedImagePayload {
public:
    EncodedImagePayload();
    explicit EncodedImagePayload(std::vector<std::uint8_t> bytes);

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] bool operator==(
        const EncodedImagePayload& other) const noexcept;
    [[nodiscard]] std::strong_ordering operator<=>(
        const EncodedImagePayload& other) const noexcept;

private:
    std::shared_ptr<const std::vector<std::uint8_t>> storage_;
};

struct ImageAtom {
    NodeId id;
    std::size_t utf16_offset{0};
    EncodedImagePayload encoded_payload;
    ImageFormat format{ImageFormat::png};
    std::string accessible_name;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
    ImageLayout layout;

    auto operator<=>(const ImageAtom&) const = default;
};

class Paragraph {
public:
    Paragraph();

    [[nodiscard]] static Result<Paragraph> create(
        std::u16string text, NodeId id = NodeId::generate(),
        CharacterFormat paragraph_mark_character_format = {});

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] const std::u16string& text() const noexcept { return text_; }
    [[nodiscard]] const std::vector<FormatRun>& characterFormats() const noexcept {
        return character_formats_;
    }
    [[nodiscard]] const std::vector<EquationAtom>& equations() const noexcept {
        return equations_;
    }
    [[nodiscard]] const EquationAtom* equationAt(std::size_t utf16_offset) const noexcept;
    [[nodiscard]] const std::vector<ImageAtom>& images() const noexcept {
        return images_;
    }
    [[nodiscard]] const ImageAtom* imageAt(
        std::size_t utf16_offset) const noexcept;
    [[nodiscard]] const ParagraphFormat& format() const noexcept { return format_; }
    // Character properties carried by the paragraph mark. They provide the
    // durable insertion format when the paragraph has no text.
    [[nodiscard]] const CharacterFormat& paragraphMarkCharacterFormat()
        const noexcept {
        return paragraph_mark_character_format_;
    }
    [[nodiscard]] CharacterFormat characterFormatAt(std::size_t utf16_offset) const;

    auto operator<=>(const Paragraph&) const = default;

private:
    Paragraph(NodeId id, std::u16string text,
              CharacterFormat paragraph_mark_character_format = {});

    [[nodiscard]] std::vector<CharacterFormat> denseFormats() const;
    void setContent(std::u16string text, std::vector<CharacterFormat> formats);
    [[nodiscard]] Result<void> insertText(std::size_t offset, const std::u16string& text,
                                          const std::optional<CharacterFormat>& format);
    [[nodiscard]] Result<void> insertEquation(
        std::size_t offset, EquationAtom equation,
        const std::optional<CharacterFormat>& format);
    [[nodiscard]] Result<void> insertImage(
        std::size_t offset, ImageAtom image,
        const std::optional<CharacterFormat>& character_format);
    [[nodiscard]] Result<void> erase(std::size_t start, std::size_t end);
    [[nodiscard]] Result<void> applyFormat(std::size_t start, std::size_t end,
                                           const CharacterFormatDelta& delta);

    NodeId id_;
    std::u16string text_;
    std::vector<FormatRun> character_formats_;
    std::vector<EquationAtom> equations_;
    std::vector<ImageAtom> images_;
    ParagraphFormat format_;
    CharacterFormat paragraph_mark_character_format_;

    friend class Document;
};

class Document {
public:
    Document();

    [[nodiscard]] static Result<Document> create(std::vector<Paragraph> paragraphs);

    [[nodiscard]] const std::vector<Paragraph>& paragraphs() const noexcept { return paragraphs_; }
    [[nodiscard]] const std::vector<Table>& tables() const noexcept { return tables_; }
    [[nodiscard]] const std::vector<BodyBlockRef>& bodyBlocks() const noexcept {
        return body_blocks_;
    }
    [[nodiscard]] const Paragraph* findParagraph(NodeId id) const noexcept;
    [[nodiscard]] const Table* findTable(NodeId id) const noexcept;
    [[nodiscard]] const EquationAtom* findEquation(NodeId id) const noexcept;
    [[nodiscard]] const ImageAtom* findImage(NodeId id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> paragraphIndex(NodeId id) const noexcept;
    [[nodiscard]] Result<NormalizedRange> normalizeRange(const Range& range) const;

    [[nodiscard]] Result<void> insertText(const Position& position, const std::u16string& text,
                                          const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> insertEquation(
        const Position& position, std::string canonical_latex, bool display = false,
        NodeId equation_id = NodeId::generate(),
        const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> insertImage(
        const Position& position, EncodedImagePayload encoded_payload,
        ImageFormat image_format, std::string accessible_name,
        std::int64_t width_emu, std::int64_t height_emu,
        NodeId image_id = NodeId::generate(),
        const std::optional<CharacterFormat>& character_format = std::nullopt,
        ImageLayout layout = {});
    [[nodiscard]] Result<void> resizeImage(
        NodeId image_id, std::int64_t width_emu, std::int64_t height_emu);
    [[nodiscard]] Result<void> setImageLayout(NodeId image_id,
                                              ImageLayout layout);
    [[nodiscard]] Result<void> setImageAccessibleName(
        NodeId image_id, std::string accessible_name);
    [[nodiscard]] Result<void> deleteRange(
        const Range& range,
        const std::optional<CharacterFormat>& empty_paragraph_format =
            std::nullopt);
    [[nodiscard]] Result<void> replaceRange(const Range& range, const std::u16string& text,
                                            const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> applyCharacterFormat(const Range& range,
                                                    const CharacterFormatDelta& delta);
    [[nodiscard]] Result<void> applyParagraphMarkCharacterFormat(
        NodeId paragraph_id, const CharacterFormatDelta& delta);
    [[nodiscard]] Result<void> applyParagraphFormat(const std::vector<NodeId>& paragraph_ids,
                                                    const ParagraphFormatDelta& delta);
    [[nodiscard]] Result<void> splitParagraph(
        const Position& position, NodeId new_paragraph_id,
        std::optional<CharacterFormat> new_paragraph_mark_format = std::nullopt);
    [[nodiscard]] Result<void> mergeWithNext(NodeId paragraph_id);
    [[nodiscard]] Result<void> insertTable(std::optional<NodeId> before_block_id,
                                           Table table);
    [[nodiscard]] Result<void> setTableCellText(NodeId table_id, std::size_t row,
                                                std::size_t column,
                                                std::u16string text,
                                                const std::optional<CharacterFormat>&
                                                    inserted_format = std::nullopt);
    [[nodiscard]] Result<void> applyTableCellCharacterFormat(
        NodeId table_id, std::size_t row, std::size_t column,
        std::size_t start, std::size_t end,
        const CharacterFormatDelta& delta);
    [[nodiscard]] Result<void> applyTableCellParagraphFormat(
        NodeId table_id, std::size_t row, std::size_t column,
        const ParagraphFormatDelta& delta);
    [[nodiscard]] Result<void> appendTableRow(NodeId table_id,
                                              std::vector<NodeId> cell_ids);
    [[nodiscard]] Result<void> insertTableRow(NodeId table_id,
                                              std::size_t index,
                                              std::vector<NodeId> cell_ids,
                                              TableInsertionSource source =
                                                  TableInsertionSource::preceding);
    [[nodiscard]] Result<void> deleteTableRows(NodeId table_id,
                                              std::size_t index,
                                              std::size_t count);
    [[nodiscard]] Result<void> insertTableColumn(NodeId table_id,
                                                 std::size_t index,
                                                 std::vector<NodeId> cell_ids,
                                                 TableInsertionSource source =
                                                     TableInsertionSource::preceding);
    [[nodiscard]] Result<void> deleteTableColumns(NodeId table_id,
                                                 std::size_t index,
                                                 std::size_t count);
    [[nodiscard]] Result<void> setTableStyle(NodeId table_id,
                                             TableStyle style);
    [[nodiscard]] Result<void> moveTable(NodeId table_id,
                                         std::optional<NodeId> before_block_id);
    [[nodiscard]] Result<void> deleteTable(NodeId table_id);

    auto operator<=>(const Document&) const = default;

private:
    explicit Document(std::vector<Paragraph> paragraphs);
    [[nodiscard]] Result<void> validatePosition(const Position& position) const;
    [[nodiscard]] std::optional<std::size_t> tableIndex(NodeId id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> bodyBlockIndex(NodeId id) const noexcept;
    [[nodiscard]] bool nodeIdInUse(NodeId id) const noexcept;

    std::vector<Paragraph> paragraphs_;
    std::vector<Table> tables_;
    std::vector<BodyBlockRef> body_blocks_;
};

[[nodiscard]] bool isValidUtf16(const std::u16string& text) noexcept;
[[nodiscard]] bool isUtf16Boundary(const std::u16string& text, std::size_t offset) noexcept;

}  // namespace docxstudio::core
