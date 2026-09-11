#pragma once

#include "docxstudio/core/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace docxstudio::core {

enum class UnderlineStyle { none, single, double_line, dotted, dashed, wavy };
enum class BaselinePosition { normal, superscript, subscript };
enum class ParagraphAlignment { left, center, right, justified, distributed };
enum class LineSpacingRule { automatic, at_least, exact };

inline constexpr std::size_t kListLevelCount = 10;
inline constexpr std::int32_t kMaximumListIndentSpaces = 400;
inline constexpr std::int32_t kMaximumListTextIndentSpaces = 64;

// List geometry is expressed in advances of the current font's space glyph.
// `text_indent_spaces` is the gap between the rendered marker and the shared
// text tab stop; it is not an absolute position from the page margin.
struct ListLevelLayout {
    std::int32_t bullet_indent_spaces{0};
    std::int32_t text_indent_spaces{2};

    auto operator<=>(const ListLevelLayout&) const = default;
};

struct ListLayout {
    std::array<ListLevelLayout, kListLevelCount> levels{};

    constexpr ListLayout() noexcept {
        for (std::size_t level = 0; level < levels.size(); ++level) {
            levels[level].bullet_indent_spaces = static_cast<std::int32_t>(level * 4U);
            levels[level].text_indent_spaces = 2;
        }
    }

    auto operator<=>(const ListLayout&) const = default;
};

struct CharacterFormat {
    std::optional<std::string> font_family;
    std::optional<std::int32_t> font_size_half_points;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<UnderlineStyle> underline;
    std::optional<bool> strike;
    std::optional<std::uint32_t> foreground_argb;
    std::optional<std::uint32_t> highlight_argb;
    std::optional<BaselinePosition> baseline;
    std::optional<std::string> language;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Result<void> validate() const;
    auto operator<=>(const CharacterFormat&) const = default;
};

struct ParagraphFormat {
    std::optional<ParagraphAlignment> alignment;
    std::optional<std::int64_t> left_indent_emu;
    std::optional<std::int64_t> right_indent_emu;
    std::optional<std::int64_t> first_line_indent_emu;
    std::optional<std::int64_t> space_before_emu;
    std::optional<std::int64_t> space_after_emu;
    std::optional<std::int64_t> line_spacing_emu;
    std::optional<LineSpacingRule> line_spacing_rule;
    std::optional<bool> keep_with_next;
    std::optional<bool> keep_lines;
    std::optional<bool> page_break_before;
    // All three list properties are present for a semantic list paragraph and
    // absent for a normal paragraph. The stable list identity scopes property
    // edits to one list even when another list uses identical visual settings.
    std::optional<NodeId> list_id;
    std::optional<std::uint8_t> list_level;
    std::optional<ListLayout> list_layout;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Result<void> validate() const;
    auto operator<=>(const ParagraphFormat&) const = default;
};

enum class DeltaAction { unchanged, set, clear };

template <typename T>
struct PropertyDelta {
    DeltaAction action{DeltaAction::unchanged};
    T value{};

    [[nodiscard]] static PropertyDelta set(T new_value) {
        return PropertyDelta{DeltaAction::set, std::move(new_value)};
    }
    [[nodiscard]] static PropertyDelta clear() { return PropertyDelta{DeltaAction::clear, T{}}; }

    void applyTo(std::optional<T>& property) const {
        if (action == DeltaAction::set) {
            property = value;
        } else if (action == DeltaAction::clear) {
            property.reset();
        }
    }

    auto operator<=>(const PropertyDelta&) const = default;
};

struct CharacterFormatDelta {
    PropertyDelta<std::string> font_family;
    PropertyDelta<std::int32_t> font_size_half_points;
    PropertyDelta<bool> bold;
    PropertyDelta<bool> italic;
    PropertyDelta<UnderlineStyle> underline;
    PropertyDelta<bool> strike;
    PropertyDelta<std::uint32_t> foreground_argb;
    PropertyDelta<std::uint32_t> highlight_argb;
    PropertyDelta<BaselinePosition> baseline;
    PropertyDelta<std::string> language;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Result<void> validate() const;
    void applyTo(CharacterFormat& format) const;
};

struct ParagraphFormatDelta {
    PropertyDelta<ParagraphAlignment> alignment;
    PropertyDelta<std::int64_t> left_indent_emu;
    PropertyDelta<std::int64_t> right_indent_emu;
    PropertyDelta<std::int64_t> first_line_indent_emu;
    PropertyDelta<std::int64_t> space_before_emu;
    PropertyDelta<std::int64_t> space_after_emu;
    PropertyDelta<std::int64_t> line_spacing_emu;
    PropertyDelta<LineSpacingRule> line_spacing_rule;
    PropertyDelta<bool> keep_with_next;
    PropertyDelta<bool> keep_lines;
    PropertyDelta<bool> page_break_before;
    PropertyDelta<NodeId> list_id;
    PropertyDelta<std::uint8_t> list_level;
    PropertyDelta<ListLayout> list_layout;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Result<void> validate() const;
    void applyTo(ParagraphFormat& format) const;
};

}  // namespace docxstudio::core
