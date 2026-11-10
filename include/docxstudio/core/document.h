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

enum class TableCellVerticalAlignment { top, center, bottom };
enum class TableBorderStyle { none, single };
struct TableBorder {
    TableBorderStyle style{TableBorderStyle::single};
    std::uint16_t width_eighth_points{4};
    std::uint32_t argb{0xff000000U};
    auto operator<=>(const TableBorder&) const = default;
};
struct TableCellLayout {
    std::optional<TableCellVerticalAlignment> vertical_alignment;
    std::optional<TableBorder> border_top, border_right, border_bottom, border_left;
    [[nodiscard]] bool empty() const noexcept { return !vertical_alignment && !border_top && !border_right && !border_bottom && !border_left; }
    [[nodiscard]] Result<void> validate() const;
    auto operator<=>(const TableCellLayout&) const = default;
};
struct TableGeometry {
    std::vector<std::uint32_t> column_widths_twips;
    std::vector<std::uint32_t> row_minimum_heights_twips;
    [[nodiscard]] bool empty() const noexcept { return column_widths_twips.empty() && row_minimum_heights_twips.empty(); }
    [[nodiscard]] Result<void> validate(std::size_t rows, std::size_t columns) const;
    auto operator<=>(const TableGeometry&) const = default;
};

struct TableCell {
    NodeId id;
    std::u16string text;
    std::vector<FormatRun> character_formats;
    ParagraphFormat paragraph_format;
    // Formatting applied to the end-of-cell marker. It is the insertion
    // format for an empty cell and the inherited format outside sparse runs.
    CharacterFormat default_character_format;
    std::vector<ImageAtom> images;
    TableCellLayout layout;

    TableCell(NodeId cell_id, std::u16string cell_text,
              std::vector<FormatRun> formats = {},
              ParagraphFormat format = {},
              CharacterFormat default_format = {},
              std::vector<ImageAtom> inline_images = {},
              TableCellLayout cell_layout = {})
        : id(cell_id), text(std::move(cell_text)),
          character_formats(std::move(formats)),
          paragraph_format(std::move(format)),
          default_character_format(std::move(default_format)),
          images(std::move(inline_images)), layout(std::move(cell_layout)) {}

    [[nodiscard]] const ImageAtom* imageAt(std::size_t offset) const noexcept;
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
        std::optional<TableStyle> style = TableStyle::grid,
        TableGeometry geometry = {});

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] std::size_t rowCount() const noexcept { return rows_; }
    [[nodiscard]] std::size_t columnCount() const noexcept { return columns_; }
    [[nodiscard]] bool hasHeaderRow() const noexcept { return header_row_; }
    [[nodiscard]] std::optional<TableStyle> style() const noexcept {
        return style_;
    }
    [[nodiscard]] const TableGeometry& geometry() const noexcept { return geometry_; }
    [[nodiscard]] const std::vector<TableCell>& cells() const noexcept { return cells_; }
    [[nodiscard]] const TableCell* cell(std::size_t row, std::size_t column) const noexcept;

    auto operator<=>(const Table&) const = default;

private:
    Table(NodeId id, std::size_t rows, std::size_t columns, bool header_row,
          std::vector<TableCell> cells, std::optional<TableStyle> style, TableGeometry geometry = {});
    [[nodiscard]] Result<void> setCellText(std::size_t row, std::size_t column,
                                           std::u16string text,
                                           const std::optional<CharacterFormat>&
                                               inserted_format);
    [[nodiscard]] Result<void> replaceCellRange(
        std::size_t row, std::size_t column, std::size_t start,
        std::size_t end, const std::u16string& text,
        const std::optional<CharacterFormat>& inserted_format);
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
    TableGeometry geometry_;

    friend class Document;
};

enum class BodyBlockKind { paragraph, table, table_of_contents };

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

enum class NoteKind { footnote, endnote };
enum class CaptionLabel { figure, table, equation };
enum class FieldKind { note_reference, caption_number, bibliography_number,
                       cross_reference, citation };
enum class CrossReferenceFormat { label_number, full_text };

struct FieldAtom {
    NodeId id;
    std::size_t utf16_offset{0};
    FieldKind kind{FieldKind::cross_reference};
    NodeId target_id;
    std::u16string locator{};
    CrossReferenceFormat reference_format{CrossReferenceFormat::label_number};
    auto operator<=>(const FieldAtom&) const = default;
};

struct Note {
    NodeId id;
    NoteKind kind{NoteKind::footnote};
    std::u16string text;
    auto operator<=>(const Note&) const = default;
};

struct CaptionMetadata {
    CaptionLabel label{CaptionLabel::figure};
    NodeId target_id;
    auto operator<=>(const CaptionMetadata&) const = default;
};

struct TocBlock {
    NodeId id;
    std::uint8_t max_level{3};
    auto operator<=>(const TocBlock&) const = default;
};

inline constexpr std::size_t kMaximumAcademicFields = 10000;
inline constexpr std::size_t kMaximumNotes = 2048;
inline constexpr std::size_t kMaximumNoteCodeUnits = 32768;
inline constexpr std::size_t kMaximumNoteAggregateCodeUnits = 1024U * 1024U;

// A DOCX paragraph style can have the same identity as one of Owl Docs'
// built-ins while resolving to a different visual definition in the source
// package.  Keep that resolved source baseline separate from the effective
// formats stored on the paragraph.  Style transitions can then distinguish
// inherited appearance from direct run/paragraph overrides without making
// the portable DOCX depend on application-private metadata.
struct ParagraphStyleProvenance {
    CharacterFormat inherited_character_format;
    // Effective insertion/paragraph-mark baseline contributed by docDefaults
    // and the paragraph-style chain.  This can differ from ordinary run
    // inheritance when a style carries paragraph-mark run properties.
    CharacterFormat inherited_paragraph_mark_character_format;
    ParagraphFormat inherited_paragraph_format;
    struct OverrideRun {
        std::size_t start{0};
        std::size_t end{0};
        CharacterFormatMask properties;

        auto operator<=>(const OverrideRun&) const = default;
    };
    std::vector<OverrideRun> character_overrides;
    CharacterFormatMask paragraph_mark_overrides;
    ParagraphFormatMask paragraph_overrides;

    auto operator<=>(const ParagraphStyleProvenance&) const = default;
};

class Paragraph {
public:
    Paragraph();

    [[nodiscard]] static Result<Paragraph> create(
        std::u16string text, NodeId id = NodeId::generate(),
        CharacterFormat paragraph_mark_character_format = {},
        std::optional<std::string> style_id = std::nullopt,
        std::optional<ParagraphStyleProvenance> style_provenance =
            std::nullopt);
    // Recovery/import code uses the same validation as ordinary creation but
    // may make the intent to retain an external style identity explicit.
    [[nodiscard]] static Result<Paragraph> restore(
        std::u16string text, NodeId id,
        CharacterFormat paragraph_mark_character_format,
        std::optional<std::string> style_id = std::nullopt,
        std::optional<ParagraphStyleProvenance> style_provenance =
            std::nullopt);

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] const std::u16string& text() const noexcept { return text_; }
    [[nodiscard]] const std::vector<FormatRun>& characterFormats() const noexcept {
        return character_formats_;
    }
    [[nodiscard]] const std::vector<EquationAtom>& equations() const noexcept {
        return equations_;
    }
    [[nodiscard]] const EquationAtom* equationAt(std::size_t utf16_offset) const noexcept;
    [[nodiscard]] const std::vector<FieldAtom>& fields() const noexcept { return fields_; }
    [[nodiscard]] const FieldAtom* fieldAt(std::size_t utf16_offset) const noexcept;
    [[nodiscard]] const std::optional<CaptionMetadata>& caption() const noexcept { return caption_; }
    [[nodiscard]] bool isBibliographyEntry() const noexcept { return bibliography_entry_; }
    [[nodiscard]] const std::vector<ImageAtom>& images() const noexcept {
        return images_;
    }
    [[nodiscard]] const ImageAtom* imageAt(
        std::size_t utf16_offset) const noexcept;
    [[nodiscard]] const ParagraphFormat& format() const noexcept { return format_; }
    [[nodiscard]] const std::optional<std::string>& styleId() const noexcept {
        return style_id_;
    }
    [[nodiscard]] const std::optional<ParagraphStyleProvenance>&
    styleProvenance() const noexcept {
        return style_provenance_;
    }
    [[nodiscard]] CharacterFormatMask styleOverrideMaskAt(
        std::size_t utf16_offset) const noexcept;
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
              CharacterFormat paragraph_mark_character_format = {},
              std::optional<std::string> style_id = std::nullopt,
              std::optional<ParagraphStyleProvenance> style_provenance =
                  std::nullopt);

    [[nodiscard]] std::vector<CharacterFormat> denseFormats() const;
    [[nodiscard]] std::vector<CharacterFormatMask>
    denseStyleOverrideMasks() const;
    void setContent(std::u16string text, std::vector<CharacterFormat> formats);
    void setStyleOverrideMasks(std::vector<CharacterFormatMask> masks);
    [[nodiscard]] Result<void> insertText(std::size_t offset, const std::u16string& text,
                                          const std::optional<CharacterFormat>& format);
    [[nodiscard]] Result<void> insertEquation(
        std::size_t offset, EquationAtom equation,
        const std::optional<CharacterFormat>& format);
    [[nodiscard]] Result<void> insertField(
        std::size_t offset, FieldAtom field,
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
    std::vector<FieldAtom> fields_;
    std::optional<CaptionMetadata> caption_;
    bool bibliography_entry_{false};
    std::vector<ImageAtom> images_;
    ParagraphFormat format_;
    CharacterFormat paragraph_mark_character_format_;
    std::optional<std::string> style_id_;
    std::optional<ParagraphStyleProvenance> style_provenance_;

    friend class Document;
};

// Coordinates of the replaced range in the previous story text. This makes
// adjacent image deletion unambiguous without changing persisted snapshots.
struct StoryTextEditRange {
    std::size_t start{0};
    std::size_t length{0};

    auto operator<=>(const StoryTextEditRange&) const = default;
};

class Document {
public:
    static constexpr std::size_t maximum_header_footer_code_units = 32768;
    Document();

    [[nodiscard]] static Result<Document> create(std::vector<Paragraph> paragraphs);

    [[nodiscard]] const std::vector<Paragraph>& paragraphs() const noexcept { return paragraphs_; }
    [[nodiscard]] const std::vector<Table>& tables() const noexcept { return tables_; }
    [[nodiscard]] const std::vector<Note>& notes() const noexcept { return notes_; }
    [[nodiscard]] const std::vector<TocBlock>& tablesOfContents() const noexcept { return tocs_; }
    [[nodiscard]] const std::vector<BodyBlockRef>& bodyBlocks() const noexcept {
        return body_blocks_;
    }
    [[nodiscard]] const std::u16string& headerText() const noexcept {
        return header_text_;
    }
    [[nodiscard]] const std::u16string& footerText() const noexcept {
        return footer_text_;
    }
    [[nodiscard]] const std::vector<ImageAtom>& headerImages() const noexcept {
        return header_images_;
    }
    [[nodiscard]] const std::vector<ImageAtom>& footerImages() const noexcept {
        return footer_images_;
    }
    [[nodiscard]] const Paragraph* findParagraph(NodeId id) const noexcept;
    [[nodiscard]] const Table* findTable(NodeId id) const noexcept;
    [[nodiscard]] const EquationAtom* findEquation(NodeId id) const noexcept;
    [[nodiscard]] const FieldAtom* findField(NodeId id) const noexcept;
    [[nodiscard]] const Note* findNote(NodeId id) const noexcept;
    [[nodiscard]] const TocBlock* findToc(NodeId id) const noexcept;
    [[nodiscard]] const ImageAtom* findImage(NodeId id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> paragraphIndex(NodeId id) const noexcept;
    [[nodiscard]] Result<NormalizedRange> normalizeRange(const Range& range) const;

    [[nodiscard]] Result<void> insertText(const Position& position, const std::u16string& text,
                                          const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> insertEquation(
        const Position& position, std::string canonical_latex, bool display = false,
        NodeId equation_id = NodeId::generate(),
        const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> replaceEquation(
        NodeId equation_id, std::string canonical_latex, bool display);
    [[nodiscard]] Result<void> insertField(
        const Position& position, FieldAtom field,
        const std::optional<CharacterFormat>& format = std::nullopt);
    [[nodiscard]] Result<void> replaceField(NodeId field_id, FieldAtom field);
    [[nodiscard]] Result<void> setNote(Note note);
    [[nodiscard]] Result<void> setCaption(NodeId paragraph_id, std::optional<CaptionMetadata> caption);
    [[nodiscard]] Result<void> setBibliographyEntry(NodeId paragraph_id, bool enabled);
    [[nodiscard]] Result<void> insertToc(std::optional<NodeId> before_block_id, TocBlock toc);
    [[nodiscard]] Result<void> configureToc(NodeId toc_id, std::uint8_t max_level);
    [[nodiscard]] Result<void> deleteToc(NodeId toc_id);
    [[nodiscard]] Result<void> moveBodyBlock(NodeId block_id, std::optional<NodeId> before_block_id);
    [[nodiscard]] Result<void> insertImage(
        const Position& position, EncodedImagePayload encoded_payload,
        ImageFormat image_format, std::string accessible_name,
        std::int64_t width_emu, std::int64_t height_emu,
        NodeId image_id = NodeId::generate(),
        const std::optional<CharacterFormat>& character_format = std::nullopt,
        ImageLayout layout = {});
    [[nodiscard]] Result<void> resizeImage(
        NodeId image_id, std::int64_t width_emu, std::int64_t height_emu);
    [[nodiscard]] Result<void> replaceImagePayload(
        NodeId image_id, EncodedImagePayload encoded_payload,
        ImageFormat image_format, std::int64_t width_emu,
        std::int64_t height_emu);
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
    [[nodiscard]] Result<void> setParagraphStyle(
        const std::vector<NodeId>& paragraph_ids,
        std::optional<std::string> style_id);
    // Import and recovery attach provenance only after restoring effective
    // formatting so those restoration operations are not mistaken for user
    // direct formatting.
    [[nodiscard]] Result<void> setParagraphStyleProvenance(
        NodeId paragraph_id,
        std::optional<ParagraphStyleProvenance> provenance);
    [[nodiscard]] Result<void> splitParagraph(
        const Position& position, NodeId new_paragraph_id,
        std::optional<CharacterFormat> new_paragraph_mark_format = std::nullopt);
    [[nodiscard]] Result<void> mergeWithNext(NodeId paragraph_id);
    [[nodiscard]] Result<void> insertTable(std::optional<NodeId> before_block_id,
                                           Table table);
    [[nodiscard]] Result<void> insertTableCellImage(
        NodeId table_id, std::size_t row, std::size_t column,
        std::size_t utf16_offset, EncodedImagePayload encoded_payload,
        ImageFormat image_format, std::string accessible_name,
        std::int64_t width_emu, std::int64_t height_emu,
        NodeId image_id = NodeId::generate(),
        const std::optional<CharacterFormat>& character_format = std::nullopt);
    [[nodiscard]] Result<void> setTableGeometry(NodeId table_id, TableGeometry geometry);
    [[nodiscard]] Result<void> setTableCellLayout(NodeId table_id, std::size_t row, std::size_t column, TableCellLayout layout);
    [[nodiscard]] Result<void> setTableCellContent(
        NodeId table_id, std::size_t row, std::size_t column, TableCell content);
    [[nodiscard]] Result<void> setTableCellText(NodeId table_id, std::size_t row,
                                                std::size_t column,
                                                std::u16string text,
                                                const std::optional<CharacterFormat>&
                                                    inserted_format = std::nullopt);
    [[nodiscard]] Result<void> replaceTableCellRange(
        NodeId table_id, std::size_t row, std::size_t column,
        std::size_t start, std::size_t end, const std::u16string& text,
        const std::optional<CharacterFormat>& inserted_format = std::nullopt);
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
    [[nodiscard]] Result<void> setHeaderText(
        std::u16string text,
        std::optional<StoryTextEditRange> replaced_range = std::nullopt);
    [[nodiscard]] Result<void> setFooterText(
        std::u16string text,
        std::optional<StoryTextEditRange> replaced_range = std::nullopt);
    [[nodiscard]] Result<void> insertHeaderFooterImage(
        bool footer, std::size_t utf16_offset,
        EncodedImagePayload encoded_payload, ImageFormat image_format,
        std::string accessible_name, std::int64_t width_emu,
        std::int64_t height_emu, NodeId image_id = NodeId::generate());

    auto operator<=>(const Document&) const = default;

private:
    explicit Document(std::vector<Paragraph> paragraphs);
    [[nodiscard]] Result<void> validatePosition(const Position& position) const;
    [[nodiscard]] std::optional<std::size_t> tableIndex(NodeId id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> bodyBlockIndex(NodeId id) const noexcept;
    [[nodiscard]] bool nodeIdInUse(NodeId id) const noexcept;
    void removeUnreferencedNotes();

    std::vector<Paragraph> paragraphs_;
    std::vector<Table> tables_;
    std::vector<Note> notes_;
    std::vector<TocBlock> tocs_;
    std::vector<BodyBlockRef> body_blocks_;
    std::u16string header_text_;
    std::u16string footer_text_;
    std::vector<ImageAtom> header_images_;
    std::vector<ImageAtom> footer_images_;
};

[[nodiscard]] bool isValidUtf16(const std::u16string& text) noexcept;
[[nodiscard]] bool isUtf16Boundary(const std::u16string& text, std::size_t offset) noexcept;

}  // namespace docxstudio::core
