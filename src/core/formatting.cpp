#include "docxstudio/core/formatting.h"

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

}  // namespace docxstudio::core
