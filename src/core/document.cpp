#include "docxstudio/core/document.h"

#include "docxstudio/raster/validation.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace docxstudio::core {
namespace {

constexpr bool isHighSurrogate(char16_t value) noexcept {
    return value >= 0xd800 && value <= 0xdbff;
}

constexpr bool isLowSurrogate(char16_t value) noexcept {
    return value >= 0xdc00 && value <= 0xdfff;
}

bool containsParagraphBreak(const std::u16string& text) noexcept {
    return std::any_of(text.begin(), text.end(), [](char16_t character) {
        return character == u'\r' || character == u'\n' || character == 0x2029;
    });
}

bool containsInlineObjectPlaceholder(const std::u16string& text) noexcept {
    return std::find(text.begin(), text.end(),
                     kInlineObjectReplacementCharacter) != text.end();
}

bool isUtf8Continuation(unsigned char value) noexcept {
    return value >= 0x80U && value <= 0xbfU;
}

bool isValidUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            if (first == 0U) {
                return false;
            }
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if (!isUtf8Continuation(
                    static_cast<unsigned char>(text[index + offset]))) {
                return false;
            }
        }

        const auto second = static_cast<unsigned char>(text[index + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) {
            return false;
        }
        index += length;
    }
    return true;
}

Result<void> validateEquationSource(std::string_view source) {
    constexpr std::size_t maximum_latex_bytes = 64U * 1024U;
    if (source.empty()) {
        return Error{ErrorCode::invalid_operation,
                     "Equation canonical LaTeX cannot be empty"};
    }
    if (source.size() > maximum_latex_bytes) {
        return Error{ErrorCode::invalid_operation,
                     "Equation canonical LaTeX exceeds the size limit"};
    }
    if (!isValidUtf8(source)) {
        return Error{ErrorCode::invalid_operation,
                     "Equation canonical LaTeX is not valid UTF-8"};
    }
    return {};
}

Result<void> validateImageDimensions(std::int64_t width_emu,
                                     std::int64_t height_emu) {
    if (width_emu <= 0 || height_emu <= 0) {
        return Error{ErrorCode::invalid_operation,
                     "Image dimensions must be positive"};
    }
    if (width_emu > kMaximumInlineImageDimensionEmu ||
        height_emu > kMaximumInlineImageDimensionEmu) {
        return Error{ErrorCode::invalid_operation,
                     "Image dimensions exceed the geometry limit"};
    }
    return {};
}

Result<void> validateImageMetadata(const ImageAtom& image) {
    if (!image.id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Image NodeId cannot be zero"};
    }
    if (image.encoded_payload.empty()) {
        return Error{ErrorCode::invalid_operation,
                     "Encoded image payload cannot be empty"};
    }
    if (image.encoded_payload.size() > kMaximumEncodedImageBytes) {
        return Error{ErrorCode::invalid_operation,
                     "Encoded image payload exceeds the size limit"};
    }
    if (imageContentType(image.format).empty()) {
        return Error{ErrorCode::invalid_operation,
                     "Encoded image format is unsupported"};
    }
    if (image.accessible_name.size() > kMaximumImageAccessibleNameBytes) {
        return Error{ErrorCode::invalid_operation,
                     "Image accessible name exceeds the size limit"};
    }
    if (!isValidUtf8(image.accessible_name)) {
        return Error{ErrorCode::invalid_operation,
                     "Image accessible name is not valid UTF-8"};
    }
    const auto dimensions_validation =
        validateImageDimensions(image.width_emu, image.height_emu);
    if (!dimensions_validation) {
        return dimensions_validation.error();
    }
    const auto layout_validation = image.layout.validate();
    if (!layout_validation) {
        return layout_validation.error();
    }
    return {};
}

Result<void> validateImagePayload(const ImageAtom& image) {
    const auto expected_format = image.format == ImageFormat::png
        ? raster::Format::png
        : raster::Format::jpeg;
    raster::ValidationLimits raster_limits;
    raster_limits.maximum_encoded_bytes = kMaximumEncodedImageBytes;
    if (!raster::inspect(image.encoded_payload.bytes(), expected_format,
                         raster_limits)
             .ok()) {
        return Error{ErrorCode::invalid_operation,
                     "Encoded image payload is malformed or does not match its format"};
    }
    return {};
}

Error paragraphMissing(NodeId id) {
    return Error{ErrorCode::paragraph_not_found, "Paragraph not found: " + id.toString()};
}

Error tableMissing(NodeId id) {
    return Error{ErrorCode::invalid_operation, "Table not found: " + id.toString()};
}

Error bodyBlockMissing(NodeId id) {
    return Error{ErrorCode::invalid_operation, "Body block not found: " + id.toString()};
}

}  // namespace

Result<void> ImageLayout::validate() const {
    switch (placement) {
        case ImagePlacement::inline_with_text:
        case ImagePlacement::square:
        case ImagePlacement::top_and_bottom:
            break;
        default:
            return Error{ErrorCode::invalid_operation,
                         "Image placement is unsupported"};
    }

    const auto valid_distance = [](std::int64_t distance) {
        return distance >= 0 && distance <= kMaximumImageWrapDistanceEmu;
    };
    if (!valid_distance(distance_top_emu) ||
        !valid_distance(distance_right_emu) ||
        !valid_distance(distance_bottom_emu) ||
        !valid_distance(distance_left_emu)) {
        return Error{ErrorCode::invalid_operation,
                     "Image wrap distances must be non-negative and within the geometry limit"};
    }
    if (placement == ImagePlacement::inline_with_text && !move_with_text) {
        return Error{ErrorCode::invalid_operation,
                     "Inline images must move with text"};
    }
    return {};
}

EncodedImagePayload::EncodedImagePayload()
    : storage_(std::make_shared<const std::vector<std::uint8_t>>()) {}

EncodedImagePayload::EncodedImagePayload(std::vector<std::uint8_t> bytes)
    : storage_(std::make_shared<const std::vector<std::uint8_t>>(
          std::move(bytes))) {}

std::span<const std::uint8_t> EncodedImagePayload::bytes() const noexcept {
    return storage_ ? std::span<const std::uint8_t>(*storage_)
                    : std::span<const std::uint8_t>{};
}

std::size_t EncodedImagePayload::size() const noexcept {
    return storage_ ? storage_->size() : 0;
}

bool EncodedImagePayload::empty() const noexcept {
    return size() == 0;
}

bool EncodedImagePayload::operator==(
    const EncodedImagePayload& other) const noexcept {
    if (storage_ == other.storage_) {
        return true;
    }
    const auto left = bytes();
    const auto right = other.bytes();
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

std::strong_ordering EncodedImagePayload::operator<=>(
    const EncodedImagePayload& other) const noexcept {
    if (storage_ == other.storage_) {
        return std::strong_ordering::equal;
    }
    const auto left = bytes();
    const auto right = other.bytes();
    return std::lexicographical_compare_three_way(
        left.begin(), left.end(), right.begin(), right.end(),
        std::compare_three_way{});
}

bool isValidUtf16(const std::u16string& text) noexcept {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (isHighSurrogate(text[index])) {
            if (index + 1 >= text.size() || !isLowSurrogate(text[index + 1])) {
                return false;
            }
            ++index;
        } else if (isLowSurrogate(text[index])) {
            return false;
        }
    }
    return true;
}

bool isUtf16Boundary(const std::u16string& text, std::size_t offset) noexcept {
    if (offset > text.size()) {
        return false;
    }
    return offset == 0 || offset == text.size() ||
           !(isHighSurrogate(text[offset - 1]) && isLowSurrogate(text[offset]));
}

namespace {

std::vector<CharacterFormat> denseCellFormats(const TableCell& cell) {
    std::vector<CharacterFormat> formats(
        cell.text.size(), cell.default_character_format);
    for (const auto& run : cell.character_formats) {
        const auto end = std::min(run.end, formats.size());
        for (auto index = std::min(run.start, end); index < end; ++index) {
            formats[index] = run.format;
        }
    }
    return formats;
}

void setCellContent(TableCell& cell, std::u16string text,
                    std::vector<CharacterFormat> formats) {
    cell.text = std::move(text);
    cell.character_formats.clear();
    if (formats.size() != cell.text.size()) {
        formats.resize(cell.text.size());
    }
    std::size_t index = 0;
    while (index < formats.size()) {
        if (formats[index] == cell.default_character_format) {
            ++index;
            continue;
        }
        const auto start = index;
        const auto format = formats[index];
        while (++index < formats.size() && formats[index] == format) {
        }
        cell.character_formats.push_back(FormatRun{start, index, format});
    }
}

Result<void> validateTableCell(const TableCell& cell) {
    if (!cell.id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Table cell NodeId cannot be zero"};
    }
    if (!isValidUtf16(cell.text) || containsParagraphBreak(cell.text) ||
        containsInlineObjectPlaceholder(cell.text)) {
        return Error{ErrorCode::invalid_operation,
                     "Table cell text is invalid"};
    }
    const auto paragraph_validation = cell.paragraph_format.validate();
    if (!paragraph_validation) return paragraph_validation.error();
    const auto default_format_validation =
        cell.default_character_format.validate();
    if (!default_format_validation) return default_format_validation.error();
    std::size_t previous_end = 0;
    for (const auto& run : cell.character_formats) {
        if (run.start >= run.end || run.end > cell.text.size() ||
            run.start < previous_end || !isUtf16Boundary(cell.text, run.start) ||
            !isUtf16Boundary(cell.text, run.end)) {
            return Error{ErrorCode::invalid_formatting,
                         "Table cell character format run is invalid"};
        }
        const auto validation = run.format.validate();
        if (!validation) return validation.error();
        previous_end = run.end;
    }
    return {};
}

CharacterFormat inheritedCellInsertionFormat(const TableCell& source) {
    if (!source.default_character_format.empty()) {
        return source.default_character_format;
    }
    return source.characterFormatAt(source.text.size());
}

TableCell inheritedEmptyCell(NodeId id, const TableCell& source) {
    return TableCell{id, {}, {}, source.paragraph_format,
                     inheritedCellInsertionFormat(source)};
}

}  // namespace

CharacterFormat TableCell::characterFormatAt(
    std::size_t utf16_offset) const {
    if (text.empty()) return default_character_format;
    const auto index = utf16_offset == 0
        ? 0
        : std::min(utf16_offset - 1, text.size() - 1);
    for (const auto& run : character_formats) {
        if (index >= run.start && index < run.end) return run.format;
    }
    return default_character_format;
}

Table::Table(NodeId id, std::size_t rows, std::size_t columns, bool header_row,
             std::vector<TableCell> cells,
             std::optional<TableStyle> style)
    : id_(id), rows_(rows), columns_(columns), header_row_(header_row),
      style_(style), cells_(std::move(cells)) {}

Result<Table> Table::create(std::size_t rows, std::size_t columns,
                            bool header_row, NodeId id,
                            std::optional<TableStyle> style) {
    if (!id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "Table NodeId cannot be zero"};
    }
    if (rows == 0 || columns == 0 || rows > maximum_rows ||
        columns > maximum_columns) {
        return Error{
            ErrorCode::invalid_operation,
            "Table dimensions must be between 1x1 and " +
                std::to_string(maximum_rows) + "x" +
                std::to_string(maximum_columns)};
    }

    std::vector<TableCell> cells;
    cells.reserve(rows * columns);
    std::unordered_set<NodeId, NodeIdHash> identifiers;
    identifiers.insert(id);
    for (std::size_t index = 0; index < rows * columns; ++index) {
        auto cell_id = NodeId::generate();
        while (!identifiers.insert(cell_id).second) {
            cell_id = NodeId::generate();
        }
        cells.push_back(TableCell{cell_id, {}});
    }
    return Table(id, rows, columns, header_row, std::move(cells), style);
}

Result<Table> Table::restore(std::size_t rows, std::size_t columns,
                             bool header_row, NodeId id,
                             std::vector<TableCell> cells,
                             std::optional<TableStyle> style) {
    if (!id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "Table NodeId cannot be zero"};
    }
    if (rows == 0 || columns == 0 || rows > maximum_rows ||
        columns > maximum_columns ||
        rows > std::numeric_limits<std::size_t>::max() / columns ||
        cells.size() != rows * columns) {
        return Error{
            ErrorCode::invalid_operation,
            "Restored table must be a rectangular grid between 1x1 and " +
                std::to_string(maximum_rows) + "x" +
                std::to_string(maximum_columns)};
    }
    std::unordered_set<NodeId, NodeIdHash> identifiers;
    identifiers.insert(id);
    for (const auto& cell : cells) {
        const auto validation = validateTableCell(cell);
        if (!validation) return validation.error();
        if (!identifiers.insert(cell.id).second) {
            return Error{ErrorCode::duplicate_node_id,
                         "Table and cell NodeIds must be unique"};
        }
    }
    return Table(id, rows, columns, header_row, std::move(cells), style);
}

const TableCell* Table::cell(std::size_t row, std::size_t column) const noexcept {
    if (row >= rows_ || column >= columns_) {
        return nullptr;
    }
    return &cells_[row * columns_ + column];
}

Result<void> Table::setCellText(
    std::size_t row, std::size_t column, std::u16string text,
    const std::optional<CharacterFormat>& inserted_format) {
    if (row >= rows_ || column >= columns_) {
        return Error{ErrorCode::invalid_range, "Table cell coordinates are out of range"};
    }
    if (!isValidUtf16(text)) {
        return Error{ErrorCode::invalid_utf16, "Table cell contains malformed UTF-16"};
    }
    if (containsParagraphBreak(text)) {
        return Error{ErrorCode::invalid_operation,
                     "Table cell text cannot contain a paragraph separator"};
    }
    if (containsInlineObjectPlaceholder(text)) {
        return Error{ErrorCode::invalid_operation,
                     "Table cell text cannot contain an orphan inline-object placeholder"};
    }
    if (inserted_format) {
        const auto validation = inserted_format->validate();
        if (!validation) return validation.error();
    }

    auto& cell = cells_[row * columns_ + column];
    if (cell.text == text) return {};
    auto old_formats = denseCellFormats(cell);
    std::size_t prefix = 0;
    while (prefix < cell.text.size() && prefix < text.size() &&
           cell.text[prefix] == text[prefix]) {
        ++prefix;
    }
    while (prefix > 0 && !isUtf16Boundary(cell.text, prefix)) --prefix;

    std::size_t suffix = 0;
    while (suffix < cell.text.size() - prefix &&
           suffix < text.size() - prefix &&
           cell.text[cell.text.size() - suffix - 1] ==
               text[text.size() - suffix - 1]) {
        ++suffix;
    }
    const auto old_suffix_start = cell.text.size() - suffix;
    const auto new_suffix_start = text.size() - suffix;
    CharacterFormat replacement_format = cell.default_character_format;
    if (inserted_format) {
        replacement_format = *inserted_format;
    } else if (prefix > 0 && prefix - 1 < old_formats.size()) {
        replacement_format = old_formats[prefix - 1];
    } else if (old_suffix_start < old_formats.size()) {
        replacement_format = old_formats[old_suffix_start];
    } else if (text.empty() && !old_formats.empty()) {
        // A whole-cell deletion leaves only the cell's paragraph mark. Carry
        // the first deleted character's format into that durable insertion
        // point when the caller did not supply an explicit active format.
        replacement_format = old_formats.front();
    }

    std::vector<CharacterFormat> updated_formats;
    updated_formats.reserve(text.size());
    updated_formats.insert(updated_formats.end(), old_formats.begin(),
                           old_formats.begin() +
                               static_cast<std::ptrdiff_t>(prefix));
    updated_formats.insert(updated_formats.end(),
                           new_suffix_start - prefix, replacement_format);
    updated_formats.insert(
        updated_formats.end(),
        old_formats.begin() + static_cast<std::ptrdiff_t>(old_suffix_start),
        old_formats.end());
    if (text.empty() && !cell.text.empty()) {
        cell.default_character_format = replacement_format;
    }
    setCellContent(cell, std::move(text), std::move(updated_formats));
    return {};
}

Result<void> Table::applyCellCharacterFormat(
    std::size_t row, std::size_t column, std::size_t start,
    std::size_t end, const CharacterFormatDelta& delta) {
    if (row >= rows_ || column >= columns_) {
        return Error{ErrorCode::invalid_range,
                     "Table cell coordinates are out of range"};
    }
    auto& cell = cells_[row * columns_ + column];
    if (start > end || !isUtf16Boundary(cell.text, start) ||
        !isUtf16Boundary(cell.text, end)) {
        return Error{ErrorCode::invalid_range,
                     "Table cell formatting range is invalid"};
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) return delta_validation.error();
    if (delta.empty()) return {};
    if (start == end) {
        if (!cell.text.empty()) return {};
        auto candidate = cell.default_character_format;
        delta.applyTo(candidate);
        const auto validation = candidate.validate();
        if (!validation) return validation.error();
        cell.default_character_format = std::move(candidate);
        return {};
    }

    auto formats = denseCellFormats(cell);
    for (auto index = start; index < end; ++index) {
        delta.applyTo(formats[index]);
        const auto validation = formats[index].validate();
        if (!validation) return validation.error();
    }
    if (start == 0 && end == cell.text.size()) {
        auto candidate = cell.default_character_format;
        delta.applyTo(candidate);
        const auto validation = candidate.validate();
        if (!validation) return validation.error();
        cell.default_character_format = std::move(candidate);
    }
    setCellContent(cell, cell.text, std::move(formats));
    return {};
}

Result<void> Table::applyCellParagraphFormat(
    std::size_t row, std::size_t column,
    const ParagraphFormatDelta& delta) {
    if (row >= rows_ || column >= columns_) {
        return Error{ErrorCode::invalid_range,
                     "Table cell coordinates are out of range"};
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) return delta_validation.error();
    if (delta.empty()) return {};
    auto candidate = cells_[row * columns_ + column].paragraph_format;
    delta.applyTo(candidate);
    const auto validation = candidate.validate();
    if (!validation) return validation.error();
    cells_[row * columns_ + column].paragraph_format = std::move(candidate);
    return {};
}

Result<void> Table::appendRow(std::vector<NodeId> cell_ids) {
    return insertRow(rows_, std::move(cell_ids),
                     TableInsertionSource::preceding);
}

Result<void> Table::insertRow(std::size_t index,
                              std::vector<NodeId> cell_ids,
                              TableInsertionSource source) {
    if (rows_ >= maximum_rows) {
        return Error{ErrorCode::invalid_operation,
                     "Table has reached the maximum row count"};
    }
    if (cell_ids.size() != columns_) {
        return Error{ErrorCode::invalid_operation,
                     "An appended table row must contain one ID per column"};
    }
    if (index > rows_) {
        return Error{ErrorCode::invalid_range,
                     "Table row insertion index is out of range"};
    }
    const std::size_t source_row =
        source == TableInsertionSource::following && index < rows_
            ? index
            : (index == 0 ? 0 : index - 1);
    std::vector<TableCell> inserted;
    inserted.reserve(columns_);
    for (std::size_t column = 0; column < columns_; ++column) {
        const auto id = cell_ids[column];
        if (!id.isValid()) {
            return Error{ErrorCode::invalid_node_id,
                         "Table cell NodeId cannot be zero"};
        }
        inserted.push_back(inheritedEmptyCell(
            id, cells_[source_row * columns_ + column]));
    }
    cells_.insert(
        cells_.begin() + static_cast<std::ptrdiff_t>(index * columns_),
        std::make_move_iterator(inserted.begin()),
        std::make_move_iterator(inserted.end()));
    ++rows_;
    return {};
}

Result<void> Table::deleteRows(std::size_t index, std::size_t count) {
    if (count == 0) return {};
    if (index >= rows_ || count > rows_ - index) {
        return Error{ErrorCode::invalid_range,
                     "Table row deletion range is out of bounds"};
    }
    if (count >= rows_) {
        return Error{ErrorCode::invalid_operation,
                     "A table must retain at least one row"};
    }
    const auto first = cells_.begin() +
        static_cast<std::ptrdiff_t>(index * columns_);
    const auto last = first + static_cast<std::ptrdiff_t>(count * columns_);
    cells_.erase(first, last);
    rows_ -= count;
    return {};
}

Result<void> Table::insertColumn(std::size_t index,
                                 std::vector<NodeId> cell_ids,
                                 TableInsertionSource source) {
    if (columns_ >= maximum_columns) {
        return Error{ErrorCode::invalid_operation,
                     "Table has reached the maximum column count"};
    }
    if (index > columns_) {
        return Error{ErrorCode::invalid_range,
                     "Table column insertion index is out of range"};
    }
    if (cell_ids.size() != rows_) {
        return Error{ErrorCode::invalid_operation,
                     "An inserted table column must contain one ID per row"};
    }
    const std::size_t source_column =
        source == TableInsertionSource::following && index < columns_
            ? index
            : (index == 0 ? 0 : index - 1);
    std::vector<TableCell> updated;
    updated.reserve(rows_ * (columns_ + 1));
    for (std::size_t row = 0; row < rows_; ++row) {
        const auto old_start = cells_.begin() +
            static_cast<std::ptrdiff_t>(row * columns_);
        updated.insert(updated.end(), old_start,
                       old_start + static_cast<std::ptrdiff_t>(index));
        if (!cell_ids[row].isValid()) {
            return Error{ErrorCode::invalid_node_id,
                         "Table cell NodeId cannot be zero"};
        }
        updated.push_back(inheritedEmptyCell(
            cell_ids[row], cells_[row * columns_ + source_column]));
        updated.insert(updated.end(),
                       old_start + static_cast<std::ptrdiff_t>(index),
                       old_start + static_cast<std::ptrdiff_t>(columns_));
    }
    cells_ = std::move(updated);
    ++columns_;
    return {};
}

Result<void> Table::deleteColumns(std::size_t index, std::size_t count) {
    if (count == 0) return {};
    if (index >= columns_ || count > columns_ - index) {
        return Error{ErrorCode::invalid_range,
                     "Table column deletion range is out of bounds"};
    }
    if (count >= columns_) {
        return Error{ErrorCode::invalid_operation,
                     "A table must retain at least one column"};
    }
    std::vector<TableCell> updated;
    updated.reserve(rows_ * (columns_ - count));
    for (std::size_t row = 0; row < rows_; ++row) {
        const auto old_start = cells_.begin() +
            static_cast<std::ptrdiff_t>(row * columns_);
        updated.insert(updated.end(), old_start,
                       old_start + static_cast<std::ptrdiff_t>(index));
        updated.insert(
            updated.end(),
            old_start + static_cast<std::ptrdiff_t>(index + count),
            old_start + static_cast<std::ptrdiff_t>(columns_));
    }
    cells_ = std::move(updated);
    columns_ -= count;
    return {};
}

Paragraph::Paragraph() : id_(NodeId::generate()) {}

Paragraph::Paragraph(NodeId id, std::u16string text,
                     CharacterFormat paragraph_mark_character_format)
    : id_(id), text_(std::move(text)),
      paragraph_mark_character_format_(
          std::move(paragraph_mark_character_format)) {}

Result<Paragraph> Paragraph::create(
    std::u16string text, NodeId id,
    CharacterFormat paragraph_mark_character_format) {
    if (!id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "Paragraph NodeId cannot be zero"};
    }
    if (!isValidUtf16(text)) {
        return Error{ErrorCode::invalid_utf16, "Paragraph contains malformed UTF-16"};
    }
    if (containsParagraphBreak(text)) {
        return Error{ErrorCode::invalid_operation,
                     "Paragraph text cannot contain a paragraph separator"};
    }
    if (containsInlineObjectPlaceholder(text)) {
        return Error{ErrorCode::invalid_operation,
                     "Paragraph text cannot contain an orphan inline-object placeholder"};
    }
    const auto mark_format_validation =
        paragraph_mark_character_format.validate();
    if (!mark_format_validation) {
        return mark_format_validation.error();
    }
    return Paragraph(id, std::move(text),
                     std::move(paragraph_mark_character_format));
}

std::vector<CharacterFormat> Paragraph::denseFormats() const {
    std::vector<CharacterFormat> formats(text_.size());
    for (const auto& run : character_formats_) {
        const auto end = std::min(run.end, formats.size());
        for (auto index = std::min(run.start, end); index < end; ++index) {
            formats[index] = run.format;
        }
    }
    return formats;
}

void Paragraph::setContent(std::u16string text, std::vector<CharacterFormat> formats) {
    text_ = std::move(text);
    character_formats_.clear();
    if (formats.size() != text_.size()) {
        formats.resize(text_.size());
    }

    std::size_t index = 0;
    while (index < formats.size()) {
        if (formats[index].empty()) {
            ++index;
            continue;
        }
        const auto start = index;
        const auto format = formats[index];
        while (++index < formats.size() && formats[index] == format) {
        }
        character_formats_.push_back(FormatRun{start, index, format});
    }
}

CharacterFormat Paragraph::characterFormatAt(std::size_t utf16_offset) const {
    if (text_.empty()) {
        return paragraph_mark_character_format_;
    }
    const auto index = utf16_offset == 0 ? 0 : std::min(utf16_offset - 1, text_.size() - 1);
    for (const auto& run : character_formats_) {
        if (index >= run.start && index < run.end) {
            return run.format;
        }
    }
    return {};
}

const EquationAtom* Paragraph::equationAt(std::size_t utf16_offset) const noexcept {
    const auto found = std::lower_bound(
        equations_.begin(), equations_.end(), utf16_offset,
        [](const EquationAtom& equation, std::size_t offset) {
            return equation.utf16_offset < offset;
        });
    return found != equations_.end() && found->utf16_offset == utf16_offset
        ? &*found
        : nullptr;
}

const ImageAtom* Paragraph::imageAt(std::size_t utf16_offset) const noexcept {
    const auto found = std::lower_bound(
        images_.begin(), images_.end(), utf16_offset,
        [](const ImageAtom& image, std::size_t offset) {
            return image.utf16_offset < offset;
        });
    return found != images_.end() && found->utf16_offset == utf16_offset
        ? &*found
        : nullptr;
}

Result<void> Paragraph::insertText(std::size_t offset, const std::u16string& text,
                                   const std::optional<CharacterFormat>& format) {
    if (!isUtf16Boundary(text_, offset)) {
        return Error{ErrorCode::invalid_position, "Insertion offset is not a UTF-16 boundary"};
    }
    if (!isValidUtf16(text)) {
        return Error{ErrorCode::invalid_utf16, "Inserted text contains malformed UTF-16"};
    }
    if (containsParagraphBreak(text)) {
        return Error{ErrorCode::invalid_operation,
                     "InsertText cannot contain a paragraph separator; use SplitParagraph"};
    }
    if (containsInlineObjectPlaceholder(text)) {
        return Error{ErrorCode::invalid_operation,
                     "InsertText cannot create an orphan inline-object placeholder"};
    }
    if (format) {
        const auto validation = format->validate();
        if (!validation) {
            return validation.error();
        }
    }
    if (text.empty()) {
        return {};
    }

    auto formats = denseFormats();
    const auto insertion_format = format.value_or(characterFormatAt(offset));
    formats.insert(formats.begin() + static_cast<std::ptrdiff_t>(offset), text.size(), insertion_format);
    auto updated_text = text_;
    updated_text.insert(offset, text);
    auto updated_equations = equations_;
    for (auto& equation : updated_equations) {
        if (equation.utf16_offset >= offset) {
            equation.utf16_offset += text.size();
        }
    }
    auto updated_images = images_;
    for (auto& image : updated_images) {
        if (image.utf16_offset >= offset) {
            image.utf16_offset += text.size();
        }
    }
    setContent(std::move(updated_text), std::move(formats));
    equations_ = std::move(updated_equations);
    images_ = std::move(updated_images);
    return {};
}

Result<void> Paragraph::insertEquation(
    std::size_t offset, EquationAtom equation,
    const std::optional<CharacterFormat>& format) {
    if (!isUtf16Boundary(text_, offset)) {
        return Error{ErrorCode::invalid_position,
                     "Equation insertion offset is not a UTF-16 boundary"};
    }
    if (!equation.id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "Equation NodeId cannot be zero"};
    }
    if (std::any_of(equations_.begin(), equations_.end(),
                    [&equation](const EquationAtom& existing) {
                        return existing.id == equation.id;
                    })) {
        return Error{ErrorCode::duplicate_node_id, "Equation NodeId already exists"};
    }
    const auto source_validation = validateEquationSource(equation.canonical_latex);
    if (!source_validation) {
        return source_validation.error();
    }
    if (format) {
        const auto format_validation = format->validate();
        if (!format_validation) {
            return format_validation.error();
        }
    }

    auto formats = denseFormats();
    const auto insertion_format = format.value_or(characterFormatAt(offset));
    formats.insert(formats.begin() + static_cast<std::ptrdiff_t>(offset),
                   insertion_format);
    auto updated_text = text_;
    updated_text.insert(offset, 1, kInlineObjectReplacementCharacter);

    auto updated_equations = equations_;
    for (auto& existing : updated_equations) {
        if (existing.utf16_offset >= offset) {
            ++existing.utf16_offset;
        }
    }
    equation.utf16_offset = offset;
    const auto insertion = std::lower_bound(
        updated_equations.begin(), updated_equations.end(), offset,
        [](const EquationAtom& existing, std::size_t candidate_offset) {
            return existing.utf16_offset < candidate_offset;
        });
    updated_equations.insert(insertion, std::move(equation));

    auto updated_images = images_;
    for (auto& image : updated_images) {
        if (image.utf16_offset >= offset) {
            ++image.utf16_offset;
        }
    }

    setContent(std::move(updated_text), std::move(formats));
    equations_ = std::move(updated_equations);
    images_ = std::move(updated_images);
    return {};
}

Result<void> Paragraph::insertImage(
    std::size_t offset, ImageAtom image,
    const std::optional<CharacterFormat>& character_format) {
    if (!isUtf16Boundary(text_, offset)) {
        return Error{ErrorCode::invalid_position,
                     "Image insertion offset is not a UTF-16 boundary"};
    }
    // Document::insertImage validates the complete atom, global identity and
    // resource limits before this private mutation helper is reached.

    auto formats = denseFormats();
    const auto insertion_format =
        character_format.value_or(characterFormatAt(offset));
    formats.insert(formats.begin() + static_cast<std::ptrdiff_t>(offset),
                   insertion_format);
    auto updated_text = text_;
    updated_text.insert(offset, 1, kInlineObjectReplacementCharacter);

    auto updated_equations = equations_;
    for (auto& equation : updated_equations) {
        if (equation.utf16_offset >= offset) {
            ++equation.utf16_offset;
        }
    }
    auto updated_images = images_;
    for (auto& existing : updated_images) {
        if (existing.utf16_offset >= offset) {
            ++existing.utf16_offset;
        }
    }
    image.utf16_offset = offset;
    const auto insertion = std::lower_bound(
        updated_images.begin(), updated_images.end(), offset,
        [](const ImageAtom& existing, std::size_t candidate_offset) {
            return existing.utf16_offset < candidate_offset;
        });
    updated_images.insert(insertion, std::move(image));

    setContent(std::move(updated_text), std::move(formats));
    equations_ = std::move(updated_equations);
    images_ = std::move(updated_images);
    return {};
}

Result<void> Paragraph::erase(std::size_t start, std::size_t end) {
    if (start > end || !isUtf16Boundary(text_, start) || !isUtf16Boundary(text_, end)) {
        return Error{ErrorCode::invalid_range, "Deletion range is invalid"};
    }
    if (start == end) {
        return {};
    }
    auto formats = denseFormats();
    std::optional<CharacterFormat> emptied_format;
    if (start == 0 && end == text_.size() && !formats.empty()) {
        // Once all text is gone, its first character is the most useful
        // durable insertion context. Without this promotion, navigating away
        // from the newly empty paragraph would expose a stale paragraph mark.
        emptied_format = formats.front();
    }
    formats.erase(formats.begin() + static_cast<std::ptrdiff_t>(start),
                  formats.begin() + static_cast<std::ptrdiff_t>(end));
    auto updated_text = text_;
    updated_text.erase(start, end - start);
    auto updated_equations = equations_;
    std::erase_if(updated_equations, [start, end](const EquationAtom& equation) {
        return equation.utf16_offset >= start && equation.utf16_offset < end;
    });
    for (auto& equation : updated_equations) {
        if (equation.utf16_offset >= end) {
            equation.utf16_offset -= end - start;
        }
    }
    auto updated_images = images_;
    std::erase_if(updated_images, [start, end](const ImageAtom& image) {
        return image.utf16_offset >= start && image.utf16_offset < end;
    });
    for (auto& image : updated_images) {
        if (image.utf16_offset >= end) {
            image.utf16_offset -= end - start;
        }
    }
    setContent(std::move(updated_text), std::move(formats));
    if (text_.empty() && emptied_format) {
        paragraph_mark_character_format_ = std::move(*emptied_format);
    }
    equations_ = std::move(updated_equations);
    images_ = std::move(updated_images);
    return {};
}

Result<void> Paragraph::applyFormat(std::size_t start, std::size_t end,
                                    const CharacterFormatDelta& delta) {
    if (start > end || !isUtf16Boundary(text_, start) || !isUtf16Boundary(text_, end)) {
        return Error{ErrorCode::invalid_range, "Formatting range is invalid"};
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) {
        return delta_validation.error();
    }
    if (delta.empty()) {
        return {};
    }
    if (start == end) {
        if (!text_.empty()) {
            return {};
        }
        auto candidate = paragraph_mark_character_format_;
        delta.applyTo(candidate);
        const auto validation = candidate.validate();
        if (!validation) {
            return validation.error();
        }
        paragraph_mark_character_format_ = std::move(candidate);
        return {};
    }

    auto formats = denseFormats();
    for (auto index = start; index < end; ++index) {
        delta.applyTo(formats[index]);
        const auto validation = formats[index].validate();
        if (!validation) {
            return validation.error();
        }
    }
    setContent(text_, std::move(formats));
    return {};
}

Document::Document() : paragraphs_{Paragraph{}} {
    body_blocks_.push_back({BodyBlockKind::paragraph, paragraphs_.front().id()});
}

Document::Document(std::vector<Paragraph> paragraphs) : paragraphs_(std::move(paragraphs)) {
    body_blocks_.reserve(paragraphs_.size());
    for (const auto& paragraph : paragraphs_) {
        body_blocks_.push_back({BodyBlockKind::paragraph, paragraph.id()});
    }
}

Result<Document> Document::create(std::vector<Paragraph> paragraphs) {
    if (paragraphs.empty()) {
        return Error{ErrorCode::invalid_operation, "A document must contain at least one paragraph"};
    }
    std::unordered_set<NodeId, NodeIdHash> identifiers;
    std::size_t image_count = 0;
    std::size_t encoded_image_bytes = 0;
    std::vector<const ImageAtom*> image_payloads_to_inspect;
    for (const auto& paragraph : paragraphs) {
        if (!paragraph.id().isValid()) {
            return Error{ErrorCode::invalid_node_id, "Paragraph NodeId cannot be zero"};
        }
        if (!identifiers.insert(paragraph.id()).second) {
            return Error{ErrorCode::duplicate_node_id, "Paragraph NodeIds must be unique"};
        }
        if (!isValidUtf16(paragraph.text())) {
            return Error{ErrorCode::invalid_utf16, "Paragraph contains malformed UTF-16"};
        }
        std::unordered_set<std::size_t> typed_object_offsets;
        for (std::size_t index = 0; index < paragraph.equations_.size(); ++index) {
            const auto& equation = paragraph.equations_[index];
            if (index > 0 && paragraph.equations_[index - 1].utf16_offset >=
                                 equation.utf16_offset) {
                return Error{ErrorCode::invalid_operation,
                             "Paragraph equations are not in document order"};
            }
            if (equation.utf16_offset >= paragraph.text().size() ||
                paragraph.text()[equation.utf16_offset] !=
                    kInlineObjectReplacementCharacter) {
                return Error{ErrorCode::invalid_operation,
                             "Equation metadata has no matching placeholder"};
            }
            if (!typed_object_offsets.insert(equation.utf16_offset).second) {
                return Error{ErrorCode::invalid_operation,
                             "Inline-object placeholder maps to multiple typed objects"};
            }
            if (!equation.id.isValid()) {
                return Error{ErrorCode::invalid_node_id,
                             "Equation NodeId cannot be zero"};
            }
            if (!identifiers.insert(equation.id).second) {
                return Error{ErrorCode::duplicate_node_id,
                             "Document NodeIds must be unique"};
            }
            const auto source_validation =
                validateEquationSource(equation.canonical_latex);
            if (!source_validation) {
                return source_validation.error();
            }
        }
        for (std::size_t index = 0; index < paragraph.images_.size(); ++index) {
            const auto& image = paragraph.images_[index];
            if (index > 0 && paragraph.images_[index - 1].utf16_offset >=
                                 image.utf16_offset) {
                return Error{ErrorCode::invalid_operation,
                             "Paragraph images are not in document order"};
            }
            if (image.utf16_offset >= paragraph.text().size() ||
                paragraph.text()[image.utf16_offset] !=
                    kInlineObjectReplacementCharacter) {
                return Error{ErrorCode::invalid_operation,
                             "Image metadata has no matching placeholder"};
            }
            if (!typed_object_offsets.insert(image.utf16_offset).second) {
                return Error{ErrorCode::invalid_operation,
                             "Inline-object placeholder maps to multiple typed objects"};
            }
            const auto metadata_validation = validateImageMetadata(image);
            if (!metadata_validation) {
                return metadata_validation.error();
            }
            if (!identifiers.insert(image.id).second) {
                return Error{ErrorCode::duplicate_node_id,
                             "Document NodeIds must be unique"};
            }
            ++image_count;
            if (image_count > kMaximumInlineImagesPerDocument) {
                return Error{ErrorCode::invalid_operation,
                             "Document exceeds the inline-image count limit"};
            }
            if (image.encoded_payload.size() >
                kMaximumDocumentEncodedImageBytes - encoded_image_bytes) {
                return Error{ErrorCode::invalid_operation,
                             "Document exceeds the encoded-image byte limit"};
            }
            encoded_image_bytes += image.encoded_payload.size();
            image_payloads_to_inspect.push_back(&image);
        }
        const auto placeholder_count = static_cast<std::size_t>(std::count(
            paragraph.text().begin(), paragraph.text().end(),
            kInlineObjectReplacementCharacter));
        if (placeholder_count != typed_object_offsets.size()) {
            return Error{ErrorCode::invalid_operation,
                         "Paragraph contains an orphan inline-object placeholder"};
        }
        const auto paragraph_validation = paragraph.format().validate();
        if (!paragraph_validation) {
            return paragraph_validation.error();
        }
        const auto mark_format_validation =
            paragraph.paragraphMarkCharacterFormat().validate();
        if (!mark_format_validation) {
            return mark_format_validation.error();
        }
        for (const auto& run : paragraph.characterFormats()) {
            if (run.start >= run.end || run.end > paragraph.text().size()) {
                return Error{ErrorCode::invalid_formatting, "Character format run is out of bounds"};
            }
            const auto format_validation = run.format.validate();
            if (!format_validation) {
                return format_validation.error();
            }
        }
    }
    for (const auto* image : image_payloads_to_inspect) {
        const auto payload_validation = validateImagePayload(*image);
        if (!payload_validation) {
            return payload_validation.error();
        }
    }
    return Document(std::move(paragraphs));
}

const Paragraph* Document::findParagraph(NodeId id) const noexcept {
    const auto found = std::find_if(paragraphs_.begin(), paragraphs_.end(),
                                    [id](const Paragraph& paragraph) { return paragraph.id() == id; });
    return found == paragraphs_.end() ? nullptr : &*found;
}

const Table* Document::findTable(NodeId id) const noexcept {
    const auto index = tableIndex(id);
    return index ? &tables_[*index] : nullptr;
}

const EquationAtom* Document::findEquation(NodeId id) const noexcept {
    for (const auto& paragraph : paragraphs_) {
        const auto found = std::find_if(
            paragraph.equations().begin(), paragraph.equations().end(),
            [id](const EquationAtom& equation) { return equation.id == id; });
        if (found != paragraph.equations().end()) {
            return &*found;
        }
    }
    return nullptr;
}

const ImageAtom* Document::findImage(NodeId id) const noexcept {
    for (const auto& paragraph : paragraphs_) {
        const auto found = std::find_if(
            paragraph.images().begin(), paragraph.images().end(),
            [id](const ImageAtom& image) { return image.id == id; });
        if (found != paragraph.images().end()) {
            return &*found;
        }
    }
    return nullptr;
}

std::optional<std::size_t> Document::paragraphIndex(NodeId id) const noexcept {
    for (std::size_t index = 0; index < paragraphs_.size(); ++index) {
        if (paragraphs_[index].id() == id) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Document::tableIndex(NodeId id) const noexcept {
    for (std::size_t index = 0; index < tables_.size(); ++index) {
        if (tables_[index].id() == id) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Document::bodyBlockIndex(NodeId id) const noexcept {
    for (std::size_t index = 0; index < body_blocks_.size(); ++index) {
        if (body_blocks_[index].id == id) {
            return index;
        }
    }
    return std::nullopt;
}

bool Document::nodeIdInUse(NodeId id) const noexcept {
    if (findParagraph(id) || findTable(id) || findEquation(id) ||
        findImage(id)) {
        return true;
    }
    for (const auto& table : tables_) {
        if (std::any_of(table.cells().begin(), table.cells().end(),
                        [id](const TableCell& cell) { return cell.id == id; })) {
            return true;
        }
    }
    return false;
}

Result<void> Document::validatePosition(const Position& position) const {
    const auto* paragraph = findParagraph(position.paragraph_id);
    if (!paragraph) {
        return paragraphMissing(position.paragraph_id);
    }
    if (!isUtf16Boundary(paragraph->text(), position.utf16_offset)) {
        return Error{ErrorCode::invalid_position,
                     "Position is out of bounds or splits a UTF-16 surrogate pair"};
    }
    return {};
}

Result<NormalizedRange> Document::normalizeRange(const Range& range) const {
    const auto anchor_validation = validatePosition(range.anchor);
    if (!anchor_validation) {
        return anchor_validation.error();
    }
    const auto focus_validation = validatePosition(range.focus);
    if (!focus_validation) {
        return focus_validation.error();
    }

    const auto anchor_index = paragraphIndex(range.anchor.paragraph_id).value();
    const auto focus_index = paragraphIndex(range.focus.paragraph_id).value();
    const bool forward = anchor_index < focus_index ||
                         (anchor_index == focus_index &&
                          range.anchor.utf16_offset <= range.focus.utf16_offset);
    if (forward) {
        return NormalizedRange{range.anchor, range.focus, anchor_index, focus_index};
    }
    return NormalizedRange{range.focus, range.anchor, focus_index, anchor_index};
}

Result<void> Document::insertText(const Position& position, const std::u16string& text,
                                  const std::optional<CharacterFormat>& format) {
    const auto validation = validatePosition(position);
    if (!validation) {
        return validation.error();
    }
    return paragraphs_[paragraphIndex(position.paragraph_id).value()].insertText(
        position.utf16_offset, text, format);
}

Result<void> Document::insertEquation(
    const Position& position, std::string canonical_latex, bool display,
    NodeId equation_id, const std::optional<CharacterFormat>& format) {
    const auto validation = validatePosition(position);
    if (!validation) {
        return validation.error();
    }
    if (!equation_id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "Equation NodeId cannot be zero"};
    }
    if (nodeIdInUse(equation_id)) {
        return Error{ErrorCode::duplicate_node_id, "Equation NodeId already exists"};
    }
    const auto source_validation = validateEquationSource(canonical_latex);
    if (!source_validation) {
        return source_validation.error();
    }
    return paragraphs_[paragraphIndex(position.paragraph_id).value()].insertEquation(
        position.utf16_offset,
        EquationAtom{equation_id, position.utf16_offset,
                     std::move(canonical_latex), display},
        format);
}

Result<void> Document::insertImage(
    const Position& position, EncodedImagePayload encoded_payload,
    ImageFormat image_format, std::string accessible_name,
    std::int64_t width_emu, std::int64_t height_emu, NodeId image_id,
    const std::optional<CharacterFormat>& character_format,
    ImageLayout layout) {
    const auto position_validation = validatePosition(position);
    if (!position_validation) {
        return position_validation.error();
    }
    if (!image_id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Image NodeId cannot be zero"};
    }
    if (nodeIdInUse(image_id)) {
        return Error{ErrorCode::duplicate_node_id,
                     "Image NodeId already exists"};
    }
    ImageAtom image{image_id, position.utf16_offset,
                    std::move(encoded_payload), image_format,
                    std::move(accessible_name), width_emu, height_emu,
                    layout};
    const auto metadata_validation = validateImageMetadata(image);
    if (!metadata_validation) {
        return metadata_validation.error();
    }
    if (character_format) {
        const auto format_validation = character_format->validate();
        if (!format_validation) {
            return format_validation.error();
        }
    }

    std::size_t image_count = 0;
    std::size_t encoded_image_bytes = 0;
    for (const auto& paragraph : paragraphs_) {
        image_count += paragraph.images().size();
        for (const auto& existing : paragraph.images()) {
            encoded_image_bytes += existing.encoded_payload.size();
        }
    }
    if (image_count >= kMaximumInlineImagesPerDocument) {
        return Error{ErrorCode::invalid_operation,
                     "Document exceeds the inline-image count limit"};
    }
    if (encoded_image_bytes > kMaximumDocumentEncodedImageBytes ||
        image.encoded_payload.size() >
        kMaximumDocumentEncodedImageBytes - encoded_image_bytes) {
        return Error{ErrorCode::invalid_operation,
                     "Document exceeds the encoded-image byte limit"};
    }
    const auto payload_validation = validateImagePayload(image);
    if (!payload_validation) {
        return payload_validation.error();
    }

    return paragraphs_[paragraphIndex(position.paragraph_id).value()].insertImage(
        position.utf16_offset, std::move(image), character_format);
}

Result<void> Document::resizeImage(NodeId image_id, std::int64_t width_emu,
                                   std::int64_t height_emu) {
    if (!image_id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Image NodeId cannot be zero"};
    }
    const auto dimensions_validation =
        validateImageDimensions(width_emu, height_emu);
    if (!dimensions_validation) {
        return dimensions_validation.error();
    }
    for (auto& paragraph : paragraphs_) {
        const auto found = std::find_if(
            paragraph.images_.begin(), paragraph.images_.end(),
            [image_id](const ImageAtom& image) {
                return image.id == image_id;
            });
        if (found != paragraph.images_.end()) {
            found->width_emu = width_emu;
            found->height_emu = height_emu;
            return {};
        }
    }
    return Error{ErrorCode::invalid_operation,
                 "Image not found: " + image_id.toString()};
}

Result<void> Document::setImageLayout(NodeId image_id, ImageLayout layout) {
    if (!image_id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Image NodeId cannot be zero"};
    }
    const auto layout_validation = layout.validate();
    if (!layout_validation) {
        return layout_validation.error();
    }
    for (auto& paragraph : paragraphs_) {
        const auto found = std::find_if(
            paragraph.images_.begin(), paragraph.images_.end(),
            [image_id](const ImageAtom& image) {
                return image.id == image_id;
            });
        if (found != paragraph.images_.end()) {
            found->layout = layout;
            return {};
        }
    }
    return Error{ErrorCode::invalid_operation,
                 "Image not found: " + image_id.toString()};
}

Result<void> Document::setImageAccessibleName(
    NodeId image_id, std::string accessible_name) {
    if (!image_id.isValid()) {
        return Error{ErrorCode::invalid_node_id,
                     "Image NodeId cannot be zero"};
    }
    if (accessible_name.size() > kMaximumImageAccessibleNameBytes) {
        return Error{ErrorCode::invalid_operation,
                     "Image accessible name exceeds the size limit"};
    }
    if (!isValidUtf8(accessible_name)) {
        return Error{ErrorCode::invalid_operation,
                     "Image accessible name is not valid UTF-8"};
    }
    for (auto& paragraph : paragraphs_) {
        const auto found = std::find_if(
            paragraph.images_.begin(), paragraph.images_.end(),
            [image_id](const ImageAtom& image) {
                return image.id == image_id;
            });
        if (found != paragraph.images_.end()) {
            found->accessible_name = std::move(accessible_name);
            return {};
        }
    }
    return Error{ErrorCode::invalid_operation,
                 "Image not found: " + image_id.toString()};
}

Result<void> Document::deleteRange(
    const Range& range,
    const std::optional<CharacterFormat>& empty_paragraph_format) {
    const auto normalized_result = normalizeRange(range);
    if (!normalized_result) {
        return normalized_result.error();
    }
    if (empty_paragraph_format) {
        const auto validation = empty_paragraph_format->validate();
        if (!validation) return validation.error();
    }
    const auto normalized = normalized_result.value();
    if (normalized.empty()) {
        return {};
    }

    if (normalized.start_paragraph_index == normalized.end_paragraph_index) {
        auto& paragraph = paragraphs_[normalized.start_paragraph_index];
        const auto erased = paragraph.erase(normalized.start.utf16_offset,
                                             normalized.end.utf16_offset);
        if (!erased) return erased.error();
        if (paragraph.text_.empty() && empty_paragraph_format) {
            paragraph.paragraph_mark_character_format_ =
                *empty_paragraph_format;
        }
        return {};
    }


    const auto start_block = bodyBlockIndex(normalized.start.paragraph_id);
    const auto end_block = bodyBlockIndex(normalized.end.paragraph_id);
    if (!start_block || !end_block || *start_block >= *end_block) {
        return Error{ErrorCode::invalid_operation,
                     "Paragraph body order is inconsistent"};
    }
    if (std::any_of(
            body_blocks_.begin() + static_cast<std::ptrdiff_t>(*start_block + 1),
            body_blocks_.begin() + static_cast<std::ptrdiff_t>(*end_block),
            [](const BodyBlockRef& block) {
                return block.kind == BodyBlockKind::table;
            })) {
        return Error{ErrorCode::invalid_operation,
                     "A text range cannot implicitly delete across a table"};
    }

    auto& first = paragraphs_[normalized.start_paragraph_index];
    const auto& last = paragraphs_[normalized.end_paragraph_index];
    const auto first_formats = first.denseFormats();
    const auto last_formats = last.denseFormats();

    std::u16string joined = first.text_.substr(0, normalized.start.utf16_offset);
    joined.append(last.text_.substr(normalized.end.utf16_offset));

    std::vector<CharacterFormat> joined_formats;
    joined_formats.reserve(joined.size());
    joined_formats.insert(joined_formats.end(), first_formats.begin(),
                          first_formats.begin() + static_cast<std::ptrdiff_t>(normalized.start.utf16_offset));
    joined_formats.insert(joined_formats.end(),
                          last_formats.begin() + static_cast<std::ptrdiff_t>(normalized.end.utf16_offset),
                          last_formats.end());

    auto joined_empty_format = first.paragraph_mark_character_format_;
    if (joined.empty()) {
        if (!first_formats.empty()) {
            joined_empty_format = first_formats.front();
        } else if (joined_empty_format.empty() && !last_formats.empty()) {
            const auto deleted_index = normalized.end.utf16_offset == 0
                ? 0U
                : std::min(normalized.end.utf16_offset - 1U,
                           last_formats.size() - 1U);
            joined_empty_format = last_formats[deleted_index];
        } else if (joined_empty_format.empty()) {
            joined_empty_format = last.paragraph_mark_character_format_;
        }
    }

    std::vector<EquationAtom> joined_equations;
    joined_equations.reserve(first.equations_.size() + last.equations_.size());
    for (const auto& equation : first.equations_) {
        if (equation.utf16_offset < normalized.start.utf16_offset) {
            joined_equations.push_back(equation);
        }
    }
    for (const auto& equation : last.equations_) {
        if (equation.utf16_offset >= normalized.end.utf16_offset) {
            auto moved = equation;
            moved.utf16_offset = normalized.start.utf16_offset +
                                 equation.utf16_offset -
                                 normalized.end.utf16_offset;
            joined_equations.push_back(std::move(moved));
        }
    }
    std::vector<ImageAtom> joined_images;
    joined_images.reserve(first.images_.size() + last.images_.size());
    for (const auto& image : first.images_) {
        if (image.utf16_offset < normalized.start.utf16_offset) {
            joined_images.push_back(image);
        }
    }
    for (const auto& image : last.images_) {
        if (image.utf16_offset >= normalized.end.utf16_offset) {
            auto moved = image;
            moved.utf16_offset = normalized.start.utf16_offset +
                                 image.utf16_offset -
                                 normalized.end.utf16_offset;
            joined_images.push_back(std::move(moved));
        }
    }
    first.setContent(std::move(joined), std::move(joined_formats));
    if (first.text_.empty()) {
        first.paragraph_mark_character_format_ =
            empty_paragraph_format.value_or(
                std::move(joined_empty_format));
    }
    first.equations_ = std::move(joined_equations);
    first.images_ = std::move(joined_images);

    const std::vector<NodeId> removed_ids(
        [&] {
            std::vector<NodeId> ids;
            ids.reserve(normalized.end_paragraph_index -
                        normalized.start_paragraph_index);
            for (std::size_t index = normalized.start_paragraph_index + 1;
                 index <= normalized.end_paragraph_index; ++index) {
                ids.push_back(paragraphs_[index].id());
            }
            return ids;
        }());
    paragraphs_.erase(paragraphs_.begin() +
                          static_cast<std::ptrdiff_t>(normalized.start_paragraph_index + 1),
                      paragraphs_.begin() +
                          static_cast<std::ptrdiff_t>(normalized.end_paragraph_index + 1));
    std::erase_if(body_blocks_, [&removed_ids](const BodyBlockRef& block) {
        return block.kind == BodyBlockKind::paragraph &&
               std::find(removed_ids.begin(), removed_ids.end(), block.id) !=
                   removed_ids.end();
    });
    return {};
}

Result<void> Document::replaceRange(const Range& range, const std::u16string& text,
                                    const std::optional<CharacterFormat>& format) {
    const auto normalized = normalizeRange(range);
    if (!normalized) {
        return normalized.error();
    }
    if (!isValidUtf16(text)) {
        return Error{ErrorCode::invalid_utf16, "Replacement text contains malformed UTF-16"};
    }
    if (containsParagraphBreak(text)) {
        return Error{ErrorCode::invalid_operation,
                     "ReplaceRange cannot contain a paragraph separator"};
    }
    if (containsInlineObjectPlaceholder(text)) {
        return Error{ErrorCode::invalid_operation,
                     "ReplaceRange cannot create an orphan inline-object placeholder"};
    }
    if (format) {
        const auto validation = format->validate();
        if (!validation) {
            return validation.error();
        }
    }

    const auto insertion = normalized.value().start;
    const bool deletes_content = !normalized.value().empty();
    const auto original_mark = paragraphs_[
        normalized.value().start_paragraph_index]
                                   .paragraph_mark_character_format_;
    const auto deletion = deleteRange(range);
    if (!deletion) {
        return deletion.error();
    }
    const auto insertion_index = paragraphIndex(insertion.paragraph_id);
    if (!insertion_index) {
        return paragraphMissing(insertion.paragraph_id);
    }
    auto& paragraph = paragraphs_[*insertion_index];
    if (!text.empty()) {
        // Deletion-to-empty promotion is only observable if the replacement
        // remains empty. A normal replacement must not rewrite the paragraph
        // mark as a hidden side effect. Restore it after insertion so a null
        // operation format can still inherit the deleted text's format.
        const auto insertion_result = insertText(insertion, text, format);
        if (!insertion_result) {
            return insertion_result.error();
        }
        paragraph.paragraph_mark_character_format_ = original_mark;
        return {};
    }
    if (deletes_content && paragraph.text_.empty() && format) {
        paragraph.paragraph_mark_character_format_ = *format;
    }
    return {};
}

Result<void> Document::applyCharacterFormat(const Range& range,
                                            const CharacterFormatDelta& delta) {
    const auto normalized_result = normalizeRange(range);
    if (!normalized_result) {
        return normalized_result.error();
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) {
        return delta_validation.error();
    }
    const auto normalized = normalized_result.value();
    if (delta.empty()) {
        return {};
    }

    for (auto index = normalized.start_paragraph_index;
         index <= normalized.end_paragraph_index; ++index) {
        const auto start = index == normalized.start_paragraph_index
                               ? normalized.start.utf16_offset
                               : 0;
        const auto end = index == normalized.end_paragraph_index
                             ? normalized.end.utf16_offset
                             : paragraphs_[index].text().size();
        const auto result = paragraphs_[index].applyFormat(start, end, delta);
        if (!result) {
            return result.error();
        }
    }
    return {};
}

Result<void> Document::applyParagraphMarkCharacterFormat(
    NodeId paragraph_id, const CharacterFormatDelta& delta) {
    const auto index = paragraphIndex(paragraph_id);
    if (!index) {
        return paragraphMissing(paragraph_id);
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) {
        return delta_validation.error();
    }
    if (delta.empty()) {
        return {};
    }
    auto candidate = paragraphs_[*index].paragraph_mark_character_format_;
    delta.applyTo(candidate);
    const auto validation = candidate.validate();
    if (!validation) {
        return validation.error();
    }
    paragraphs_[*index].paragraph_mark_character_format_ =
        std::move(candidate);
    return {};
}

Result<void> Document::applyParagraphFormat(const std::vector<NodeId>& paragraph_ids,
                                            const ParagraphFormatDelta& delta) {
    if (paragraph_ids.empty()) {
        return Error{ErrorCode::invalid_operation, "No paragraphs were supplied"};
    }
    const auto delta_validation = delta.validate();
    if (!delta_validation) {
        return delta_validation.error();
    }

    std::vector<std::size_t> indices;
    indices.reserve(paragraph_ids.size());
    for (const auto id : paragraph_ids) {
        const auto index = paragraphIndex(id);
        if (!index) {
            return paragraphMissing(id);
        }
        if (std::find(indices.begin(), indices.end(), *index) == indices.end()) {
            indices.push_back(*index);
        }
    }

    for (const auto index : indices) {
        auto candidate = paragraphs_[index].format_;
        delta.applyTo(candidate);
        const auto validation = candidate.validate();
        if (!validation) {
            return validation.error();
        }
    }
    for (const auto index : indices) {
        delta.applyTo(paragraphs_[index].format_);
    }
    return {};
}

Result<void> Document::splitParagraph(
    const Position& position, NodeId new_paragraph_id,
    std::optional<CharacterFormat> new_paragraph_mark_format) {
    const auto position_validation = validatePosition(position);
    if (!position_validation) {
        return position_validation.error();
    }
    if (!new_paragraph_id.isValid()) {
        return Error{ErrorCode::invalid_node_id, "New paragraph NodeId cannot be zero"};
    }
    if (nodeIdInUse(new_paragraph_id)) {
        return Error{ErrorCode::duplicate_node_id, "New paragraph NodeId already exists"};
    }
    if (new_paragraph_mark_format) {
        const auto mark_format_validation =
            new_paragraph_mark_format->validate();
        if (!mark_format_validation) {
            return mark_format_validation.error();
        }
    }

    const auto index = paragraphIndex(position.paragraph_id).value();
    auto& original = paragraphs_[index];
    const auto original_format = original.format_;
    const auto inherited_mark_format = new_paragraph_mark_format.value_or(
        original.characterFormatAt(position.utf16_offset));
    const auto formats = original.denseFormats();

    auto right_text = original.text_.substr(position.utf16_offset);
    std::vector<CharacterFormat> right_formats(
        formats.begin() + static_cast<std::ptrdiff_t>(position.utf16_offset), formats.end());
    auto left_text = original.text_.substr(0, position.utf16_offset);
    std::vector<CharacterFormat> left_formats(
        formats.begin(), formats.begin() + static_cast<std::ptrdiff_t>(position.utf16_offset));
    std::vector<EquationAtom> left_equations;
    std::vector<EquationAtom> right_equations;
    left_equations.reserve(original.equations_.size());
    right_equations.reserve(original.equations_.size());
    for (const auto& equation : original.equations_) {
        if (equation.utf16_offset < position.utf16_offset) {
            left_equations.push_back(equation);
        } else {
            auto moved = equation;
            moved.utf16_offset -= position.utf16_offset;
            right_equations.push_back(std::move(moved));
        }
    }
    std::vector<ImageAtom> left_images;
    std::vector<ImageAtom> right_images;
    left_images.reserve(original.images_.size());
    right_images.reserve(original.images_.size());
    for (const auto& image : original.images_) {
        if (image.utf16_offset < position.utf16_offset) {
            left_images.push_back(image);
        } else {
            auto moved = image;
            moved.utf16_offset -= position.utf16_offset;
            right_images.push_back(std::move(moved));
        }
    }

    original.setContent(std::move(left_text), std::move(left_formats));
    original.equations_ = std::move(left_equations);
    original.images_ = std::move(left_images);
    Paragraph right(new_paragraph_id, {}, inherited_mark_format);
    right.format_ = original_format;
    right.setContent(std::move(right_text), std::move(right_formats));
    right.equations_ = std::move(right_equations);
    right.images_ = std::move(right_images);
    paragraphs_.insert(paragraphs_.begin() + static_cast<std::ptrdiff_t>(index + 1),
                       std::move(right));
    const auto body_index = bodyBlockIndex(position.paragraph_id);
    if (!body_index) {
        return Error{ErrorCode::invalid_operation,
                     "Paragraph is missing from the document body"};
    }
    body_blocks_.insert(
        body_blocks_.begin() + static_cast<std::ptrdiff_t>(*body_index + 1),
        {BodyBlockKind::paragraph, new_paragraph_id});
    return {};
}

Result<void> Document::mergeWithNext(NodeId paragraph_id) {
    const auto index_result = paragraphIndex(paragraph_id);
    if (!index_result) {
        return paragraphMissing(paragraph_id);
    }
    const auto index = *index_result;
    if (index + 1 >= paragraphs_.size()) {
        return Error{ErrorCode::invalid_operation, "The final paragraph has no following paragraph"};
    }

    const auto body_index = bodyBlockIndex(paragraph_id);
    if (!body_index || *body_index + 1 >= body_blocks_.size() ||
        body_blocks_[*body_index + 1].kind != BodyBlockKind::paragraph ||
        body_blocks_[*body_index + 1].id != paragraphs_[index + 1].id()) {
        return Error{ErrorCode::invalid_operation,
                     "The next body block is not a paragraph"};
    }

    auto& first = paragraphs_[index];
    const auto& second = paragraphs_[index + 1];
    const auto second_id = second.id();
    const auto second_equations = second.equations_;
    const auto second_images = second.images_;
    const auto first_text_size = first.text_.size();
    auto text = first.text_;
    text.append(second.text_);
    auto formats = first.denseFormats();
    const auto second_formats = second.denseFormats();
    formats.insert(formats.end(), second_formats.begin(), second_formats.end());
    first.setContent(std::move(text), std::move(formats));
    first.equations_.reserve(first.equations_.size() + second_equations.size());
    for (auto equation : second_equations) {
        equation.utf16_offset += first_text_size;
        first.equations_.push_back(std::move(equation));
    }
    first.images_.reserve(first.images_.size() + second_images.size());
    for (auto image : second_images) {
        image.utf16_offset += first_text_size;
        first.images_.push_back(std::move(image));
    }
    paragraphs_.erase(paragraphs_.begin() + static_cast<std::ptrdiff_t>(index + 1));
    const auto second_block = bodyBlockIndex(second_id);
    if (second_block) {
        body_blocks_.erase(
            body_blocks_.begin() + static_cast<std::ptrdiff_t>(*second_block));
    }
    return {};
}

Result<void> Document::insertTable(std::optional<NodeId> before_block_id,
                                   Table table) {
    if (nodeIdInUse(table.id())) {
        return Error{ErrorCode::duplicate_node_id, "Table NodeId already exists"};
    }
    std::unordered_set<NodeId, NodeIdHash> incoming_ids;
    incoming_ids.insert(table.id());
    for (const auto& cell : table.cells()) {
        const auto validation = validateTableCell(cell);
        if (!validation) return validation.error();
        if (!incoming_ids.insert(cell.id).second || nodeIdInUse(cell.id)) {
            return Error{ErrorCode::duplicate_node_id,
                         "Table cell NodeId already exists"};
        }
    }

    std::size_t block_index = body_blocks_.size();
    if (before_block_id) {
        const auto found = bodyBlockIndex(*before_block_id);
        if (!found) {
            return bodyBlockMissing(*before_block_id);
        }
        block_index = *found;
    }
    body_blocks_.insert(
        body_blocks_.begin() + static_cast<std::ptrdiff_t>(block_index),
        {BodyBlockKind::table, table.id()});
    tables_.push_back(std::move(table));
    return {};
}

Result<void> Document::setTableCellText(NodeId table_id, std::size_t row,
                                        std::size_t column,
                                        std::u16string text,
                                        const std::optional<CharacterFormat>&
                                            inserted_format) {
    const auto index = tableIndex(table_id);
    if (!index) {
        return tableMissing(table_id);
    }
    return tables_[*index].setCellText(
        row, column, std::move(text), inserted_format);
}

Result<void> Document::applyTableCellCharacterFormat(
    NodeId table_id, std::size_t row, std::size_t column,
    std::size_t start, std::size_t end,
    const CharacterFormatDelta& delta) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    return tables_[*index].applyCellCharacterFormat(
        row, column, start, end, delta);
}

Result<void> Document::applyTableCellParagraphFormat(
    NodeId table_id, std::size_t row, std::size_t column,
    const ParagraphFormatDelta& delta) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    return tables_[*index].applyCellParagraphFormat(
        row, column, delta);
}

Result<void> Document::appendTableRow(NodeId table_id,
                                      std::vector<NodeId> cell_ids) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    return insertTableRow(table_id, tables_[*index].rowCount(),
                          std::move(cell_ids),
                          TableInsertionSource::preceding);
}

Result<void> Document::insertTableRow(NodeId table_id, std::size_t row,
                                      std::vector<NodeId> cell_ids,
                                      TableInsertionSource source) {
    const auto index = tableIndex(table_id);
    if (!index) {
        return tableMissing(table_id);
    }
    const auto& table = tables_[*index];
    if (cell_ids.size() != table.columnCount()) {
        return Error{ErrorCode::invalid_operation,
                     "An appended table row must contain one ID per column"};
    }
    std::unordered_set<NodeId, NodeIdHash> incoming;
    for (const auto id : cell_ids) {
        if (!id.isValid()) {
            return Error{ErrorCode::invalid_node_id,
                         "Table cell NodeId cannot be zero"};
        }
        if (!incoming.insert(id).second || nodeIdInUse(id)) {
            return Error{ErrorCode::duplicate_node_id,
                         "Table cell NodeId already exists"};
        }
    }
    return tables_[*index].insertRow(row, std::move(cell_ids), source);
}

Result<void> Document::deleteTableRows(NodeId table_id, std::size_t row,
                                       std::size_t count) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    return tables_[*index].deleteRows(row, count);
}

Result<void> Document::insertTableColumn(
    NodeId table_id, std::size_t column, std::vector<NodeId> cell_ids,
    TableInsertionSource source) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    const auto& table = tables_[*index];
    if (cell_ids.size() != table.rowCount()) {
        return Error{ErrorCode::invalid_operation,
                     "An inserted table column must contain one ID per row"};
    }
    std::unordered_set<NodeId, NodeIdHash> incoming;
    for (const auto id : cell_ids) {
        if (!id.isValid()) {
            return Error{ErrorCode::invalid_node_id,
                         "Table cell NodeId cannot be zero"};
        }
        if (!incoming.insert(id).second || nodeIdInUse(id)) {
            return Error{ErrorCode::duplicate_node_id,
                         "Table cell NodeId already exists"};
        }
    }
    return tables_[*index].insertColumn(column, std::move(cell_ids), source);
}

Result<void> Document::deleteTableColumns(NodeId table_id,
                                          std::size_t column,
                                          std::size_t count) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    return tables_[*index].deleteColumns(column, count);
}

Result<void> Document::setTableStyle(NodeId table_id, TableStyle style) {
    const auto index = tableIndex(table_id);
    if (!index) return tableMissing(table_id);
    tables_[*index].setStyle(style);
    return {};
}

Result<void> Document::moveTable(NodeId table_id,
                                 std::optional<NodeId> before_block_id) {
    if (!findTable(table_id)) {
        return tableMissing(table_id);
    }
    if (before_block_id && *before_block_id == table_id) {
        return {};
    }
    if (before_block_id && !bodyBlockIndex(*before_block_id)) {
        return bodyBlockMissing(*before_block_id);
    }

    const auto source = bodyBlockIndex(table_id);
    if (!source) {
        return Error{ErrorCode::invalid_operation,
                     "Table is missing from the document body"};
    }
    const BodyBlockRef block = body_blocks_[*source];
    body_blocks_.erase(
        body_blocks_.begin() + static_cast<std::ptrdiff_t>(*source));

    std::size_t destination = body_blocks_.size();
    if (before_block_id) {
        destination = *bodyBlockIndex(*before_block_id);
    }
    body_blocks_.insert(
        body_blocks_.begin() + static_cast<std::ptrdiff_t>(destination), block);
    return {};
}

Result<void> Document::deleteTable(NodeId table_id) {
    const auto table_index = tableIndex(table_id);
    if (!table_index) {
        return tableMissing(table_id);
    }
    const auto block_index = bodyBlockIndex(table_id);
    if (!block_index) {
        return Error{ErrorCode::invalid_operation,
                     "Table is missing from the document body"};
    }
    tables_.erase(tables_.begin() + static_cast<std::ptrdiff_t>(*table_index));
    body_blocks_.erase(
        body_blocks_.begin() + static_cast<std::ptrdiff_t>(*block_index));
    return {};
}

}  // namespace docxstudio::core
