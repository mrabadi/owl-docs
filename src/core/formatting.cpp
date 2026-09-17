#include "docxstudio/core/formatting.h"

#include <algorithm>
#include <array>
#include <limits>

namespace docxstudio::core {
namespace {

template <typename... Deltas>
bool allUnchanged(const Deltas&... deltas) noexcept {
    return ((deltas.action == DeltaAction::unchanged) && ...);
}

Result<void> validateEmu(std::optional<std::int64_t> value, bool nonnegative,
                         const char* name) {
    constexpr std::int64_t maximum = 1'000'000'000'000LL;
    if (value && ((nonnegative && *value < 0) || *value < -maximum || *value > maximum)) {
        return Error{ErrorCode::invalid_formatting, std::string("Invalid ") + name};
    }
    return {};
}

Result<void> validateListLayout(const ListLayout& layout) {
    for (std::size_t level = 0; level < layout.levels.size(); ++level) {
        const auto& settings = layout.levels[level];
        if (settings.bullet_indent_spaces < 0 ||
            settings.bullet_indent_spaces > kMaximumListIndentSpaces) {
            return Error{ErrorCode::invalid_formatting,
                         "List bullet indentation must be between 0 and " +
                             std::to_string(kMaximumListIndentSpaces) + " spaces"};
        }
        if (settings.text_indent_spaces < 0 ||
            settings.text_indent_spaces > kMaximumListTextIndentSpaces) {
            return Error{ErrorCode::invalid_formatting,
                         "List text indentation must be between 0 and " +
                             std::to_string(kMaximumListTextIndentSpaces) + " spaces"};
        }
    }
    return {};
}

bool isUtf8Continuation(unsigned char value) noexcept {
    return value >= 0x80U && value <= 0xbfU;
}

// Style IDs are future OOXML identifiers as well as local semantic data. In
// addition to well-formed UTF-8, reject control characters that cannot form a
// useful visible identifier and would need special transport handling.
bool isValidStyleIdUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            if (first < 0x20U || first == 0x7fU) return false;
            ++index;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(text[index + offset]);
            if (!isUtf8Continuation(continuation)) return false;
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        const auto second = static_cast<unsigned char>(text[index + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU) ||
            (code_point >= 0x80U && code_point <= 0x9fU)) {
            return false;
        }
        index += length;
    }
    return true;
}

constexpr std::int64_t points(double value) noexcept {
    return static_cast<std::int64_t>(value * 12700.0);
}

ParagraphFormat paragraphBaseline(
    double space_before, double space_after,
    bool keep_with_next = false, bool keep_lines = false,
    std::int64_t left_indent = 0, std::int64_t right_indent = 0) {
    ParagraphFormat format;
    format.alignment = ParagraphAlignment::left;
    format.left_indent_emu = left_indent;
    format.right_indent_emu = right_indent;
    format.first_line_indent_emu = 0;
    format.space_before_emu = points(space_before);
    format.space_after_emu = points(space_after);
    // In the current layout vocabulary 12 points with the automatic rule is
    // a one-times multiplier, independent of the selected font's natural line
    // height. This matches the application's single-spacing default.
    format.line_spacing_emu = points(12.0);
    format.line_spacing_rule = LineSpacingRule::automatic;
    format.keep_with_next = keep_with_next;
    format.keep_lines = keep_lines;
    format.page_break_before = false;
    return format;
}

CharacterFormat characterBaseline(
    std::optional<std::int32_t> half_points, bool bold, bool italic,
    std::uint32_t foreground_argb) {
    CharacterFormat format;
    // Paragraph styles inherit the document/editor default family. The DOCX
    // writer follows the same model by putting rFonts on Normal and omitting
    // it from derived built-ins; keeping this sparse prevents a style change
    // from silently forcing Carlito in a document configured for another
    // family.
    format.font_size_half_points = half_points;
    format.bold = bold;
    format.italic = italic;
    format.underline = UnderlineStyle::none;
    format.strike = false;
    format.foreground_argb = foreground_argb;
    format.baseline = BaselinePosition::normal;
    return format;
}

const std::array<ParagraphStyleDefinition, 14>& styleCatalog() {
    static const std::array<ParagraphStyleDefinition, 14> catalog{{
        {"Normal", "Normal", "Normal", std::nullopt,
         characterBaseline(22, false, false, 0xff000000U),
         paragraphBaseline(0.0, 0.0)},
        {"NoSpacing", "No Spacing", "NoSpacing", std::nullopt,
         characterBaseline(22, false, false, 0xff000000U),
         paragraphBaseline(0.0, 0.0)},
        {"Title", "Title", "Normal", std::nullopt,
         characterBaseline(56, true, false, 0xff77216fU),
         paragraphBaseline(0.0, 12.0, true, true)},
        {"Subtitle", "Subtitle", "Normal", std::nullopt,
         characterBaseline(28, false, true, 0xff5e2750U),
         paragraphBaseline(0.0, 12.0, true, true)},
        {"Quote", "Quote", "Normal", std::nullopt,
         characterBaseline(22, false, true, 0xff5e2750U),
         paragraphBaseline(6.0, 6.0, false, true, points(36.0),
                           points(36.0))},
        {"Heading1", "Heading 1", "Normal", std::uint8_t{0},
         characterBaseline(32, true, false, 0xffe95420U),
         paragraphBaseline(12.0, 6.0, true, true)},
        {"Heading2", "Heading 2", "Normal", std::uint8_t{1},
         characterBaseline(26, true, false, 0xff77216fU),
         paragraphBaseline(10.0, 4.0, true, true)},
        {"Heading3", "Heading 3", "Normal", std::uint8_t{2},
         characterBaseline(24, true, false, 0xff5e2750U),
         paragraphBaseline(8.0, 3.0, true, true)},
        {"Heading4", "Heading 4", "Normal", std::uint8_t{3},
         characterBaseline(22, true, false, 0xff5e2750U),
         paragraphBaseline(8.0, 2.0, true, true)},
        {"Heading5", "Heading 5", "Normal", std::uint8_t{4},
         characterBaseline(22, true, true, 0xff77216fU),
         paragraphBaseline(7.0, 2.0, true, true)},
        {"Heading6", "Heading 6", "Normal", std::uint8_t{5},
         characterBaseline(22, false, true, 0xff77216fU),
         paragraphBaseline(6.0, 2.0, true, true)},
        {"Heading7", "Heading 7", "Normal", std::uint8_t{6},
         characterBaseline(20, true, false, 0xff2c001eU),
         paragraphBaseline(6.0, 2.0, true, true)},
        {"Heading8", "Heading 8", "Normal", std::uint8_t{7},
         characterBaseline(20, false, true, 0xff2c001eU),
         paragraphBaseline(5.0, 2.0, true, true)},
        {"Heading9", "Heading 9", "Normal", std::uint8_t{8},
         characterBaseline(20, true, true, 0xff2c001eU),
         paragraphBaseline(4.0, 2.0, true, true)},
    }};
    return catalog;
}

template <typename T>
PropertyDelta<T> setWhenPresent(const std::optional<T>& value) {
    return value ? PropertyDelta<T>::set(*value) : PropertyDelta<T>{};
}

}  // namespace

bool CharacterFormat::empty() const noexcept {
    return !font_family && !font_size_half_points && !bold && !italic && !underline && !strike &&
           !foreground_argb && !highlight_argb && !baseline && !language;
}

Result<void> CharacterFormat::validate() const {
    if (font_family && font_family->empty()) {
        return Error{ErrorCode::invalid_formatting, "Font family cannot be empty"};
    }
    if (font_size_half_points && (*font_size_half_points <= 0 || *font_size_half_points > 3276)) {
        return Error{ErrorCode::invalid_formatting, "Font size must be between 0.5 and 1638 points"};
    }
    if (language && language->empty()) {
        return Error{ErrorCode::invalid_formatting, "Language cannot be empty"};
    }
    return {};
}

bool ParagraphFormat::empty() const noexcept {
    return !alignment && !left_indent_emu && !right_indent_emu && !first_line_indent_emu &&
           !space_before_emu && !space_after_emu && !line_spacing_emu && !line_spacing_rule &&
           !keep_with_next && !keep_lines && !page_break_before && !list_id && !list_level &&
           !list_layout;
}

Result<void> ParagraphFormat::validate() const {
    for (const auto& result : {validateEmu(left_indent_emu, false, "left indent"),
                               validateEmu(right_indent_emu, false, "right indent"),
                               validateEmu(first_line_indent_emu, false, "first-line indent"),
                               validateEmu(space_before_emu, true, "space before"),
                               validateEmu(space_after_emu, true, "space after"),
                               validateEmu(line_spacing_emu, true, "line spacing")}) {
        if (!result) {
            return result.error();
        }
    }
    if (line_spacing_emu && *line_spacing_emu == 0) {
        return Error{ErrorCode::invalid_formatting, "Line spacing must be positive"};
    }
    const auto listPropertyCount = static_cast<unsigned>(list_id.has_value()) +
                                   static_cast<unsigned>(list_level.has_value()) +
                                   static_cast<unsigned>(list_layout.has_value());
    if (listPropertyCount != 0U && listPropertyCount != 3U) {
        return Error{ErrorCode::invalid_formatting,
                     "List identity, level, and layout must be set or cleared together"};
    }
    if (list_id && !list_id->isValid()) {
        return Error{ErrorCode::invalid_formatting, "List NodeId cannot be zero"};
    }
    if (list_level && *list_level >= kListLevelCount) {
        return Error{ErrorCode::invalid_formatting, "List level must be between 0 and 9"};
    }
    if (list_layout) {
        const auto result = validateListLayout(*list_layout);
        if (!result) {
            return result.error();
        }
    }
    return {};
}

bool CharacterFormatDelta::empty() const noexcept {
    return allUnchanged(font_family, font_size_half_points, bold, italic, underline, strike,
                        foreground_argb, highlight_argb, baseline, language);
}

Result<void> CharacterFormatDelta::validate() const {
    CharacterFormat candidate;
    applyTo(candidate);
    return candidate.validate();
}

void CharacterFormatDelta::applyTo(CharacterFormat& format) const {
    font_family.applyTo(format.font_family);
    font_size_half_points.applyTo(format.font_size_half_points);
    bold.applyTo(format.bold);
    italic.applyTo(format.italic);
    underline.applyTo(format.underline);
    strike.applyTo(format.strike);
    foreground_argb.applyTo(format.foreground_argb);
    highlight_argb.applyTo(format.highlight_argb);
    baseline.applyTo(format.baseline);
    language.applyTo(format.language);
}

bool CharacterFormatMask::empty() const noexcept {
    return !font_family && !font_size_half_points && !bold && !italic &&
           !underline && !strike && !foreground_argb && !highlight_argb &&
           !baseline && !language;
}

void CharacterFormatMask::mark(
    const CharacterFormatDelta& delta) noexcept {
    font_family = font_family ||
                  delta.font_family.action != DeltaAction::unchanged;
    font_size_half_points =
        font_size_half_points ||
        delta.font_size_half_points.action != DeltaAction::unchanged;
    bold = bold || delta.bold.action != DeltaAction::unchanged;
    italic = italic || delta.italic.action != DeltaAction::unchanged;
    underline = underline ||
                delta.underline.action != DeltaAction::unchanged;
    strike = strike || delta.strike.action != DeltaAction::unchanged;
    foreground_argb =
        foreground_argb ||
        delta.foreground_argb.action != DeltaAction::unchanged;
    highlight_argb =
        highlight_argb ||
        delta.highlight_argb.action != DeltaAction::unchanged;
    baseline = baseline || delta.baseline.action != DeltaAction::unchanged;
    language = language || delta.language.action != DeltaAction::unchanged;
}

bool ParagraphFormatDelta::empty() const noexcept {
    return allUnchanged(alignment, left_indent_emu, right_indent_emu, first_line_indent_emu,
                        space_before_emu, space_after_emu, line_spacing_emu, line_spacing_rule,
                        keep_with_next, keep_lines, page_break_before, list_id, list_level,
                        list_layout);
}

Result<void> ParagraphFormatDelta::validate() const {
    // A delta is intentionally allowed to change just the current level or
    // layout of an existing list. Document::applyParagraphFormat validates the
    // fully-applied candidate, including the all-or-none list invariant.
    ParagraphFormat candidate;
    applyTo(candidate);
    candidate.list_id.reset();
    candidate.list_level.reset();
    candidate.list_layout.reset();
    const auto ordinaryValidation = candidate.validate();
    if (!ordinaryValidation) {
        return ordinaryValidation.error();
    }
    if (list_id.action == DeltaAction::set && !list_id.value.isValid()) {
        return Error{ErrorCode::invalid_formatting, "List NodeId cannot be zero"};
    }
    if (list_level.action == DeltaAction::set && list_level.value >= kListLevelCount) {
        return Error{ErrorCode::invalid_formatting, "List level must be between 0 and 9"};
    }
    if (list_layout.action == DeltaAction::set) {
        return validateListLayout(list_layout.value);
    }
    return {};
}

void ParagraphFormatDelta::applyTo(ParagraphFormat& format) const {
    alignment.applyTo(format.alignment);
    left_indent_emu.applyTo(format.left_indent_emu);
    right_indent_emu.applyTo(format.right_indent_emu);
    first_line_indent_emu.applyTo(format.first_line_indent_emu);
    space_before_emu.applyTo(format.space_before_emu);
    space_after_emu.applyTo(format.space_after_emu);
    line_spacing_emu.applyTo(format.line_spacing_emu);
    line_spacing_rule.applyTo(format.line_spacing_rule);
    keep_with_next.applyTo(format.keep_with_next);
    keep_lines.applyTo(format.keep_lines);
    page_break_before.applyTo(format.page_break_before);
    list_id.applyTo(format.list_id);
    list_level.applyTo(format.list_level);
    list_layout.applyTo(format.list_layout);
}

bool ParagraphFormatMask::empty() const noexcept {
    return !alignment && !left_indent_emu && !right_indent_emu &&
           !first_line_indent_emu && !space_before_emu && !space_after_emu &&
           !line_spacing_emu && !line_spacing_rule && !keep_with_next &&
           !keep_lines && !page_break_before;
}

void ParagraphFormatMask::mark(
    const ParagraphFormatDelta& delta) noexcept {
    alignment = alignment || delta.alignment.action != DeltaAction::unchanged;
    left_indent_emu =
        left_indent_emu ||
        delta.left_indent_emu.action != DeltaAction::unchanged;
    right_indent_emu =
        right_indent_emu ||
        delta.right_indent_emu.action != DeltaAction::unchanged;
    first_line_indent_emu =
        first_line_indent_emu ||
        delta.first_line_indent_emu.action != DeltaAction::unchanged;
    space_before_emu =
        space_before_emu ||
        delta.space_before_emu.action != DeltaAction::unchanged;
    space_after_emu =
        space_after_emu ||
        delta.space_after_emu.action != DeltaAction::unchanged;
    line_spacing_emu =
        line_spacing_emu ||
        delta.line_spacing_emu.action != DeltaAction::unchanged;
    line_spacing_rule =
        line_spacing_rule ||
        delta.line_spacing_rule.action != DeltaAction::unchanged;
    keep_with_next =
        keep_with_next ||
        delta.keep_with_next.action != DeltaAction::unchanged;
    keep_lines =
        keep_lines || delta.keep_lines.action != DeltaAction::unchanged;
    page_break_before =
        page_break_before ||
        delta.page_break_before.action != DeltaAction::unchanged;
}

Result<void> validateParagraphStyleId(std::string_view style_id) {
    if (style_id.empty()) {
        return Error{ErrorCode::invalid_formatting,
                     "Paragraph style ID cannot be empty"};
    }
    if (style_id.size() > kMaximumParagraphStyleIdBytes) {
        return Error{ErrorCode::invalid_formatting,
                     "Paragraph style ID exceeds the size limit"};
    }
    if (!isValidStyleIdUtf8(style_id)) {
        return Error{ErrorCode::invalid_formatting,
                     "Paragraph style ID is not valid visible UTF-8"};
    }
    return {};
}

CharacterFormatDelta ParagraphStyleDefinition::characterBaselineDelta() const {
    CharacterFormatDelta delta;
    delta.font_family = setWhenPresent(character_format.font_family);
    delta.font_size_half_points =
        setWhenPresent(character_format.font_size_half_points);
    delta.bold = setWhenPresent(character_format.bold);
    delta.italic = setWhenPresent(character_format.italic);
    delta.underline = setWhenPresent(character_format.underline);
    delta.strike = setWhenPresent(character_format.strike);
    delta.foreground_argb = setWhenPresent(character_format.foreground_argb);
    delta.highlight_argb = setWhenPresent(character_format.highlight_argb);
    delta.baseline = setWhenPresent(character_format.baseline);
    delta.language = setWhenPresent(character_format.language);
    return delta;
}

ParagraphFormatDelta ParagraphStyleDefinition::paragraphBaselineDelta() const {
    ParagraphFormatDelta delta;
    delta.alignment = setWhenPresent(paragraph_format.alignment);
    delta.left_indent_emu = setWhenPresent(paragraph_format.left_indent_emu);
    delta.right_indent_emu = setWhenPresent(paragraph_format.right_indent_emu);
    delta.first_line_indent_emu =
        setWhenPresent(paragraph_format.first_line_indent_emu);
    delta.space_before_emu = setWhenPresent(paragraph_format.space_before_emu);
    delta.space_after_emu = setWhenPresent(paragraph_format.space_after_emu);
    delta.line_spacing_emu = setWhenPresent(paragraph_format.line_spacing_emu);
    delta.line_spacing_rule =
        setWhenPresent(paragraph_format.line_spacing_rule);
    delta.keep_with_next = setWhenPresent(paragraph_format.keep_with_next);
    delta.keep_lines = setWhenPresent(paragraph_format.keep_lines);
    delta.page_break_before = setWhenPresent(paragraph_format.page_break_before);
    return delta;
}

std::span<const ParagraphStyleDefinition> builtInParagraphStyles() noexcept {
    return styleCatalog();
}

const ParagraphStyleDefinition* findBuiltInParagraphStyle(
    std::string_view style_id) noexcept {
    const auto& catalog = styleCatalog();
    const auto found = std::find_if(
        catalog.begin(), catalog.end(), [style_id](const auto& style) {
            return style.id == style_id;
        });
    return found == catalog.end() ? nullptr : &*found;
}

}  // namespace docxstudio::core
