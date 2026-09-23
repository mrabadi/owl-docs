#pragma once

#include "docxstudio/raster/validation.h"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace docxstudio::ooxml {

// This is a capability classification, not a claim that the complete Word
// feature set is understood. Unknown package parts are retained as opaque
// bytes and unknown document.xml markup is never regenerated.
enum class CompatibilityClass {
    invalid,
    exact_round_trip_only,
    safe_text_patch,
    basic_body_text_patch,
};

enum class IssueSeverity { information, warning, blocking };

enum class IssueCode {
    unsupported_body_content,
    unsupported_formatting,
    unsupported_text_context,
    non_utf8_document_xml,
    encrypted_package_member,
    digital_signature,
    unsupported_document_compression,
    invalid_text_span,
    structural_rewrite_required,
    whitespace_preservation_required,
    invalid_xml_character,
    invalid_utf8,
};

struct CompatibilityIssue {
    IssueSeverity severity{IssueSeverity::information};
    IssueCode code{IssueCode::unsupported_body_content};
    std::string part_name;
    std::string detail;
    std::optional<std::size_t> paragraph_index;

    auto operator<=>(const CompatibilityIssue&) const = default;
};

struct LossReport {
    std::vector<CompatibilityIssue> issues;

    [[nodiscard]] bool hasBlockers() const noexcept;
    [[nodiscard]] bool safe() const noexcept { return !hasBlockers(); }
};

struct CompatibilityReport {
    CompatibilityClass classification{CompatibilityClass::invalid};
    bool byte_identical_unchanged_save{false};
    bool preserves_unmodified_package_members{false};
    std::vector<CompatibilityIssue> issues;

    [[nodiscard]] bool allowsTextPatching() const noexcept;
};

enum class BasicParagraphAlignment { left, center, right, justified };
enum class BasicBaseline { normal, superscript, subscript };
enum class BasicLineSpacingRule { automatic, at_least, exact };

struct BasicRunFormat {
    std::optional<std::string> font_family;
    std::optional<std::int32_t> font_size_half_points;
    // Presence is significant: false is an explicit OOXML override of an
    // inherited style, while nullopt leaves the property unspecified.
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
    std::optional<bool> strike;
    // 0xRRGGBB. The high byte must be zero.
    std::optional<std::uint32_t> foreground_rgb;
    // Character shading used for arbitrary editor highlight colors.
    std::optional<std::uint32_t> highlight_rgb;
    std::optional<BasicBaseline> baseline;

    auto operator<=>(const BasicRunFormat&) const = default;
};

enum class BasicNumberFormat {
    bullet,
    decimal,
    upper_letter,
    lower_letter,
    upper_roman,
    lower_roman,
};

enum class BasicNumberSuffix { tab, space, nothing };

// Effective numbering information resolved from word/numbering.xml. The
// source numId and ilvl remain available so adjacent paragraphs can retain
// their list identity, while marker_text is the display marker after applying
// the level template and counters (for example "2." or "b)").
struct ImportedNumbering {
    std::int32_t num_id{0};
    std::uint8_t level{0};
    BasicNumberFormat format{BasicNumberFormat::decimal};
    std::int32_t start{1};
    std::string level_text;
    std::string marker_text;
    BasicNumberSuffix suffix{BasicNumberSuffix::tab};
    std::optional<std::int32_t> left_indent_twips;
    std::optional<std::int32_t> first_line_indent_twips;
    std::vector<std::uint32_t> left_tab_stops_twips;
    BasicRunFormat marker_format;

    auto operator<=>(const ImportedNumbering&) const = default;
};

// A safe, inert equation payload. canonical_latex is always accepted by the
// built-in restricted LaTeX parser; it is never executed as TeX.
struct EquationPayload {
    std::string canonical_latex;
    bool display{false};

    auto operator<=>(const EquationPayload&) const = default;
};

inline constexpr std::int64_t kMaximumImageWrapDistanceEmu = 254000000;
inline constexpr std::int64_t kMaximumImageDimensionEmu = 254000000;
inline constexpr std::size_t kMaximumImageAccessibleNameBytes =
    4U * 1024U;

// This transport-neutral subset deliberately models only placements whose
// positioning and wrapping can be regenerated without guessing. Other
// DrawingML anchors remain opaque in the opened package.
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

    auto operator<=>(const ImageLayout&) const = default;
};

// Value-like, immutable byte storage shared by every DrawingML reference to
// one package member. Copying paragraphs or snapshots therefore cannot copy a
// large media member once per reference.
class SharedImageBytes {
public:
    using Storage = std::vector<std::uint8_t>;

    SharedImageBytes() = default;
    explicit SharedImageBytes(Storage bytes)
        : storage_(bytes.empty()
                       ? nullptr
                       : std::make_shared<const Storage>(std::move(bytes))) {}
    explicit SharedImageBytes(std::shared_ptr<const Storage> bytes)
        : storage_(std::move(bytes)) {}

    [[nodiscard]] bool empty() const noexcept {
        return !storage_ || storage_->empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return storage_ ? storage_->size() : 0U;
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return storage_ ? storage_->data() : nullptr;
    }
    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
        return {data(), size()};
    }
    [[nodiscard]] bool sharesStorageWith(
        const SharedImageBytes& other) const noexcept {
        return storage_ && storage_ == other.storage_;
    }

    bool operator==(const SharedImageBytes& other) const {
        return values() == other.values();
    }
    auto operator<=>(const SharedImageBytes& other) const {
        return values() <=> other.values();
    }
    friend bool operator==(const SharedImageBytes& left,
                           const Storage& right) {
        return left.values() == right;
    }
    friend bool operator==(const Storage& left,
                           const SharedImageBytes& right) {
        return left == right.values();
    }

private:
    [[nodiscard]] const Storage& values() const {
        static const Storage empty_storage;
        return storage_ ? *storage_ : empty_storage;
    }

    std::shared_ptr<const Storage> storage_;
};

// Raster drawings are decoded by the UI toolkit, never by the OOXML parser.
// Keeping shared original package bytes here makes this transport neutral
// while allowing a bounded preview without fetching external relationships.
struct InlineImagePayload {
    std::string relationship_id;
    std::string package_member;
    std::string content_type;
    std::string name;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
    SharedImageBytes bytes;
    // DrawingML docPr@descr. `name` remains the required DrawingML object
    // identity/display name and is the fallback when descr is absent.
    std::string accessible_name;
    ImageLayout layout;

    [[nodiscard]] bool renderable() const noexcept {
        return width_emu > 0 && height_emu > 0 && !bytes.empty();
    }

    auto operator<=>(const InlineImagePayload&) const = default;
};

struct ImportedStoryImage {
    std::size_t text_offset_bytes{0};
    InlineImagePayload image;

    auto operator<=>(const ImportedStoryImage&) const = default;
};

enum class FragmentKind { text, tab, line_break, page_break, equation, inline_image };
using TextSpanId = std::uint64_t;

struct RunFragment {
    RunFragment() = default;
    RunFragment(FragmentKind new_kind, std::string new_text,
                std::optional<TextSpanId> new_text_span_id, bool new_editable,
                std::optional<EquationPayload> new_equation = std::nullopt,
                std::optional<InlineImagePayload> new_inline_image = std::nullopt)
        : kind(new_kind),
          text(std::move(new_text)),
          text_span_id(new_text_span_id),
          editable(new_editable),
          equation(std::move(new_equation)),
          inline_image(std::move(new_inline_image)) {}

    FragmentKind kind{FragmentKind::text};
    std::string text;
    std::optional<TextSpanId> text_span_id;
    bool editable{false};
    std::optional<EquationPayload> equation;
    std::optional<InlineImagePayload> inline_image;

    auto operator<=>(const RunFragment&) const = default;
};

struct Run {
    std::vector<RunFragment> fragments;
    // Advisory source style identity. `format` is the effective supported
    // formatting after the character/paragraph style cascade is resolved.
    std::optional<std::string> style_id;
    BasicRunFormat format;
    // Supported properties contributed by a character style or direct rPr,
    // excluding the paragraph-style baseline. Presence is significant even
    // when the value is false or equals the inherited value.
    BasicRunFormat paragraph_style_overrides;
    bool format_is_basic{true};
    bool has_unsupported_content{false};

    [[nodiscard]] std::string plainText() const;
};

struct ParagraphStyleProvenance {
    BasicRunFormat inherited_character_format;
    // Resolved paragraph-mark baseline before direct w:pPr/w:rPr is applied.
    // It includes docDefaults, paragraph-style rPr, and style-chain pPr/rPr.
    BasicRunFormat inherited_paragraph_mark_format;
    std::optional<BasicParagraphAlignment> inherited_alignment;
    std::optional<std::int32_t> inherited_left_indent_twips;
    std::optional<std::int32_t> inherited_right_indent_twips;
    std::optional<std::int32_t> inherited_first_line_indent_twips;
    std::optional<std::uint32_t> inherited_space_before_twips;
    std::optional<std::uint32_t> inherited_space_after_twips;
    std::optional<std::uint32_t> inherited_line_spacing;
    std::optional<BasicLineSpacingRule> inherited_line_spacing_rule;
    std::optional<bool> inherited_keep_with_next;
    std::optional<bool> inherited_keep_lines;
    std::optional<bool> inherited_page_break_before;

    // Direct pPr values are represented by presence, including explicit
    // false. Numbering/list properties are deliberately excluded because the
    // editor treats list identity and geometry independently from styles.
    std::optional<BasicParagraphAlignment> direct_alignment;
    std::optional<std::int32_t> direct_left_indent_twips;
    std::optional<std::int32_t> direct_right_indent_twips;
    std::optional<std::int32_t> direct_first_line_indent_twips;
    std::optional<std::uint32_t> direct_space_before_twips;
    std::optional<std::uint32_t> direct_space_after_twips;
    std::optional<std::uint32_t> direct_line_spacing;
    std::optional<BasicLineSpacingRule> direct_line_spacing_rule;
    std::optional<bool> direct_keep_with_next;
    std::optional<bool> direct_keep_lines;
    std::optional<bool> direct_page_break_before;
    BasicRunFormat direct_paragraph_mark_format;

    auto operator<=>(const ParagraphStyleProvenance&) const = default;
};

struct Paragraph {
    std::vector<Run> runs;
    // Advisory source style identity. The fields below contain the effective
    // supported values after docDefaults, basedOn, style, numbering-level,
    // and direct formatting have been cascaded in OOXML precedence order.
    std::optional<std::string> style_id;
    // Present only for an explicit source pStyle whose supported cascade was
    // resolved successfully. It lets the editor later change that style
    // without confusing source inheritance with direct formatting.
    std::optional<ParagraphStyleProvenance> style_provenance;
    std::optional<std::int32_t> numbering_id;
    std::optional<std::uint8_t> numbering_level;
    std::optional<ImportedNumbering> numbering;
    std::optional<BasicParagraphAlignment> alignment;
    std::optional<std::int32_t> left_indent_twips;
    std::optional<std::int32_t> right_indent_twips;
    // Negative values represent a hanging indent.
    std::optional<std::int32_t> first_line_indent_twips;
    std::optional<std::uint32_t> space_before_twips;
    std::optional<std::uint32_t> space_after_twips;
    std::optional<std::uint32_t> line_spacing;
    std::optional<BasicLineSpacingRule> line_spacing_rule;
    std::optional<bool> keep_with_next;
    std::optional<bool> keep_lines;
    std::optional<bool> page_break_before;
    // Explicit left tab stops in twentieths of a point. These are used by
    // literal-marker lists to keep continuation lines aligned with list text
    // in other DOCX editors without claiming native numbering semantics.
    std::vector<std::uint32_t> left_tab_stops_twips;
    bool format_is_basic{true};
    bool direct_body_child{false};
    bool has_unsupported_content{false};
    // Whole-paragraph replacement is intentionally limited to a paragraph
    // whose visible contents are one editable w:t span. This avoids guessing
    // how formatting should be distributed across multiple runs.
    bool whole_text_editable{false};
    // True when this paragraph ends in w:br w:type="page". The current
    // semantic editor maps that terminal break to page-break-before on the
    // following body paragraph while the original XML remains intact.
    bool hard_page_break_after{false};
    // Formatting on the paragraph mark (w:pPr/w:rPr). This is also the
    // insertion format of an empty table cell in WordprocessingML.
    std::optional<BasicRunFormat> paragraph_mark_format;

    [[nodiscard]] std::string plainText() const;
};

enum class BasicVerticalAlignment { top, center, bottom };

// A deliberately small set of table styles that can be represented by both
// the semantic editor and standard WordprocessingML built-in style IDs.  The
// original w:tblStyle value is retained separately on imported tables so a
// custom or newer style is never silently relabelled as one of these styles.
enum class BasicTableStyle {
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

struct ImportedTableBorder {
    std::uint32_t rgb{0};
    // OOXML w:sz is measured in eighths of a point.
    std::uint16_t width_eighth_points{0};

    auto operator<=>(const ImportedTableBorder&) const = default;
};

// Imported body blocks preserve the direct w:body order while retaining the
// existing flat paragraphs() view. A table cell refers to exactly one entry
// in paragraphs(); callers can therefore reuse the already parsed run and
// equation data without storing pointers into the document implementation.
struct ImportedParagraphBlock {
    std::size_t source_paragraph_index{0};

    auto operator<=>(const ImportedParagraphBlock&) const = default;
};

struct ImportedTableCell {
    std::size_t source_paragraph_index{0};
    std::optional<std::uint32_t> width_twips;
    std::optional<std::uint32_t> margin_top_twips;
    std::optional<std::uint32_t> margin_right_twips;
    std::optional<std::uint32_t> margin_bottom_twips;
    std::optional<std::uint32_t> margin_left_twips;
    std::optional<std::uint32_t> fill_rgb;
    std::optional<BasicVerticalAlignment> vertical_alignment;
    std::optional<ImportedTableBorder> border_top;
    std::optional<ImportedTableBorder> border_right;
    std::optional<ImportedTableBorder> border_bottom;
    std::optional<ImportedTableBorder> border_left;

    auto operator<=>(const ImportedTableCell&) const = default;
};

struct ImportedTableBlock {
    std::size_t rows{0};
    std::size_t columns{0};
    bool header_row{false};
    std::optional<BasicParagraphAlignment> alignment;
    std::vector<std::uint32_t> column_widths_twips;
    // Cells are ordered row-major and always contain rows * columns entries.
    std::vector<ImportedTableCell> cells;
    // Recognized built-in semantic style, if any. source_style_id retains the
    // exact w:tblStyle value even when the style is unknown to Owl Docs.
    std::optional<BasicTableStyle> style;
    std::optional<std::string> source_style_id;

    auto operator<=>(const ImportedTableBlock&) const = default;
};

// Complex body content remains available through its recursively collected
// paragraph fallbacks, but is deliberately not represented as an editable
// paragraph or table. The original OOXML subtree remains preserved verbatim.
struct ImportedUnsupportedBodyBlock {
    std::string element_name;
    std::string reason;
    std::vector<std::size_t> fallback_paragraph_indices;

    auto operator<=>(const ImportedUnsupportedBodyBlock&) const = default;
};

using ImportedBodyBlock = std::variant<ImportedParagraphBlock,
                                       ImportedTableBlock,
                                       ImportedUnsupportedBodyBlock>;

struct PackageMember {
    std::string name;
    std::uint64_t uncompressed_size{0};
    std::uint64_t compressed_size{0};
    std::uint32_t crc32{0};
    std::uint16_t compression_method{0};
    std::uint16_t encryption_method{0};

    auto operator<=>(const PackageMember&) const = default;
};

struct OpenOptions {
    std::uint64_t max_package_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t max_document_xml_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t max_member_uncompressed_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t max_total_uncompressed_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t max_member_count{10000};
    std::uint64_t max_xml_depth{256};
    std::uint64_t max_xml_nodes{1'000'000};
};

using RasterImageFormat = raster::Format;

// Authored pictures are byte-owned and transport neutral. The writer chooses
// all OPC member names and relationship IDs; callers cannot smuggle package
// paths or external relationships through this surface.
struct NewInlineImage {
    static constexpr std::size_t maximum_encoded_bytes =
        64ULL * 1024ULL * 1024ULL;

    RasterImageFormat format{RasterImageFormat::png};
    std::string name;
    std::int64_t width_emu{0};
    std::int64_t height_emu{0};
    std::vector<std::uint8_t> bytes;
    std::string accessible_name;
    ImageLayout layout;

    auto operator<=>(const NewInlineImage&) const = default;
};

// An image anchored to a U+FFFC placeholder in a UTF-8 header/footer story.
// The byte offset is used so the writer can stream the original UTF-8 text
// without transcoding while still preserving exact inline object order.
struct NewStoryImage {
    std::size_t text_offset_bytes{0};
    NewInlineImage image;

    auto operator<=>(const NewStoryImage&) const = default;
};

struct NewRun {
    NewRun() = default;
    NewRun(std::string new_text, BasicRunFormat new_format)
        : text(std::move(new_text)), format(std::move(new_format)) {}
    NewRun(std::string new_text, BasicRunFormat new_format,
           std::optional<EquationPayload> new_equation)
        : text(std::move(new_text)),
          format(std::move(new_format)),
          equation(std::move(new_equation)) {}
    NewRun(std::string new_text, BasicRunFormat new_format,
           std::optional<EquationPayload> new_equation,
           std::optional<NewInlineImage> new_inline_image)
        : text(std::move(new_text)),
          format(std::move(new_format)),
          equation(std::move(new_equation)),
          inline_image(std::move(new_inline_image)) {}

    std::string text;
    BasicRunFormat format;
    // When present this run represents exactly one equation and text must be
    // empty. Paragraph run order is therefore the inline content order.
    std::optional<EquationPayload> equation;
    // When present this run represents exactly one bounded internal raster
    // picture. It is mutually exclusive with both text and equation.
    std::optional<NewInlineImage> inline_image;

    auto operator<=>(const NewRun&) const = default;
};

// Native WordprocessingML numbering attached to an authored paragraph.
// Positive num_id values identify one list instance across paragraphs. The
// writer emits a matching numbering.xml definition and stores only numPr in
// document.xml; marker text must therefore not be duplicated in NewRun text.
// OOXML permits levels 0 through 8. The editor's tenth visual level is written
// as interoperable literal marker text and must not be represented here.
struct NewNumbering {
    std::int32_t num_id{0};
    std::uint8_t level{0};
    BasicNumberFormat format{BasicNumberFormat::decimal};
    std::int32_t start{1};
    std::string level_text{"%1."};
    BasicNumberSuffix suffix{BasicNumberSuffix::tab};
    std::uint32_t text_indent_twips{720};
    std::uint32_t hanging_indent_twips{360};
    std::optional<std::uint32_t> tab_stop_twips{720};

    auto operator<=>(const NewNumbering&) const = default;
};

struct NewParagraph {
    NewParagraph() = default;
    NewParagraph(std::vector<NewRun> new_runs,
                 std::optional<BasicParagraphAlignment> new_alignment = std::nullopt)
        : runs(std::move(new_runs)), alignment(new_alignment) {}

    std::vector<NewRun> runs;
    // WordprocessingML paragraph style ID (w:pPr/w:pStyle). New documents
    // accept only Owl Docs' deterministic built-in paragraph styles; imported
    // custom IDs remain visible on Paragraph but are never fabricated here.
    std::optional<std::string> style_id;
    std::optional<BasicParagraphAlignment> alignment;
    std::optional<std::int32_t> left_indent_twips;
    std::optional<std::int32_t> right_indent_twips;
    std::optional<std::int32_t> first_line_indent_twips;
    std::optional<std::uint32_t> space_before_twips;
    std::optional<std::uint32_t> space_after_twips;
    std::optional<std::uint32_t> line_spacing;
    std::optional<BasicLineSpacingRule> line_spacing_rule;
    std::optional<bool> keep_with_next;
    std::optional<bool> keep_lines;
    std::optional<bool> page_break_before;
    std::vector<std::uint32_t> left_tab_stops_twips;
    std::optional<NewNumbering> numbering;
    // Written as w:pPr/w:rPr. This is the durable insertion format of an
    // empty body paragraph or table cell; a zero-length w:r is not equivalent.
    std::optional<BasicRunFormat> paragraph_mark_format;

    auto operator<=>(const NewParagraph&) const = default;
};

struct NewTable {
    static constexpr std::size_t maximum_rows = 50;
    static constexpr std::size_t maximum_columns = 20;

    NewTable() = default;
    NewTable(std::size_t new_rows, std::size_t new_columns,
             bool new_header_row, std::vector<std::string> new_cells)
        : rows(new_rows),
          columns(new_columns),
          header_row(new_header_row),
          cells(std::move(new_cells)) {}

    std::size_t rows{0};
    std::size_t columns{0};
    bool header_row{false};
    // UTF-8 cell contents in row-major order. Tabs and line breaks are emitted
    // using the same safe WordprocessingML run vocabulary as body paragraphs.
    // This legacy/plain-text surface remains supported for aggregate callers.
    std::vector<std::string> cells;
    // When non-empty this is the authoritative row-major cell content and
    // formatting. It must contain rows * columns entries. `cells` may then be
    // empty (or may contain the same number of fallback strings). Equations in
    // table cells remain outside the editable subset and are rejected.
    std::vector<NewParagraph> cell_paragraphs;
    std::optional<BasicTableStyle> style{BasicTableStyle::grid};
    // Optional exact w:tblStyle ID from an imported document. A recognized
    // built-in ID may be emitted; an unknown ID is omitted from a generated
    // package because this API does not also generate its style definition.
    // Unchanged imported packages still preserve unknown IDs byte-for-byte.
    std::optional<std::string> source_style_id;

    auto operator<=>(const NewTable&) const = default;
};

using NewBodyBlock = std::variant<NewParagraph, NewTable>;

struct NewDocumentBody {
    NewDocumentBody() = default;
    NewDocumentBody(std::vector<NewBodyBlock> new_blocks)
        : blocks(std::move(new_blocks)) {}

    std::vector<NewBodyBlock> blocks;
    std::optional<std::string> header_text;
    std::optional<std::string> footer_text;
    std::vector<NewStoryImage> header_images;
    std::vector<NewStoryImage> footer_images;
};

struct PageSettings {
    std::uint32_t width_twips{12240};
    std::uint32_t height_twips{15840};
    std::uint32_t margin_top_twips{1440};
    std::uint32_t margin_right_twips{1440};
    std::uint32_t margin_bottom_twips{1440};
    std::uint32_t margin_left_twips{1440};

    auto operator<=>(const PageSettings&) const = default;
};

enum class SectionBreakKind { next_page, continuous, even_page, odd_page };

struct ImportedSection {
    // Half-open range into bodyBlocks(). The paragraph carrying a non-final
    // section property belongs to the section which it terminates.
    std::size_t first_body_block_index{0};
    std::size_t body_block_count{0};
    PageSettings page;
    SectionBreakKind break_kind{SectionBreakKind::next_page};

    auto operator<=>(const ImportedSection&) const = default;
};

struct NewSection {
    NewDocumentBody body;
    PageSettings page;
    // OOXML stores the section-start kind in this section's sectPr. For a
    // non-final section, its last body block must currently be a paragraph.
    SectionBreakKind break_kind{SectionBreakKind::next_page};

    auto operator<=>(const NewSection&) const = default;
};

struct NewSectionedDocumentBody {
    std::vector<NewSection> sections;
};

// Defaults written into the standard Word styles/settings parts. Callers can
// still put direct formatting on individual runs; these values control Normal
// style and newly entered unformatted content in other word processors.
struct DocumentDefaults {
    std::string font_family{"Carlito"};
    std::int32_t font_size_half_points{22};
    std::uint32_t default_tab_stop_twips{240};

    auto operator<=>(const DocumentDefaults&) const = default;
};

enum class ErrorCode {
    none,
    io_error,
    package_too_large,
    invalid_zip,
    package_limit_exceeded,
    duplicate_package_member,
    missing_opc_part,
    invalid_opc_metadata,
    unreadable_package_member,
    malformed_document_xml,
    invalid_word_document,
    xml_complexity_exceeded,
    unsafe_edit,
    save_validation_failed,
    atomic_commit_failed,
};

struct Error {
    ErrorCode code{ErrorCode::none};
    std::string message;

    auto operator<=>(const Error&) const = default;
};

struct EditResult {
    bool accepted{false};
    LossReport loss_report;
    std::optional<Error> error;

    [[nodiscard]] explicit operator bool() const noexcept { return accepted; }
};

struct SaveResult {
    bool saved{false};
    bool byte_identical_to_opened_file{false};
    std::size_t preserved_member_count{0};
    LossReport loss_report;
    std::optional<Error> error;

    [[nodiscard]] explicit operator bool() const noexcept { return saved; }
};

class DocxDocument {
public:
    // Returns nullptr on failure and fills error when it is non-null. The
    // opened package bytes are retained, making an unchanged Save As stable
    // even if the source path is later changed or removed.
    [[nodiscard]] static std::unique_ptr<DocxDocument> open(
        const std::filesystem::path& path,
        Error* error = nullptr,
        const OpenOptions& options = {});

    // Creates a conservative, macro-free DOCX with one body section. Run
    // formatting is limited to the fields in BasicRunFormat. Tabs and line
    // breaks in NewRun::text are emitted as w:tab/w:br; other unsupported
    // control characters and invalid style values are rejected.
    [[nodiscard]] static SaveResult writeNew(
        const std::filesystem::path& target,
        const std::vector<NewParagraph>& paragraphs,
        const PageSettings& page = {},
        const DocumentDefaults& defaults = {});

    // Block-aware overload used by semantic authoring. The wrapper type keeps
    // existing initializer-list calls to the paragraph-only API unambiguous.
    [[nodiscard]] static SaveResult writeNew(
        const std::filesystem::path& target,
        const NewDocumentBody& body,
        const PageSettings& page = {},
        const DocumentDefaults& defaults = {});

    // Ordered multi-section authoring. Non-final section properties are
    // written on the terminating paragraph and final properties on w:body,
    // matching native WordprocessingML structure.
    [[nodiscard]] static SaveResult writeNew(
        const std::filesystem::path& target,
        const NewSectionedDocumentBody& body,
        const DocumentDefaults& defaults = {});

    ~DocxDocument();
    DocxDocument(DocxDocument&&) noexcept;
    DocxDocument& operator=(DocxDocument&&) noexcept;
    DocxDocument(const DocxDocument&) = delete;
    DocxDocument& operator=(const DocxDocument&) = delete;

    [[nodiscard]] const std::filesystem::path& sourcePath() const noexcept;
    [[nodiscard]] const std::vector<PackageMember>& packageMembers() const noexcept;
    [[nodiscard]] const std::vector<Paragraph>& paragraphs() const noexcept;
    [[nodiscard]] const std::vector<ImportedBodyBlock>& bodyBlocks() const noexcept;
    [[nodiscard]] const std::optional<PageSettings>& bodyPageSettings() const noexcept;
    [[nodiscard]] const std::vector<ImportedSection>& sections() const noexcept;
    [[nodiscard]] const std::optional<std::string>& headerText() const noexcept;
    [[nodiscard]] const std::optional<std::string>& footerText() const noexcept;
    [[nodiscard]] const std::vector<ImportedStoryImage>& headerImages() const noexcept;
    [[nodiscard]] const std::vector<ImportedStoryImage>& footerImages() const noexcept;
    [[nodiscard]] const CompatibilityReport& compatibility() const noexcept;
    // True only for the deliberately narrow, text-only package shape emitted
    // by this writer when every package part and document structure is covered
    // by the current semantic regeneration path. This is stricter than
    // basic_body_text_patch: an extra/custom part, relationship, setting,
    // unsupported or extra style definition, section property, table,
    // drawing, equation, or numbering part makes the answer false so callers
    // never infer rewrite safety merely from a simple-looking body.
    [[nodiscard]] bool isCanonicalRegeneratableSimplePackage(
        const DocumentDefaults& regeneration_defaults) const noexcept;
    [[nodiscard]] bool dirty() const noexcept;

    // Replaces only the character data inside one mapped w:t element. Markup,
    // formatting, and every other package member remain untouched. Tabs,
    // newlines, malformed UTF-8/XML characters, field/revision contexts, and
    // leading/trailing whitespace without xml:space="preserve" are refused.
    [[nodiscard]] EditResult replaceText(TextSpanId id, std::string text);

    // Convenience operation with a deliberately strict formatting guard; see
    // Paragraph::whole_text_editable.
    [[nodiscard]] EditResult replaceParagraphText(std::size_t paragraph_index, std::string text);

    void discardEdits();

    // Writes to a unique temporary file in the target directory, fsyncs it,
    // reopens and validates the DOCX, then atomically renames it over target.
    // On success the saved file becomes this object's new clean baseline.
    [[nodiscard]] SaveResult saveAs(const std::filesystem::path& target);

private:
    struct Impl;
    explicit DocxDocument(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace docxstudio::ooxml
