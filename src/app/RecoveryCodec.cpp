#include "docxstudio/app/RecoveryCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace docxstudio::app {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumParagraphs = 1'000'000U;
constexpr std::size_t kMaximumTables = 100'000U;
constexpr std::size_t kMaximumTableCells = 1'000'000U;
constexpr std::size_t kMaximumEquations = 1'000'000U;
// UTF-16 units are JSON encoded as bounded integers; eight million units can
// approach the 64 MiB serialized payload ceiling in the worst case.
constexpr std::size_t kMaximumCodeUnits = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumEquationSourceBytes = 8U * 1024U * 1024U;

Json encodeUtf16(const std::u16string& value) {
    Json output = Json::array();
    for (const char16_t codeUnit : value) {
        output.push_back(static_cast<std::uint16_t>(codeUnit));
    }
    return output;
}

bool decodeUtf16(const Json& input, std::u16string& value,
                 std::size_t& totalCodeUnits, std::string& error) {
    if (!input.is_array() || input.size() > kMaximumCodeUnits - totalCodeUnits) {
        error = "Recovery document text exceeds the size limit";
        return false;
    }
    value.clear();
    value.reserve(input.size());
    for (const auto& unit : input) {
        if (!unit.is_number_unsigned()) {
            error = "Recovery text contains a non-UTF-16 value";
            return false;
        }
        const auto encoded = unit.get<std::uint64_t>();
        if (encoded > std::numeric_limits<std::uint16_t>::max()) {
            error = "Recovery text contains an out-of-range UTF-16 value";
            return false;
        }
        value.push_back(static_cast<char16_t>(encoded));
    }
    if (!core::isValidUtf16(value)) {
        error = "Recovery text contains malformed UTF-16";
        return false;
    }
    totalCodeUnits += value.size();
    return true;
}

template <typename T>
void putOptional(Json& object, const char* key, const std::optional<T>& value) {
    if (value) object[key] = *value;
}

template <typename T>
bool readOptional(const Json& object, const char* key, std::optional<T>& value,
                  std::string& error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    try {
        value = found->get<T>();
        return true;
    } catch (const std::exception&) {
        error = std::string("Recovery field '") + key + "' has the wrong type";
        return false;
    }
}

const char* underlineName(core::UnderlineStyle value) {
    switch (value) {
        case core::UnderlineStyle::none: return "none";
        case core::UnderlineStyle::single: return "single";
        case core::UnderlineStyle::double_line: return "double";
        case core::UnderlineStyle::dotted: return "dotted";
        case core::UnderlineStyle::dashed: return "dashed";
        case core::UnderlineStyle::wavy: return "wavy";
    }
    return "none";
}

std::optional<core::UnderlineStyle> parseUnderline(const std::string& value) {
    if (value == "none") return core::UnderlineStyle::none;
    if (value == "single") return core::UnderlineStyle::single;
    if (value == "double") return core::UnderlineStyle::double_line;
    if (value == "dotted") return core::UnderlineStyle::dotted;
    if (value == "dashed") return core::UnderlineStyle::dashed;
    if (value == "wavy") return core::UnderlineStyle::wavy;
    return std::nullopt;
}

const char* baselineName(core::BaselinePosition value) {
    switch (value) {
        case core::BaselinePosition::normal: return "normal";
        case core::BaselinePosition::superscript: return "superscript";
        case core::BaselinePosition::subscript: return "subscript";
    }
    return "normal";
}

std::optional<core::BaselinePosition> parseBaseline(const std::string& value) {
    if (value == "normal") return core::BaselinePosition::normal;
    if (value == "superscript") return core::BaselinePosition::superscript;
    if (value == "subscript") return core::BaselinePosition::subscript;
    return std::nullopt;
}

const char* alignmentName(core::ParagraphAlignment value) {
    switch (value) {
        case core::ParagraphAlignment::left: return "left";
        case core::ParagraphAlignment::center: return "center";
        case core::ParagraphAlignment::right: return "right";
        case core::ParagraphAlignment::justified: return "justified";
        case core::ParagraphAlignment::distributed: return "distributed";
    }
    return "left";
}

std::optional<core::ParagraphAlignment> parseAlignment(const std::string& value) {
    if (value == "left") return core::ParagraphAlignment::left;
    if (value == "center") return core::ParagraphAlignment::center;
    if (value == "right") return core::ParagraphAlignment::right;
    if (value == "justified") return core::ParagraphAlignment::justified;
    if (value == "distributed") return core::ParagraphAlignment::distributed;
    return std::nullopt;
}

const char* spacingRuleName(core::LineSpacingRule value) {
    switch (value) {
        case core::LineSpacingRule::automatic: return "automatic";
        case core::LineSpacingRule::at_least: return "at_least";
        case core::LineSpacingRule::exact: return "exact";
    }
    return "automatic";
}

std::optional<core::LineSpacingRule> parseSpacingRule(const std::string& value) {
    if (value == "automatic") return core::LineSpacingRule::automatic;
    if (value == "at_least") return core::LineSpacingRule::at_least;
    if (value == "exact") return core::LineSpacingRule::exact;
    return std::nullopt;
}

const char* tableStyleName(core::TableStyle value) {
    switch (value) {
        case core::TableStyle::plain: return "plain";
        case core::TableStyle::grid: return "grid";
        case core::TableStyle::light_gray: return "light-gray";
        case core::TableStyle::light_blue: return "light-blue";
        case core::TableStyle::light_orange: return "light-orange";
        case core::TableStyle::medium_blue: return "medium-blue";
        case core::TableStyle::medium_green: return "medium-green";
        case core::TableStyle::medium_orange: return "medium-orange";
        case core::TableStyle::aubergine: return "aubergine";
        case core::TableStyle::orange_accent: return "orange-accent";
        case core::TableStyle::banded_blue: return "banded-blue";
        case core::TableStyle::banded_aubergine: return "banded-aubergine";
        case core::TableStyle::dark_header: return "dark-header";
    }
    return "grid";
}

std::optional<core::TableStyle> parseTableStyle(const std::string& value) {
    if (value == "plain") return core::TableStyle::plain;
    if (value == "grid") return core::TableStyle::grid;
    if (value == "light-gray") return core::TableStyle::light_gray;
    if (value == "light-blue") return core::TableStyle::light_blue;
    if (value == "light-orange") return core::TableStyle::light_orange;
    if (value == "medium-blue") return core::TableStyle::medium_blue;
    if (value == "medium-green") return core::TableStyle::medium_green;
    if (value == "medium-orange") return core::TableStyle::medium_orange;
    if (value == "aubergine") return core::TableStyle::aubergine;
    if (value == "orange-accent") return core::TableStyle::orange_accent;
    if (value == "banded-blue") return core::TableStyle::banded_blue;
    if (value == "banded-aubergine") return core::TableStyle::banded_aubergine;
    if (value == "dark-header") return core::TableStyle::dark_header;
    return std::nullopt;
}

Json encodeCharacterFormat(const core::CharacterFormat& format) {
    Json output = Json::object();
    putOptional(output, "font_family", format.font_family);
    putOptional(output, "font_size_half_points", format.font_size_half_points);
    putOptional(output, "bold", format.bold);
    putOptional(output, "italic", format.italic);
    if (format.underline) output["underline"] = underlineName(*format.underline);
    putOptional(output, "strike", format.strike);
    putOptional(output, "foreground_argb", format.foreground_argb);
    putOptional(output, "highlight_argb", format.highlight_argb);
    if (format.baseline) output["baseline"] = baselineName(*format.baseline);
    putOptional(output, "language", format.language);
    return output;
}

Json encodeParagraphFormat(const core::ParagraphFormat& format) {
    Json output = Json::object();
    if (format.alignment) output["alignment"] = alignmentName(*format.alignment);
    putOptional(output, "left_indent_emu", format.left_indent_emu);
    putOptional(output, "right_indent_emu", format.right_indent_emu);
    putOptional(output, "first_line_indent_emu", format.first_line_indent_emu);
    putOptional(output, "space_before_emu", format.space_before_emu);
    putOptional(output, "space_after_emu", format.space_after_emu);
    putOptional(output, "line_spacing_emu", format.line_spacing_emu);
    if (format.line_spacing_rule)
        output["line_spacing_rule"] = spacingRuleName(*format.line_spacing_rule);
    putOptional(output, "keep_with_next", format.keep_with_next);
    putOptional(output, "keep_lines", format.keep_lines);
    putOptional(output, "page_break_before", format.page_break_before);
    if (format.list_id) output["list_id"] = format.list_id->toString();
    if (format.list_level) output["list_level"] = static_cast<unsigned>(*format.list_level);
    if (format.list_layout) {
        Json levels = Json::array();
        for (const auto& level : format.list_layout->levels) {
            levels.push_back({{"bullet_indent_spaces", level.bullet_indent_spaces},
                              {"text_indent_spaces", level.text_indent_spaces}});
        }
        output["list_layout"] = {{"levels", std::move(levels)}};
    }
    return output;
}

bool decodeCharacterFormat(const Json& input, core::CharacterFormat& format,
                           std::string& error) {
    if (!input.is_object()) {
        error = "Recovery character format is not an object";
        return false;
    }
    if (!readOptional(input, "font_family", format.font_family, error) ||
        !readOptional(input, "font_size_half_points", format.font_size_half_points, error) ||
        !readOptional(input, "bold", format.bold, error) ||
        !readOptional(input, "italic", format.italic, error) ||
        !readOptional(input, "strike", format.strike, error) ||
        !readOptional(input, "foreground_argb", format.foreground_argb, error) ||
        !readOptional(input, "highlight_argb", format.highlight_argb, error) ||
        !readOptional(input, "language", format.language, error)) {
        return false;
    }
    if (const auto found = input.find("underline"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery underline value is not a string";
            return false;
        }
        format.underline = parseUnderline(found->get<std::string>());
        if (!format.underline) {
            error = "Recovery underline value is unknown";
            return false;
        }
    }
    if (const auto found = input.find("baseline"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery baseline value is not a string";
            return false;
        }
        format.baseline = parseBaseline(found->get<std::string>());
        if (!format.baseline) {
            error = "Recovery baseline value is unknown";
            return false;
        }
    }
    const auto validation = format.validate();
    if (!validation) {
        error = validation.error().message;
        return false;
    }
    return true;
}

bool decodeParagraphFormat(const Json& input, core::ParagraphFormat& format,
                           std::string& error) {
    if (!input.is_object()) {
        error = "Recovery paragraph format is not an object";
        return false;
    }
    if (!readOptional(input, "left_indent_emu", format.left_indent_emu, error) ||
        !readOptional(input, "right_indent_emu", format.right_indent_emu, error) ||
        !readOptional(input, "first_line_indent_emu", format.first_line_indent_emu, error) ||
        !readOptional(input, "space_before_emu", format.space_before_emu, error) ||
        !readOptional(input, "space_after_emu", format.space_after_emu, error) ||
        !readOptional(input, "line_spacing_emu", format.line_spacing_emu, error) ||
        !readOptional(input, "keep_with_next", format.keep_with_next, error) ||
        !readOptional(input, "keep_lines", format.keep_lines, error) ||
        !readOptional(input, "page_break_before", format.page_break_before, error)) {
        return false;
    }
    if (const auto found = input.find("alignment"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery alignment value is not a string";
            return false;
        }
        format.alignment = parseAlignment(found->get<std::string>());
        if (!format.alignment) {
            error = "Recovery alignment value is unknown";
            return false;
        }
    }
    if (const auto found = input.find("line_spacing_rule"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery line-spacing rule is not a string";
            return false;
        }
        format.line_spacing_rule = parseSpacingRule(found->get<std::string>());
        if (!format.line_spacing_rule) {
            error = "Recovery line-spacing rule is unknown";
            return false;
        }
    }
    if (const auto found = input.find("list_id"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery list identity is not a string";
            return false;
        }
        format.list_id = core::NodeId::parse(found->get<std::string>());
        if (!format.list_id) {
            error = "Recovery list identity is invalid";
            return false;
        }
    }
    if (const auto found = input.find("list_level"); found != input.end()) {
        if (!found->is_number_integer()) {
            error = "Recovery list level is not an integer";
            return false;
        }
        const auto level = found->get<std::int64_t>();
        if (level < 0 || level >= static_cast<std::int64_t>(core::kListLevelCount)) {
            error = "Recovery list level must be between 0 and 9";
            return false;
        }
        format.list_level = static_cast<std::uint8_t>(level);
    }
    if (const auto found = input.find("list_layout"); found != input.end()) {
        if (!found->is_object()) {
            error = "Recovery list layout is not an object";
            return false;
        }
        const auto levels = found->find("levels");
        if (levels == found->end() || !levels->is_array() ||
            levels->size() != core::kListLevelCount) {
            error = "Recovery list layout must contain exactly 10 levels";
            return false;
        }
        core::ListLayout layout;
        for (std::size_t index = 0; index < levels->size(); ++index) {
            const auto& encodedLevel = (*levels)[index];
            if (!encodedLevel.is_object()) {
                error = "Recovery list-layout level is not an object";
                return false;
            }
            const auto bullet = encodedLevel.find("bullet_indent_spaces");
            const auto textIndent = encodedLevel.find("text_indent_spaces");
            if (bullet == encodedLevel.end() || !bullet->is_number_integer() ||
                textIndent == encodedLevel.end() || !textIndent->is_number_integer()) {
                error = "Recovery list-layout indentation is not an integer";
                return false;
            }
            const auto bulletValue = bullet->get<std::int64_t>();
            const auto textValue = textIndent->get<std::int64_t>();
            if (bulletValue < 0 || bulletValue > core::kMaximumListIndentSpaces ||
                textValue < 0 || textValue > core::kMaximumListTextIndentSpaces) {
                error = "Recovery list-layout indentation is out of range";
                return false;
            }
            layout.levels[index].bullet_indent_spaces =
                static_cast<std::int32_t>(bulletValue);
            layout.levels[index].text_indent_spaces =
                static_cast<std::int32_t>(textValue);
        }
        format.list_layout = std::move(layout);
    }
    const auto validation = format.validate();
    if (!validation) {
        error = validation.error().message;
        return false;
    }
    return true;
}

core::CharacterFormatDelta completeDelta(const core::CharacterFormat& format) {
    core::CharacterFormatDelta delta;
    if (format.font_family) delta.font_family = core::PropertyDelta<std::string>::set(*format.font_family);
    if (format.font_size_half_points) delta.font_size_half_points = core::PropertyDelta<std::int32_t>::set(*format.font_size_half_points);
    if (format.bold) delta.bold = core::PropertyDelta<bool>::set(*format.bold);
    if (format.italic) delta.italic = core::PropertyDelta<bool>::set(*format.italic);
    if (format.underline) delta.underline = core::PropertyDelta<core::UnderlineStyle>::set(*format.underline);
    if (format.strike) delta.strike = core::PropertyDelta<bool>::set(*format.strike);
    if (format.foreground_argb) delta.foreground_argb = core::PropertyDelta<std::uint32_t>::set(*format.foreground_argb);
    if (format.highlight_argb) delta.highlight_argb = core::PropertyDelta<std::uint32_t>::set(*format.highlight_argb);
    if (format.baseline) delta.baseline = core::PropertyDelta<core::BaselinePosition>::set(*format.baseline);
    if (format.language) delta.language = core::PropertyDelta<std::string>::set(*format.language);
    return delta;
}

core::ParagraphFormatDelta completeDelta(const core::ParagraphFormat& format) {
    core::ParagraphFormatDelta delta;
    if (format.alignment) delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(*format.alignment);
    if (format.left_indent_emu) delta.left_indent_emu = core::PropertyDelta<std::int64_t>::set(*format.left_indent_emu);
    if (format.right_indent_emu) delta.right_indent_emu = core::PropertyDelta<std::int64_t>::set(*format.right_indent_emu);
    if (format.first_line_indent_emu) delta.first_line_indent_emu = core::PropertyDelta<std::int64_t>::set(*format.first_line_indent_emu);
    if (format.space_before_emu) delta.space_before_emu = core::PropertyDelta<std::int64_t>::set(*format.space_before_emu);
    if (format.space_after_emu) delta.space_after_emu = core::PropertyDelta<std::int64_t>::set(*format.space_after_emu);
    if (format.line_spacing_emu) delta.line_spacing_emu = core::PropertyDelta<std::int64_t>::set(*format.line_spacing_emu);
    if (format.line_spacing_rule) delta.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(*format.line_spacing_rule);
    if (format.keep_with_next) delta.keep_with_next = core::PropertyDelta<bool>::set(*format.keep_with_next);
    if (format.keep_lines) delta.keep_lines = core::PropertyDelta<bool>::set(*format.keep_lines);
    if (format.page_break_before) delta.page_break_before = core::PropertyDelta<bool>::set(*format.page_break_before);
    if (format.list_id) delta.list_id = core::PropertyDelta<core::NodeId>::set(*format.list_id);
    if (format.list_level) delta.list_level = core::PropertyDelta<std::uint8_t>::set(*format.list_level);
    if (format.list_layout) delta.list_layout = core::PropertyDelta<core::ListLayout>::set(*format.list_layout);
    return delta;
}

bool validPage(const RecoveryPageLayout& page) {
    const auto finiteInRange = [](double value, double minimum, double maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    return finiteInRange(page.width_points, 72.0, 20'000.0) &&
           finiteInRange(page.height_points, 72.0, 20'000.0) &&
           finiteInRange(page.margin_top_points, 0.0, page.height_points / 2.0) &&
           finiteInRange(page.margin_right_points, 0.0, page.width_points / 2.0) &&
           finiteInRange(page.margin_bottom_points, 0.0, page.height_points / 2.0) &&
           finiteInRange(page.margin_left_points, 0.0, page.width_points / 2.0);
}

}  // namespace

std::optional<std::string> RecoveryCodec::encode(const RecoveryDocument& recovery,
                                                 std::string& error) {
    error.clear();
    if (!validPage(recovery.page)) {
        error = "Recovery page dimensions or margins are invalid";
        return std::nullopt;
    }
    if (recovery.document.paragraphs().empty() ||
        recovery.document.paragraphs().size() > kMaximumParagraphs ||
        recovery.document.tables().size() > kMaximumTables) {
        error = "Recovery document has too many paragraphs or tables";
        return std::nullopt;
    }
    std::size_t totalCodeUnits = 0;
    std::size_t totalEquations = 0;
    std::size_t totalEquationBytes = 0;
    for (const auto& paragraph : recovery.document.paragraphs()) {
        if (paragraph.text().size() > kMaximumCodeUnits - totalCodeUnits) {
            error = "Recovery document text exceeds the size limit";
            return std::nullopt;
        }
        totalCodeUnits += paragraph.text().size();
        if (paragraph.equations().size() > kMaximumEquations - totalEquations) {
            error = "Recovery document has too many equations";
            return std::nullopt;
        }
        totalEquations += paragraph.equations().size();
        for (const auto& equation : paragraph.equations()) {
            if (equation.canonical_latex.size() >
                kMaximumEquationSourceBytes - totalEquationBytes) {
                error = "Recovery equation source exceeds the size limit";
                return std::nullopt;
            }
            totalEquationBytes += equation.canonical_latex.size();
        }
    }
    std::size_t totalCells = 0;
    for (const auto& table : recovery.document.tables()) {
        if (table.cells().size() > kMaximumTableCells - totalCells) {
            error = "Recovery document has too many table cells";
            return std::nullopt;
        }
        totalCells += table.cells().size();
        for (const auto& cell : table.cells()) {
            if (cell.text.size() > kMaximumCodeUnits - totalCodeUnits) {
                error = "Recovery document text exceeds the size limit";
                return std::nullopt;
            }
            totalCodeUnits += cell.text.size();
        }
    }
    Json root{{"schema", "docxstudio.recovery"}, {"version", currentVersion}};
    root["page"] = {{"width_points", recovery.page.width_points},
                    {"height_points", recovery.page.height_points},
                    {"margin_top_points", recovery.page.margin_top_points},
                    {"margin_right_points", recovery.page.margin_right_points},
                    {"margin_bottom_points", recovery.page.margin_bottom_points},
                    {"margin_left_points", recovery.page.margin_left_points}};
    root["paragraphs"] = Json::array();
    for (const auto& paragraph : recovery.document.paragraphs()) {
        Json runs = Json::array();
        for (const auto& run : paragraph.characterFormats()) {
            runs.push_back({{"start", run.start}, {"end", run.end},
                            {"format", encodeCharacterFormat(run.format)}});
        }
        Json equations = Json::array();
        for (const auto& equation : paragraph.equations()) {
            equations.push_back({{"id", equation.id.toString()},
                                 {"offset", equation.utf16_offset},
                                 {"latex", equation.canonical_latex},
                                 {"display", equation.display}});
        }
        root["paragraphs"].push_back(
            {{"id", paragraph.id().toString()},
             {"text_utf16", encodeUtf16(paragraph.text())},
             {"format", encodeParagraphFormat(paragraph.format())},
             {"runs", std::move(runs)},
             {"equations", std::move(equations)}});
    }
    root["tables"] = Json::array();
    for (const auto& table : recovery.document.tables()) {
        Json cells = Json::array();
        for (const auto& cell : table.cells()) {
            Json runs = Json::array();
            for (const auto& run : cell.character_formats) {
                runs.push_back({{"start", run.start},
                                {"end", run.end},
                                {"format", encodeCharacterFormat(run.format)}});
            }
            cells.push_back({{"id", cell.id.toString()},
                             {"text_utf16", encodeUtf16(cell.text)},
                             {"format", encodeParagraphFormat(cell.paragraph_format)},
                             {"default_character_format",
                              encodeCharacterFormat(
                                  cell.default_character_format)},
                             {"runs", std::move(runs)}});
        }
        Json encodedTable{{"id", table.id().toString()},
                          {"rows", table.rowCount()},
                          {"columns", table.columnCount()},
                          {"header_row", table.hasHeaderRow()},
                          {"cells", std::move(cells)}};
        encodedTable["style"] = table.style()
            ? Json(tableStyleName(*table.style()))
            : Json(nullptr);
        root["tables"].push_back(std::move(encodedTable));
    }
    root["body_blocks"] = Json::array();
    for (const auto& block : recovery.document.bodyBlocks()) {
        root["body_blocks"].push_back(
            {{"kind", block.kind == core::BodyBlockKind::paragraph
                           ? "paragraph" : "table"},
             {"id", block.id.toString()}});
    }
    try {
        std::string payload = root.dump();
        if (payload.size() > kMaximumPayloadBytes) {
            error = "Encoded recovery snapshot exceeds the size limit";
            return std::nullopt;
        }
        return payload;
    } catch (const std::exception& exception) {
        error = std::string("Could not encode recovery snapshot: ") + exception.what();
        return std::nullopt;
    }
}

std::optional<RecoveryDocument> RecoveryCodec::decode(std::string_view payload,
                                                      std::string& error) {
    error.clear();
    if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
        error = "Recovery snapshot is empty or exceeds the size limit";
        return std::nullopt;
    }
    const Json root = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Recovery snapshot is not valid JSON";
        return std::nullopt;
    }
    try {
        const int version = root.value("version", 0);
        if (root.value("schema", std::string{}) != "docxstudio.recovery" ||
            version < 1 || version > currentVersion) {
            error = "Recovery snapshot uses an unsupported format version";
            return std::nullopt;
        }
        const auto& pageJson = root.at("page");
        RecoveryPageLayout page{pageJson.at("width_points").get<double>(),
                                pageJson.at("height_points").get<double>(),
                                pageJson.at("margin_top_points").get<double>(),
                                pageJson.at("margin_right_points").get<double>(),
                                pageJson.at("margin_bottom_points").get<double>(),
                                pageJson.at("margin_left_points").get<double>()};
        if (!validPage(page)) {
            error = "Recovery page dimensions or margins are invalid";
            return std::nullopt;
        }
        const auto& paragraphJson = root.at("paragraphs");
        if (!paragraphJson.is_array() || paragraphJson.empty() ||
            paragraphJson.size() > kMaximumParagraphs) {
            error = "Recovery paragraph list is invalid";
            return std::nullopt;
        }

        struct PendingRun {
            core::NodeId paragraph_id;
            std::size_t start{};
            std::size_t end{};
            core::CharacterFormat format;
        };
        struct PendingParagraphFormat {
            core::NodeId paragraph_id;
            core::ParagraphFormat format;
        };
        struct PendingEquation {
            core::NodeId paragraph_id;
            core::NodeId equation_id;
            std::size_t offset{};
            std::string latex;
            bool display{false};
        };
        std::vector<core::Paragraph> paragraphs;
        std::vector<PendingRun> runs;
        std::vector<PendingParagraphFormat> paragraphFormats;
        std::vector<PendingEquation> equations;
        paragraphs.reserve(paragraphJson.size());
        std::size_t totalCodeUnits = 0;
        std::size_t totalEquations = 0;
        std::size_t totalEquationBytes = 0;
        for (const auto& encodedParagraph : paragraphJson) {
            if (!encodedParagraph.is_object()) {
                error = "Recovery paragraph is not an object";
                return std::nullopt;
            }
            const auto id = core::NodeId::parse(encodedParagraph.at("id").get<std::string>());
            if (!id) {
                error = "Recovery paragraph has an invalid NodeId";
                return std::nullopt;
            }
            std::u16string text;
            if (!decodeUtf16(encodedParagraph.at("text_utf16"), text,
                             totalCodeUnits, error)) {
                return std::nullopt;
            }

            std::vector<PendingEquation> paragraphEquations;
            if (version >= 2) {
                const auto& encodedEquations = encodedParagraph.at("equations");
                if (!encodedEquations.is_array() ||
                    encodedEquations.size() > kMaximumEquations - totalEquations) {
                    error = "Recovery equation list is invalid";
                    return std::nullopt;
                }
                totalEquations += encodedEquations.size();
                paragraphEquations.reserve(encodedEquations.size());
                for (const auto& encodedEquation : encodedEquations) {
                    if (!encodedEquation.is_object()) {
                        error = "Recovery equation is not an object";
                        return std::nullopt;
                    }
                    const auto equationId = core::NodeId::parse(
                        encodedEquation.at("id").get<std::string>());
                    const auto offset64 = encodedEquation.at("offset").get<std::uint64_t>();
                    const auto latex = encodedEquation.at("latex").get<std::string>();
                    const bool display = encodedEquation.value("display", false);
                    if (!equationId ||
                        offset64 > std::numeric_limits<std::size_t>::max() ||
                        offset64 >= text.size() ||
                        text[static_cast<std::size_t>(offset64)] !=
                            core::kInlineObjectReplacementCharacter ||
                        latex.size() >
                            kMaximumEquationSourceBytes - totalEquationBytes) {
                        error = "Recovery equation metadata is invalid";
                        return std::nullopt;
                    }
                    totalEquationBytes += latex.size();
                    paragraphEquations.push_back(
                        {*id, *equationId, static_cast<std::size_t>(offset64),
                         latex, display});
                }
                std::sort(
                    paragraphEquations.begin(), paragraphEquations.end(),
                    [](const PendingEquation& left, const PendingEquation& right) {
                        return left.offset < right.offset;
                    });
                for (std::size_t index = 1; index < paragraphEquations.size(); ++index) {
                    if (paragraphEquations[index - 1].offset ==
                        paragraphEquations[index].offset) {
                        error = "Recovery equations share a placeholder";
                        return std::nullopt;
                    }
                }
            }

            const auto placeholderCount = static_cast<std::size_t>(std::count(
                text.begin(), text.end(), core::kInlineObjectReplacementCharacter));
            if (placeholderCount != paragraphEquations.size()) {
                error = "Recovery paragraph has orphan equation placeholders";
                return std::nullopt;
            }
            std::u16string plainText;
            plainText.reserve(text.size() - placeholderCount);
            std::copy_if(text.begin(), text.end(), std::back_inserter(plainText),
                         [](char16_t value) {
                             return value != core::kInlineObjectReplacementCharacter;
                         });
            auto paragraph = core::Paragraph::create(std::move(plainText), *id);
            if (!paragraph) {
                error = paragraph.error().message;
                return std::nullopt;
            }
            paragraphs.push_back(std::move(paragraph.value()));
            equations.insert(equations.end(),
                             std::make_move_iterator(paragraphEquations.begin()),
                             std::make_move_iterator(paragraphEquations.end()));

            core::ParagraphFormat paragraphFormat;
            if (!decodeParagraphFormat(encodedParagraph.at("format"), paragraphFormat, error))
                return std::nullopt;
            paragraphFormats.push_back({*id, std::move(paragraphFormat)});

            const auto& encodedRuns = encodedParagraph.at("runs");
            if (!encodedRuns.is_array()) {
                error = "Recovery format runs are not an array";
                return std::nullopt;
            }
            for (const auto& encodedRun : encodedRuns) {
                const auto start64 = encodedRun.at("start").get<std::uint64_t>();
                const auto end64 = encodedRun.at("end").get<std::uint64_t>();
                if (start64 > std::numeric_limits<std::size_t>::max() ||
                    end64 > std::numeric_limits<std::size_t>::max()) {
                    error = "Recovery format run is too large";
                    return std::nullopt;
                }
                core::CharacterFormat format;
                if (!decodeCharacterFormat(encodedRun.at("format"), format, error))
                    return std::nullopt;
                runs.push_back({*id, static_cast<std::size_t>(start64),
                                static_cast<std::size_t>(end64), std::move(format)});
            }
        }

        auto document = core::Document::create(std::move(paragraphs));
        if (!document) {
            error = document.error().message;
            return std::nullopt;
        }
        for (auto& equation : equations) {
            const auto inserted = document.value().insertEquation(
                {equation.paragraph_id, equation.offset},
                std::move(equation.latex), equation.display,
                equation.equation_id);
            if (!inserted) {
                error = inserted.error().message;
                return std::nullopt;
            }
        }
        for (const auto& paragraphFormat : paragraphFormats) {
            const auto applied = document.value().applyParagraphFormat(
                {paragraphFormat.paragraph_id}, completeDelta(paragraphFormat.format));
            if (!applied) {
                error = applied.error().message;
                return std::nullopt;
            }
        }
        for (const auto& run : runs) {
            const auto applied = document.value().applyCharacterFormat(
                {{run.paragraph_id, run.start}, {run.paragraph_id, run.end}},
                completeDelta(run.format));
            if (!applied) {
                error = applied.error().message;
                return std::nullopt;
            }
        }

        if (version >= 2) {
            const auto& encodedTables = root.at("tables");
            if (!encodedTables.is_array() ||
                encodedTables.size() > kMaximumTables) {
                error = "Recovery table list is invalid";
                return std::nullopt;
            }
            std::vector<std::pair<core::NodeId, core::Table>> tables;
            std::unordered_set<core::NodeId, core::NodeIdHash> tableIds;
            tables.reserve(encodedTables.size());
            std::size_t totalCells = 0;
            for (const auto& encodedTable : encodedTables) {
                if (!encodedTable.is_object()) {
                    error = "Recovery table is not an object";
                    return std::nullopt;
                }
                const auto id = core::NodeId::parse(
                    encodedTable.at("id").get<std::string>());
                const auto rows64 = encodedTable.at("rows").get<std::uint64_t>();
                const auto columns64 = encodedTable.at("columns").get<std::uint64_t>();
                if (!id || rows64 > std::numeric_limits<std::size_t>::max() ||
                    columns64 > std::numeric_limits<std::size_t>::max()) {
                    error = "Recovery table metadata is invalid";
                    return std::nullopt;
                }
                const auto rows = static_cast<std::size_t>(rows64);
                const auto columns = static_cast<std::size_t>(columns64);
                const auto& encodedCells = encodedTable.at("cells");
                if (!encodedCells.is_array() ||
                    encodedCells.size() > kMaximumTableCells - totalCells) {
                    error = "Recovery table cell list is invalid";
                    return std::nullopt;
                }
                totalCells += encodedCells.size();
                std::vector<core::TableCell> cells;
                cells.reserve(encodedCells.size());
                for (const auto& encodedCell : encodedCells) {
                    const auto cellId = core::NodeId::parse(
                        encodedCell.at("id").get<std::string>());
                    std::u16string cellText;
                    if (!cellId ||
                        !decodeUtf16(encodedCell.at("text_utf16"), cellText,
                                     totalCodeUnits, error)) {
                        if (error.empty()) {
                            error = "Recovery table cell has an invalid NodeId";
                        }
                        return std::nullopt;
                    }
                    core::ParagraphFormat cellParagraphFormat;
                    core::CharacterFormat cellDefaultCharacterFormat;
                    std::vector<core::FormatRun> cellRuns;
                    if (version >= 4) {
                        const auto format = encodedCell.find("format");
                        if (format == encodedCell.end() ||
                            !decodeParagraphFormat(*format, cellParagraphFormat,
                                                   error)) {
                            if (error.empty()) {
                                error = "Recovery table cell format is missing";
                            }
                            return std::nullopt;
                        }
                        const auto defaultFormat =
                            encodedCell.find("default_character_format");
                        if (defaultFormat != encodedCell.end() &&
                            !decodeCharacterFormat(
                                *defaultFormat, cellDefaultCharacterFormat,
                                error)) {
                            return std::nullopt;
                        }
                        const auto encodedRuns = encodedCell.find("runs");
                        if (encodedRuns == encodedCell.end() ||
                            !encodedRuns->is_array()) {
                            error = "Recovery table cell format runs are not an array";
                            return std::nullopt;
                        }
                        cellRuns.reserve(encodedRuns->size());
                        for (const auto& encodedRun : *encodedRuns) {
                            if (!encodedRun.is_object()) {
                                error = "Recovery table cell format run is not an object";
                                return std::nullopt;
                            }
                            const auto start64 =
                                encodedRun.at("start").get<std::uint64_t>();
                            const auto end64 =
                                encodedRun.at("end").get<std::uint64_t>();
                            if (start64 > std::numeric_limits<std::size_t>::max() ||
                                end64 > std::numeric_limits<std::size_t>::max()) {
                                error = "Recovery table cell format run is too large";
                                return std::nullopt;
                            }
                            core::CharacterFormat cellFormat;
                            if (!decodeCharacterFormat(encodedRun.at("format"),
                                                       cellFormat, error)) {
                                return std::nullopt;
                            }
                            cellRuns.push_back(
                                {static_cast<std::size_t>(start64),
                                 static_cast<std::size_t>(end64),
                                 std::move(cellFormat)});
                        }
                    }
                    cells.push_back({*cellId, std::move(cellText),
                                     std::move(cellRuns),
                                     std::move(cellParagraphFormat),
                                     std::move(cellDefaultCharacterFormat)});
                }
                std::optional<core::TableStyle> tableStyle =
                    core::TableStyle::grid;
                if (version >= 4) {
                    const auto style = encodedTable.find("style");
                    if (style == encodedTable.end()) {
                        error = "Recovery table style is missing";
                        return std::nullopt;
                    }
                    if (style->is_null()) {
                        tableStyle.reset();
                    } else if (style->is_string()) {
                        tableStyle = parseTableStyle(style->get<std::string>());
                        if (!tableStyle) {
                            error = "Recovery table style is unknown";
                            return std::nullopt;
                        }
                    } else {
                        error = "Recovery table style is not a string or null";
                        return std::nullopt;
                    }
                }
                auto restored = core::Table::restore(
                    rows, columns, encodedTable.value("header_row", false),
                    *id, std::move(cells), tableStyle);
                if (!restored || !tableIds.insert(*id).second) {
                    error = restored ? "Recovery table IDs are not unique"
                                     : restored.error().message;
                    return std::nullopt;
                }
                tables.emplace_back(*id, std::move(restored.value()));
            }

            const auto& encodedBlocks = root.at("body_blocks");
            if (!encodedBlocks.is_array() ||
                encodedBlocks.size() !=
                    document.value().paragraphs().size() + tables.size()) {
                error = "Recovery body-block order is invalid";
                return std::nullopt;
            }
            struct PendingBlock {
                core::BodyBlockKind kind{core::BodyBlockKind::paragraph};
                core::NodeId id;
            };
            std::vector<PendingBlock> blocks;
            blocks.reserve(encodedBlocks.size());
            std::unordered_set<core::NodeId, core::NodeIdHash> seenBlocks;
            std::size_t paragraphOrder = 0;
            for (const auto& encodedBlock : encodedBlocks) {
                const auto id = core::NodeId::parse(
                    encodedBlock.at("id").get<std::string>());
                const auto kind = encodedBlock.at("kind").get<std::string>();
                if (!id || !seenBlocks.insert(*id).second) {
                    error = "Recovery body block has an invalid or duplicate ID";
                    return std::nullopt;
                }
                if (kind == "paragraph") {
                    if (paragraphOrder >= document.value().paragraphs().size() ||
                        document.value().paragraphs()[paragraphOrder].id() != *id) {
                        error = "Recovery paragraph order is invalid";
                        return std::nullopt;
                    }
                    ++paragraphOrder;
                    blocks.push_back({core::BodyBlockKind::paragraph, *id});
                } else if (kind == "table" && tableIds.contains(*id)) {
                    blocks.push_back({core::BodyBlockKind::table, *id});
                } else {
                    error = "Recovery body block kind or ID is invalid";
                    return std::nullopt;
                }
            }
            if (paragraphOrder != document.value().paragraphs().size()) {
                error = "Recovery body order omits a paragraph";
                return std::nullopt;
            }
            for (auto& entry : tables) {
                const auto inserted = document.value().insertTable(
                    std::nullopt, std::move(entry.second));
                if (!inserted) {
                    error = inserted.error().message;
                    return std::nullopt;
                }
            }
            for (std::size_t index = blocks.size(); index-- > 0;) {
                if (blocks[index].kind != core::BodyBlockKind::table) continue;
                const std::optional<core::NodeId> before =
                    index + 1 < blocks.size()
                        ? std::optional<core::NodeId>(blocks[index + 1].id)
                        : std::nullopt;
                const auto moved = document.value().moveTable(
                    blocks[index].id, before);
                if (!moved) {
                    error = moved.error().message;
                    return std::nullopt;
                }
            }
        }
        return RecoveryDocument{std::move(document.value()), page};
    } catch (const std::exception& exception) {
        error = std::string("Recovery snapshot is malformed: ") + exception.what();
        return std::nullopt;
    }
}

}  // namespace docxstudio::app
