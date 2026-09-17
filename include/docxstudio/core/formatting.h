#pragma once

#include "docxstudio/core/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

// Presence and value are separate in OOXML: an explicit direct `false` (or a
// direct clear made in the editor) must remain an override even when it looks
// like an inherited/default value. These masks retain that provenance without
// polluting the effective CharacterFormat used for shaping and rendering.
struct CharacterFormatMask {
    bool font_family{false};
    bool font_size_half_points{false};
    bool bold{false};
    bool italic{false};
    bool underline{false};
    bool strike{false};
    bool foreground_argb{false};
    bool highlight_argb{false};
    bool baseline{false};
    bool language{false};

    [[nodiscard]] bool empty() const noexcept;
    void mark(const CharacterFormatDelta& delta) noexcept;
    auto operator<=>(const CharacterFormatMask&) const = default;
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

struct ParagraphFormatMask {
    bool alignment{false};
    bool left_indent_emu{false};
    bool right_indent_emu{false};
    bool first_line_indent_emu{false};
    bool space_before_emu{false};
    bool space_after_emu{false};
    bool line_spacing_emu{false};
    bool line_spacing_rule{false};
    bool keep_with_next{false};
    bool keep_lines{false};
    bool page_break_before{false};

    [[nodiscard]] bool empty() const noexcept;
    void mark(const ParagraphFormatDelta& delta) noexcept;
    auto operator<=>(const ParagraphFormatMask&) const = default;
};

// Paragraph style IDs originate in DOCX packages and are therefore retained
// as bounded UTF-8 strings rather than reduced to an application enum. This
// lets an imported custom ID survive in the semantic model without falsely
// claiming that Owl Docs understands its definition.
inline constexpr std::size_t kMaximumParagraphStyleIdBytes = 1024;

[[nodiscard]] Result<void> validateParagraphStyleId(
    std::string_view style_id);

// The built-in catalog is deliberately separate from style identity. Exact
// lookup recognizes only the stable English IDs below; callers can still
// retain any other valid style ID as an opaque/custom identity.
struct ParagraphStyleDefinition {
    std::string_view id;
    std::string_view display_name;
    std::string_view next_style_id;
    std::optional<std::uint8_t> outline_level;
    CharacterFormat character_format;
    ParagraphFormat paragraph_format;

    // These deltas contain every property specified by the catalog baseline.
    // Unspecified properties remain untouched, including semantic list state,
    // so a UI can apply the baseline and style identity in one operation batch.
    [[nodiscard]] CharacterFormatDelta characterBaselineDelta() const;
    [[nodiscard]] ParagraphFormatDelta paragraphBaselineDelta() const;

    auto operator<=>(const ParagraphStyleDefinition&) const = default;
};

[[nodiscard]] std::span<const ParagraphStyleDefinition>
builtInParagraphStyles() noexcept;
[[nodiscard]] const ParagraphStyleDefinition* findBuiltInParagraphStyle(
    std::string_view style_id) noexcept;

}  // namespace docxstudio::core
