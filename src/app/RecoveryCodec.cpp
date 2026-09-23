#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/raster/validation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <span>
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
constexpr std::size_t kMaximumJsonNestingDepth = 64U;
constexpr std::size_t kMaximumJsonStructuralElements =
    16U * 1024U * 1024U;
constexpr std::size_t kMaximumJsonStringBytes = kMaximumPayloadBytes;
// Valid core formatting runs are non-empty and non-overlapping, so their
// aggregate count cannot exceed the document's aggregate UTF-16 length.
constexpr std::size_t kMaximumFormatRuns = kMaximumCodeUnits;
constexpr double kEmuPerPoint = 12'700.0;

class RecoveryJsonPreflight final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return beginValue(); }
    bool boolean(bool /*value*/) override { return beginValue(); }
    bool number_integer(number_integer_t /*value*/) override {
        return beginValue();
    }
    bool number_unsigned(number_unsigned_t /*value*/) override {
        return beginValue();
    }
    bool number_float(number_float_t /*value*/,
                      const string_t& /*token*/) override {
        return beginValue();
    }
    bool string(string_t& value) override {
        return beginValue() && countString(value.size());
    }
    bool binary(binary_t& value) override {
        return beginValue() && countString(value.size());
    }

    bool start_object(std::size_t /*elements*/) override {
        bool runsValue = false;
        if (!beginValue(&runsValue)) return false;
        if (frames_.size() >= kMaximumJsonNestingDepth) {
            error_ = "Recovery snapshot exceeds the JSON nesting limit";
            return false;
        }
        frames_.push_back({ContainerKind::object, false, false});
        return true;
    }

    bool key(string_t& value) override {
        if (frames_.empty() ||
            frames_.back().kind != ContainerKind::object) {
            error_ = "Recovery snapshot has invalid JSON structure";
            return false;
        }
        if (!countElement() || !countString(value.size())) return false;
        frames_.back().pending_runs_value = value == "runs";
        return true;
    }

    bool end_object() override {
        return endContainer(ContainerKind::object);
    }

    bool start_array(std::size_t /*elements*/) override {
        bool runsValue = false;
        if (!beginValue(&runsValue)) return false;
        if (frames_.size() >= kMaximumJsonNestingDepth) {
            error_ = "Recovery snapshot exceeds the JSON nesting limit";
            return false;
        }
        frames_.push_back({ContainerKind::array, runsValue, false});
        return true;
    }

    bool end_array() override {
        return endContainer(ContainerKind::array);
    }

    bool parse_error(std::size_t /*position*/,
                     const std::string& /*lastToken*/,
                     const nlohmann::detail::exception& /*exception*/) override {
        if (error_.empty()) {
            error_ = "Recovery snapshot is not valid JSON";
        }
        return false;
    }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    enum class ContainerKind { object, array };

    struct Frame {
        ContainerKind kind{ContainerKind::object};
        bool format_runs_array{false};
        bool pending_runs_value{false};
    };

    bool beginValue(bool* runsValue = nullptr) {
        if (!countElement()) return false;
        bool keyedRunsValue = false;
        if (!frames_.empty()) {
            auto& parent = frames_.back();
            if (parent.kind == ContainerKind::object) {
                keyedRunsValue = parent.pending_runs_value;
                parent.pending_runs_value = false;
            } else if (parent.format_runs_array) {
                if (format_run_count_ >= kMaximumFormatRuns) {
                    error_ =
                        "Recovery snapshot exceeds the formatting-run limit";
                    return false;
                }
                ++format_run_count_;
            }
        }
        if (runsValue) *runsValue = keyedRunsValue;
        return true;
    }

    bool countElement() {
        if (structural_element_count_ >=
            kMaximumJsonStructuralElements) {
            error_ = "Recovery snapshot exceeds the JSON element limit";
            return false;
        }
        ++structural_element_count_;
        return true;
    }

    bool countString(std::size_t bytes) {
        if (bytes > kMaximumJsonStringBytes - json_string_bytes_) {
            error_ = "Recovery snapshot exceeds the JSON string-byte limit";
            return false;
        }
        json_string_bytes_ += bytes;
        return true;
    }

    bool endContainer(ContainerKind expected) {
        if (frames_.empty() || frames_.back().kind != expected) {
            error_ = "Recovery snapshot has invalid JSON structure";
            return false;
        }
        frames_.pop_back();
        return true;
    }

    std::vector<Frame> frames_;
    std::size_t structural_element_count_{0};
    std::size_t json_string_bytes_{0};
    std::size_t format_run_count_{0};
    std::string error_;
};

bool preflightRecoveryJson(std::string_view payload, std::string& error) {
    RecoveryJsonPreflight preflight;
    try {
        if (!Json::sax_parse(payload.begin(), payload.end(), &preflight)) {
            error = preflight.error().empty()
                ? "Recovery snapshot is not valid JSON"
                : preflight.error();
            return false;
        }
    } catch (const std::exception& exception) {
        error = std::string("Could not preflight recovery snapshot: ") +
            exception.what();
        return false;
    }
    return true;
}

std::string base64Encode(std::span<const std::uint8_t> input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    while (index + 3U <= input.size()) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(input[index]) << 16U) |
            (static_cast<std::uint32_t>(input[index + 1U]) << 8U) |
            static_cast<std::uint32_t>(input[index + 2U]);
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(alphabet[(value >> 6U) & 0x3fU]);
        output.push_back(alphabet[value & 0x3fU]);
        index += 3U;
    }
    const std::size_t remaining = input.size() - index;
    if (remaining == 1U) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(input[index]) << 16U;
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output += "==";
    } else if (remaining == 2U) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(input[index]) << 16U) |
            (static_cast<std::uint32_t>(input[index + 1U]) << 8U);
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(alphabet[(value >> 6U) & 0x3fU]);
        output.push_back('=');
    }
    return output;
}

int base64Value(char character) noexcept {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

std::optional<std::vector<std::uint8_t>> base64Decode(
    std::string_view input, std::size_t maximumBytes) {
    if (input.empty() || input.size() % 4U != 0U) return std::nullopt;
    const std::size_t padding =
        (input.back() == '=' ? 1U : 0U) +
        (input.size() >= 2U && input[input.size() - 2U] == '=' ? 1U : 0U);
    if (input.size() / 4U >
        (std::numeric_limits<std::size_t>::max() - 2U) / 3U) {
        return std::nullopt;
    }
    const std::size_t decodedSize = input.size() / 4U * 3U - padding;
    if (decodedSize == 0U || decodedSize > maximumBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> output;
    output.reserve(decodedSize);
    for (std::size_t index = 0; index < input.size(); index += 4U) {
        const bool finalGroup = index + 4U == input.size();
        const int first = base64Value(input[index]);
        const int second = base64Value(input[index + 1U]);
        const bool thirdPadding = input[index + 2U] == '=';
        const bool fourthPadding = input[index + 3U] == '=';
        const int third = thirdPadding ? 0 : base64Value(input[index + 2U]);
        const int fourth = fourthPadding ? 0 : base64Value(input[index + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (!finalGroup && (thirdPadding || fourthPadding)) ||
            (thirdPadding && !fourthPadding) ||
            (thirdPadding && (second & 0x0f) != 0) ||
            (fourthPadding && !thirdPadding && (third & 0x03) != 0)) {
            return std::nullopt;
        }
        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 18U) |
            (static_cast<std::uint32_t>(second) << 12U) |
            (static_cast<std::uint32_t>(third) << 6U) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<std::uint8_t>(value >> 16U));
        if (!thirdPadding) {
            output.push_back(static_cast<std::uint8_t>(value >> 8U));
        }
        if (!fourthPadding) {
            output.push_back(static_cast<std::uint8_t>(value));
        }
    }
    if (output.size() != decodedSize) return std::nullopt;
    return output;
}

const char* imageFormatName(core::ImageFormat format) noexcept {
    if (format == core::ImageFormat::png) return "png";
    if (format == core::ImageFormat::jpeg) return "jpeg";
    return "unknown";
}

std::optional<core::ImageFormat> parseImageFormat(std::string_view value) {
    if (value == "png") return core::ImageFormat::png;
    if (value == "jpeg") return core::ImageFormat::jpeg;
    return std::nullopt;
}

const char* imagePlacementName(core::ImagePlacement placement) noexcept {
    switch (placement) {
        case core::ImagePlacement::inline_with_text:
            return "inline";
        case core::ImagePlacement::square:
            return "square";
        case core::ImagePlacement::top_and_bottom:
            return "top-and-bottom";
    }
    return "unknown";
}

std::optional<core::ImagePlacement> parseImagePlacement(
    std::string_view value) {
    if (value == "inline") return core::ImagePlacement::inline_with_text;
    if (value == "square") return core::ImagePlacement::square;
    if (value == "top-and-bottom") {
        return core::ImagePlacement::top_and_bottom;
    }
    return std::nullopt;
}

Json encodeImageLayout(const core::ImageLayout& layout) {
    return {{"placement", imagePlacementName(layout.placement)},
            {"distance_top_emu", layout.distance_top_emu},
            {"distance_right_emu", layout.distance_right_emu},
            {"distance_bottom_emu", layout.distance_bottom_emu},
            {"distance_left_emu", layout.distance_left_emu},
            {"move_with_text", layout.move_with_text}};
}

bool readImageDistance(const Json& input, const char* key,
                       std::int64_t& value, std::string& error) {
    const auto found = input.find(key);
    if (found == input.end()) return true;
    if (found->is_number_unsigned()) {
        const auto encoded = found->get<std::uint64_t>();
        if (encoded > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
            error = std::string("Recovery image-layout field '") + key +
                "' is out of range";
            return false;
        }
        value = static_cast<std::int64_t>(encoded);
        return true;
    }
    if (found->is_number_integer()) {
        value = found->get<std::int64_t>();
        return true;
    }
    error = std::string("Recovery image-layout field '") + key +
        "' is not an integer";
    return false;
}

bool decodeImageLayout(const Json& input, core::ImageLayout& layout,
                       std::string& error) {
    if (!input.is_object()) {
        error = "Recovery image layout is not an object";
        return false;
    }
    if (const auto found = input.find("placement"); found != input.end()) {
        if (!found->is_string()) {
            error = "Recovery image placement is not a string";
            return false;
        }
        const auto parsed = parseImagePlacement(found->get<std::string>());
        if (!parsed) {
            error = "Recovery image placement is unknown";
            return false;
        }
        layout.placement = *parsed;
    }
    if (!readImageDistance(
            input, "distance_top_emu", layout.distance_top_emu, error) ||
        !readImageDistance(
            input, "distance_right_emu", layout.distance_right_emu, error) ||
        !readImageDistance(
            input, "distance_bottom_emu", layout.distance_bottom_emu, error) ||
        !readImageDistance(
            input, "distance_left_emu", layout.distance_left_emu, error)) {
        return false;
    }
    if (const auto found = input.find("move_with_text");
        found != input.end()) {
        if (!found->is_boolean()) {
            error = "Recovery image move-with-text value is not Boolean";
            return false;
        }
        layout.move_with_text = found->get<bool>();
    }
    const auto validation = layout.validate();
    if (!validation) {
        error = validation.error().message;
        return false;
    }
    return true;
}

raster::Format rasterFormat(core::ImageFormat format) noexcept {
    if (format == core::ImageFormat::png) return raster::Format::png;
    if (format == core::ImageFormat::jpeg) return raster::Format::jpeg;
    return raster::Format::unknown;
}

bool validEncodedImage(const core::EncodedImagePayload& payload,
                       core::ImageFormat format) {
    if (payload.empty() ||
        payload.size() > core::kMaximumEncodedImageBytes ||
        rasterFormat(format) == raster::Format::unknown) {
        return false;
    }
    raster::ValidationLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    return raster::inspect(payload.bytes(), rasterFormat(format), limits).ok();
}

std::optional<std::int64_t> pointsToEmu(double points) {
    if (!std::isfinite(points) || points <= 0.0 ||
        points > static_cast<double>(
                     core::kMaximumInlineImageDimensionEmu) /
                     kEmuPerPoint) {
        return std::nullopt;
    }
    const auto emu = static_cast<std::int64_t>(
        std::llround(points * kEmuPerPoint));
    if (emu <= 0 || emu > core::kMaximumInlineImageDimensionEmu) {
        return std::nullopt;
    }
    return emu;
}

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

Json encodeCharacterFormatMask(const core::CharacterFormatMask& mask) {
    Json output = Json::object();
    if (mask.font_family) output["font_family"] = true;
    if (mask.font_size_half_points) output["font_size_half_points"] = true;
    if (mask.bold) output["bold"] = true;
    if (mask.italic) output["italic"] = true;
    if (mask.underline) output["underline"] = true;
    if (mask.strike) output["strike"] = true;
    if (mask.foreground_argb) output["foreground_argb"] = true;
    if (mask.highlight_argb) output["highlight_argb"] = true;
    if (mask.baseline) output["baseline"] = true;
    if (mask.language) output["language"] = true;
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

Json encodeParagraphFormatMask(const core::ParagraphFormatMask& mask) {
    Json output = Json::object();
    if (mask.alignment) output["alignment"] = true;
    if (mask.left_indent_emu) output["left_indent_emu"] = true;
    if (mask.right_indent_emu) output["right_indent_emu"] = true;
    if (mask.first_line_indent_emu) output["first_line_indent_emu"] = true;
    if (mask.space_before_emu) output["space_before_emu"] = true;
    if (mask.space_after_emu) output["space_after_emu"] = true;
    if (mask.line_spacing_emu) output["line_spacing_emu"] = true;
    if (mask.line_spacing_rule) output["line_spacing_rule"] = true;
    if (mask.keep_with_next) output["keep_with_next"] = true;
    if (mask.keep_lines) output["keep_lines"] = true;
    if (mask.page_break_before) output["page_break_before"] = true;
    return output;
}

bool decodeMaskProperty(const Json& input, const char* name, bool& output,
                        std::string& error) {
    const auto found = input.find(name);
    if (found == input.end()) return true;
    if (!found->is_boolean()) {
        error = std::string("Recovery style-provenance mask property '") +
                name + "' is not a boolean";
        return false;
    }
    output = found->get<bool>();
    return true;
}

bool decodeCharacterFormatMask(const Json& input,
                               core::CharacterFormatMask& mask,
                               std::string& error) {
    if (!input.is_object()) {
        error = "Recovery character style-provenance mask is not an object";
        return false;
    }
    return decodeMaskProperty(input, "font_family", mask.font_family, error) &&
           decodeMaskProperty(input, "font_size_half_points",
                              mask.font_size_half_points, error) &&
           decodeMaskProperty(input, "bold", mask.bold, error) &&
           decodeMaskProperty(input, "italic", mask.italic, error) &&
           decodeMaskProperty(input, "underline", mask.underline, error) &&
           decodeMaskProperty(input, "strike", mask.strike, error) &&
           decodeMaskProperty(input, "foreground_argb",
                              mask.foreground_argb, error) &&
           decodeMaskProperty(input, "highlight_argb",
                              mask.highlight_argb, error) &&
           decodeMaskProperty(input, "baseline", mask.baseline, error) &&
           decodeMaskProperty(input, "language", mask.language, error);
}

bool decodeParagraphFormatMask(const Json& input,
                               core::ParagraphFormatMask& mask,
                               std::string& error) {
    if (!input.is_object()) {
        error = "Recovery paragraph style-provenance mask is not an object";
        return false;
    }
    return decodeMaskProperty(input, "alignment", mask.alignment, error) &&
           decodeMaskProperty(input, "left_indent_emu",
                              mask.left_indent_emu, error) &&
           decodeMaskProperty(input, "right_indent_emu",
                              mask.right_indent_emu, error) &&
           decodeMaskProperty(input, "first_line_indent_emu",
                              mask.first_line_indent_emu, error) &&
           decodeMaskProperty(input, "space_before_emu",
                              mask.space_before_emu, error) &&
           decodeMaskProperty(input, "space_after_emu",
                              mask.space_after_emu, error) &&
           decodeMaskProperty(input, "line_spacing_emu",
                              mask.line_spacing_emu, error) &&
           decodeMaskProperty(input, "line_spacing_rule",
                              mask.line_spacing_rule, error) &&
           decodeMaskProperty(input, "keep_with_next",
                              mask.keep_with_next, error) &&
           decodeMaskProperty(input, "keep_lines", mask.keep_lines, error) &&
           decodeMaskProperty(input, "page_break_before",
                              mask.page_break_before, error);
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
    std::size_t totalFormatRuns = 0;
    std::size_t totalImages = 0;
    std::size_t totalImageBytes = 0;
    std::unordered_set<core::NodeId, core::NodeIdHash> imageIds;
    for (const auto& paragraph : recovery.document.paragraphs()) {
        if (paragraph.styleId()) {
            const auto styleValidation =
                core::validateParagraphStyleId(*paragraph.styleId());
            if (!styleValidation) {
                error = styleValidation.error().message;
                return std::nullopt;
            }
        }
        if (paragraph.styleProvenance()) {
            if (!paragraph.styleId()) {
                error = "Paragraph style provenance requires a style ID";
                return std::nullopt;
            }
            const auto& provenance = *paragraph.styleProvenance();
            const auto characterValidation =
                provenance.inherited_character_format.validate();
            const auto markCharacterValidation =
                provenance.inherited_paragraph_mark_character_format
                    .validate();
            const auto paragraphValidation =
                provenance.inherited_paragraph_format.validate();
            if (!characterValidation || !markCharacterValidation ||
                !paragraphValidation) {
                error = !characterValidation
                    ? characterValidation.error().message
                    : !markCharacterValidation
                    ? markCharacterValidation.error().message
                    : paragraphValidation.error().message;
                return std::nullopt;
            }
            if (provenance.character_overrides.size() >
                kMaximumFormatRuns - totalFormatRuns) {
                error = "Recovery document has too many formatting runs";
                return std::nullopt;
            }
            totalFormatRuns += provenance.character_overrides.size();
        }
        const auto markFormatValidation =
            paragraph.paragraphMarkCharacterFormat().validate();
        if (!markFormatValidation) {
            error = markFormatValidation.error().message;
            return std::nullopt;
        }
        if (paragraph.text().size() > kMaximumCodeUnits - totalCodeUnits) {
            error = "Recovery document text exceeds the size limit";
            return std::nullopt;
        }
        totalCodeUnits += paragraph.text().size();
        if (paragraph.characterFormats().size() >
            kMaximumFormatRuns - totalFormatRuns) {
            error = "Recovery document has too many formatting runs";
            return std::nullopt;
        }
        totalFormatRuns += paragraph.characterFormats().size();
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
        if (paragraph.images().size() >
            core::kMaximumInlineImagesPerDocument - totalImages) {
            error = "Recovery document has too many inline pictures";
            return std::nullopt;
        }
        totalImages += paragraph.images().size();
        for (const auto& image : paragraph.images()) {
            if (!image.id.isValid() || !imageIds.insert(image.id).second ||
                image.utf16_offset >= paragraph.text().size() ||
                paragraph.text()[image.utf16_offset] !=
                    core::kInlineObjectReplacementCharacter ||
                image.accessible_name.size() >
                    core::kMaximumImageAccessibleNameBytes ||
                image.width_emu <= 0 || image.height_emu <= 0 ||
                image.width_emu > core::kMaximumInlineImageDimensionEmu ||
                image.height_emu > core::kMaximumInlineImageDimensionEmu ||
                image.encoded_payload.size() >
                    core::kMaximumDocumentEncodedImageBytes - totalImageBytes ||
                !image.layout.validate() ||
                !validEncodedImage(image.encoded_payload, image.format)) {
                error =
                    "Recovery inline-picture metadata or payload is invalid";
                return std::nullopt;
            }
            totalImageBytes += image.encoded_payload.size();
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
            if (cell.character_formats.size() >
                kMaximumFormatRuns - totalFormatRuns) {
                error = "Recovery document has too many formatting runs";
                return std::nullopt;
            }
            totalFormatRuns += cell.character_formats.size();
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
        Json images = Json::array();
        for (const auto& image : paragraph.images()) {
            images.push_back(
                {{"id", image.id.toString()},
                 {"offset", image.utf16_offset},
                 {"width_emu", image.width_emu},
                 {"height_emu", image.height_emu},
                 {"accessible_name", image.accessible_name},
                 {"layout", encodeImageLayout(image.layout)},
                 {"format", imageFormatName(image.format)},
                 {"encoded_base64", base64Encode(image.encoded_payload.bytes())}});
        }
        Json encodedParagraph{
            {"id", paragraph.id().toString()},
            {"text_utf16", encodeUtf16(paragraph.text())},
            {"format", encodeParagraphFormat(paragraph.format())},
            {"paragraph_mark_character_format",
             encodeCharacterFormat(
                 paragraph.paragraphMarkCharacterFormat())},
            {"runs", std::move(runs)},
            {"equations", std::move(equations)},
            {"images", std::move(images)}};
        if (paragraph.styleId()) {
            encodedParagraph["style_id"] = *paragraph.styleId();
        }
        if (paragraph.styleProvenance()) {
            const auto& provenance = *paragraph.styleProvenance();
            Json overrides = Json::array();
            for (const auto& run : provenance.character_overrides) {
                overrides.push_back(
                    {{"start", run.start},
                     {"end", run.end},
                     {"properties",
                      encodeCharacterFormatMask(run.properties)}});
            }
            encodedParagraph["style_provenance"] = {
                {"inherited_character_format",
                 encodeCharacterFormat(
                     provenance.inherited_character_format)},
                {"inherited_paragraph_mark_character_format",
                 encodeCharacterFormat(
                     provenance
                         .inherited_paragraph_mark_character_format)},
                {"inherited_paragraph_format",
                 encodeParagraphFormat(
                     provenance.inherited_paragraph_format)},
                {"character_overrides", std::move(overrides)},
                {"paragraph_mark_overrides",
                 encodeCharacterFormatMask(
                     provenance.paragraph_mark_overrides)},
                {"paragraph_overrides",
                 encodeParagraphFormatMask(
                     provenance.paragraph_overrides)}};
        }
        root["paragraphs"].push_back(std::move(encodedParagraph));
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
    root["header_text_utf16"] = encodeUtf16(recovery.document.headerText());
    root["footer_text_utf16"] = encodeUtf16(recovery.document.footerText());
    const auto encodeStoryImages = [](const std::vector<core::ImageAtom>& images) {
        Json encoded = Json::array();
        for (const auto& image : images) {
            encoded.push_back(
                {{"id", image.id.toString()},
                 {"offset", image.utf16_offset},
                 {"width_emu", image.width_emu},
                 {"height_emu", image.height_emu},
                 {"accessible_name", image.accessible_name},
                 {"format", imageFormatName(image.format)},
                 {"encoded_base64",
                  base64Encode(image.encoded_payload.bytes())}});
        }
        return encoded;
    };
    root["header_images"] = encodeStoryImages(
        recovery.document.headerImages());
    root["footer_images"] = encodeStoryImages(
        recovery.document.footerImages());
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
    if (!preflightRecoveryJson(payload, error)) return std::nullopt;
    try {
        const Json root = Json::parse(
            payload.begin(), payload.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            error = "Recovery snapshot is not valid JSON";
            return std::nullopt;
        }
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
        struct PendingStyleProvenance {
            core::NodeId paragraph_id;
            core::ParagraphStyleProvenance provenance;
        };
        struct PendingEquation {
            core::NodeId paragraph_id;
            core::NodeId equation_id;
            std::size_t offset{};
            std::string latex;
            bool display{false};
        };
        struct PendingImage {
            core::NodeId paragraph_id;
            core::NodeId image_id;
            std::size_t offset{};
            core::EncodedImagePayload encoded_payload;
            core::ImageFormat format{core::ImageFormat::png};
            std::string accessible_name;
            std::int64_t width_emu{};
            std::int64_t height_emu{};
            core::ImageLayout layout;
            std::size_t source_order{};
        };
        std::vector<core::Paragraph> paragraphs;
        std::vector<PendingRun> runs;
        std::vector<PendingParagraphFormat> paragraphFormats;
        std::vector<PendingStyleProvenance> styleProvenance;
        std::vector<PendingEquation> equations;
        std::vector<PendingImage> images;
        std::unordered_map<core::NodeId, std::vector<std::size_t>,
                           core::NodeIdHash>
            legacyImagesByParagraph;
        paragraphs.reserve(paragraphJson.size());
        std::size_t totalCodeUnits = 0;
        std::size_t totalEquations = 0;
        std::size_t totalEquationBytes = 0;
        std::size_t totalFormatRuns = 0;
        std::size_t totalImages = 0;
        std::size_t totalImageBytes = 0;
        std::unordered_set<core::NodeId, core::NodeIdHash> imageIds;

        // Version 5 stored presentation-only, zero-width image anchors at the
        // root. Read those journals once, then migrate them into semantic image
        // atoms after the original equation-bearing text has been restored.
        if (version == 5) {
            const auto& encodedImages = root.at("inline_images");
            if (!encodedImages.is_array() ||
                encodedImages.size() >
                    core::kMaximumInlineImagesPerDocument) {
                error = "Recovery inline-picture list is invalid";
                return std::nullopt;
            }
            images.reserve(encodedImages.size());
            for (std::size_t sourceOrder = 0;
                 sourceOrder < encodedImages.size(); ++sourceOrder) {
                const auto& encodedImage = encodedImages[sourceOrder];
                if (!encodedImage.is_object()) {
                    error = "Recovery inline picture is not an object";
                    return std::nullopt;
                }
                const auto imageId = core::NodeId::parse(
                    encodedImage.at("id").get<std::string>());
                const auto paragraphId = core::NodeId::parse(
                    encodedImage.at("paragraph_id").get<std::string>());
                const auto offset64 =
                    encodedImage.at("utf16_offset").get<std::uint64_t>();
                const auto width = pointsToEmu(
                    encodedImage.at("width_points").get<double>());
                const auto height = pointsToEmu(
                    encodedImage.at("height_points").get<double>());
                const auto accessibleName =
                    encodedImage.at("accessible_name").get<std::string>();
                const auto format = parseImageFormat(
                    encodedImage.at("format").get<std::string>());
                const auto encodedBase64 =
                    encodedImage.at("encoded_base64").get<std::string>();
                const std::size_t remainingDocumentBytes =
                    core::kMaximumDocumentEncodedImageBytes -
                    totalImageBytes;
                auto bytes = base64Decode(
                    encodedBase64,
                    std::min(core::kMaximumEncodedImageBytes,
                             remainingDocumentBytes));
                if (!imageId || !paragraphId || !format || !width ||
                    !height || !bytes ||
                    !imageIds.insert(*imageId).second ||
                    offset64 > std::numeric_limits<std::size_t>::max() ||
                    accessibleName.size() >
                        core::kMaximumImageAccessibleNameBytes) {
                    error =
                        "Recovery inline-picture metadata or payload is invalid";
                    return std::nullopt;
                }
                core::EncodedImagePayload encodedPayload(std::move(*bytes));
                if (!validEncodedImage(encodedPayload, *format)) {
                    error =
                        "Recovery inline-picture metadata or payload is invalid";
                    return std::nullopt;
                }
                totalImageBytes += encodedPayload.size();
                const std::size_t imageIndex = images.size();
                images.push_back(
                    {*paragraphId, *imageId,
                     static_cast<std::size_t>(offset64),
                     std::move(encodedPayload), *format,
                     std::move(accessibleName), *width, *height,
                     {}, sourceOrder});
                legacyImagesByParagraph[*paragraphId].push_back(imageIndex);
            }
            totalImages = images.size();
        }
        std::size_t seenLegacyImages = 0;
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
            std::vector<PendingImage> paragraphImages;
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

            if (version >= 6) {
                const auto& encodedImages = encodedParagraph.at("images");
                if (!encodedImages.is_array() ||
                    encodedImages.size() >
                        core::kMaximumInlineImagesPerDocument - totalImages) {
                    error = "Recovery inline-picture list is invalid";
                    return std::nullopt;
                }
                totalImages += encodedImages.size();
                paragraphImages.reserve(encodedImages.size());
                for (std::size_t sourceOrder = 0;
                     sourceOrder < encodedImages.size(); ++sourceOrder) {
                    const auto& encodedImage = encodedImages[sourceOrder];
                    if (!encodedImage.is_object()) {
                        error = "Recovery inline picture is not an object";
                        return std::nullopt;
                    }
                    const auto imageId = core::NodeId::parse(
                        encodedImage.at("id").get<std::string>());
                    const auto offset64 =
                        encodedImage.at("offset").get<std::uint64_t>();
                    const auto widthEmu =
                        encodedImage.at("width_emu").get<std::int64_t>();
                    const auto heightEmu =
                        encodedImage.at("height_emu").get<std::int64_t>();
                    const auto accessibleName =
                        encodedImage.at("accessible_name").get<std::string>();
                    const auto format = parseImageFormat(
                        encodedImage.at("format").get<std::string>());
                    const auto encodedBase64 =
                        encodedImage.at("encoded_base64").get<std::string>();
                    core::ImageLayout layout;
                    if (version >= 7) {
                        const auto encodedLayout = encodedImage.find("layout");
                        if (encodedLayout != encodedImage.end() &&
                            !decodeImageLayout(*encodedLayout, layout, error)) {
                            return std::nullopt;
                        }
                    }
                    const std::size_t remainingDocumentBytes =
                        core::kMaximumDocumentEncodedImageBytes -
                        totalImageBytes;
                    auto bytes = base64Decode(
                        encodedBase64,
                        std::min(core::kMaximumEncodedImageBytes,
                                 remainingDocumentBytes));
                    if (!imageId || !format || !bytes ||
                        !imageIds.insert(*imageId).second ||
                        offset64 >
                            std::numeric_limits<std::size_t>::max() ||
                        offset64 >= text.size() ||
                        text[static_cast<std::size_t>(offset64)] !=
                            core::kInlineObjectReplacementCharacter ||
                        accessibleName.size() >
                            core::kMaximumImageAccessibleNameBytes ||
                        widthEmu <= 0 || heightEmu <= 0 ||
                        widthEmu >
                            core::kMaximumInlineImageDimensionEmu ||
                        heightEmu >
                            core::kMaximumInlineImageDimensionEmu) {
                        error =
                            "Recovery inline-picture metadata or payload is invalid";
                        return std::nullopt;
                    }
                    core::EncodedImagePayload encodedPayload(
                        std::move(*bytes));
                    if (!validEncodedImage(encodedPayload, *format)) {
                        error =
                            "Recovery inline-picture metadata or payload is invalid";
                        return std::nullopt;
                    }
                    totalImageBytes += encodedPayload.size();
                    paragraphImages.push_back(
                        {*id, *imageId,
                         static_cast<std::size_t>(offset64),
                         std::move(encodedPayload), *format,
                         std::move(accessibleName), widthEmu, heightEmu,
                         layout, sourceOrder});
                }
                std::sort(
                    paragraphImages.begin(), paragraphImages.end(),
                    [](const PendingImage& left, const PendingImage& right) {
                        return left.offset < right.offset;
                    });
            }

            std::set<std::size_t> typedObjectOffsets;
            for (const auto& equation : paragraphEquations) {
                if (!typedObjectOffsets.insert(equation.offset).second) {
                    error = "Recovery inline objects share a placeholder";
                    return std::nullopt;
                }
            }
            for (const auto& image : paragraphImages) {
                if (!typedObjectOffsets.insert(image.offset).second) {
                    error = "Recovery inline objects share a placeholder";
                    return std::nullopt;
                }
            }

            if (version == 5) {
                const auto legacy = legacyImagesByParagraph.find(*id);
                if (legacy != legacyImagesByParagraph.end()) {
                    for (const std::size_t imageIndex : legacy->second) {
                        if (!core::isUtf16Boundary(
                                text, images[imageIndex].offset)) {
                            error =
                                "Recovery inline-picture anchor is not a UTF-16 boundary";
                            return std::nullopt;
                        }
                        ++seenLegacyImages;
                    }
                }
            }

            const auto placeholderCount = static_cast<std::size_t>(std::count(
                text.begin(), text.end(), core::kInlineObjectReplacementCharacter));
            const std::size_t expectedPlaceholders =
                paragraphEquations.size() + paragraphImages.size();
            if (placeholderCount != expectedPlaceholders) {
                error = "Recovery paragraph has orphan inline-object placeholders";
                return std::nullopt;
            }
            std::u16string plainText;
            plainText.reserve(text.size() - placeholderCount);
            std::copy_if(text.begin(), text.end(), std::back_inserter(plainText),
                         [](char16_t value) {
                             return value != core::kInlineObjectReplacementCharacter;
                         });
            core::CharacterFormat paragraphMarkCharacterFormat;
            if (version >= 8) {
                const auto encodedMarkFormat = encodedParagraph.find(
                    "paragraph_mark_character_format");
                if (encodedMarkFormat != encodedParagraph.end() &&
                    !decodeCharacterFormat(
                        *encodedMarkFormat, paragraphMarkCharacterFormat,
                        error)) {
                    return std::nullopt;
                }
            }
            std::optional<std::string> styleId;
            if (const auto encodedStyleId =
                    encodedParagraph.find("style_id");
                encodedStyleId != encodedParagraph.end()) {
                if (!encodedStyleId->is_string()) {
                    error = "Recovery paragraph style ID is not a string";
                    return std::nullopt;
                }
                const auto& value =
                    encodedStyleId->get_ref<const std::string&>();
                const auto validation =
                    core::validateParagraphStyleId(value);
                if (!validation) {
                    error = validation.error().message;
                    return std::nullopt;
                }
                styleId = value;
            }
            std::optional<core::ParagraphStyleProvenance>
                paragraphStyleProvenance;
            if (version >= 10) {
                const auto encodedProvenance =
                    encodedParagraph.find("style_provenance");
                if (encodedProvenance != encodedParagraph.end()) {
                    if (!styleId) {
                        error =
                            "Recovery paragraph style provenance requires a style ID";
                        return std::nullopt;
                    }
                    if (!encodedProvenance->is_object()) {
                        error =
                            "Recovery paragraph style provenance is not an object";
                        return std::nullopt;
                    }
                    core::ParagraphStyleProvenance provenance;
                    const auto inheritedCharacter = encodedProvenance->find(
                        "inherited_character_format");
                    const auto inheritedParagraphMark =
                        encodedProvenance->find(
                            "inherited_paragraph_mark_character_format");
                    const auto inheritedParagraph = encodedProvenance->find(
                        "inherited_paragraph_format");
                    const auto encodedOverrides = encodedProvenance->find(
                        "character_overrides");
                    const auto markOverrides = encodedProvenance->find(
                        "paragraph_mark_overrides");
                    const auto paragraphOverrides = encodedProvenance->find(
                        "paragraph_overrides");
                    if (inheritedCharacter == encodedProvenance->end() ||
                        inheritedParagraph == encodedProvenance->end() ||
                        encodedOverrides == encodedProvenance->end() ||
                        markOverrides == encodedProvenance->end() ||
                        paragraphOverrides == encodedProvenance->end() ||
                        !decodeCharacterFormat(
                            *inheritedCharacter,
                            provenance.inherited_character_format, error) ||
                        !decodeParagraphFormat(
                            *inheritedParagraph,
                            provenance.inherited_paragraph_format, error) ||
                        !decodeCharacterFormatMask(
                            *markOverrides,
                            provenance.paragraph_mark_overrides, error) ||
                        !decodeParagraphFormatMask(
                            *paragraphOverrides,
                            provenance.paragraph_overrides, error)) {
                        if (error.empty()) {
                            error =
                                "Recovery paragraph style provenance is incomplete";
                        }
                        return std::nullopt;
                    }
                    if (version >= 11) {
                        if (inheritedParagraphMark ==
                                encodedProvenance->end() ||
                            !decodeCharacterFormat(
                                *inheritedParagraphMark,
                                provenance
                                    .inherited_paragraph_mark_character_format,
                                error)) {
                            if (error.empty()) {
                                error =
                                    "Recovery paragraph style provenance is incomplete";
                            }
                            return std::nullopt;
                        }
                    } else {
                        // Version 10 predated a distinct paragraph-mark style
                        // baseline.  Its only safe interpretation is the run
                        // baseline used by the old transition code.
                        provenance.inherited_paragraph_mark_character_format =
                            provenance.inherited_character_format;
                    }
                    if (!encodedOverrides->is_array() ||
                        encodedOverrides->size() >
                            kMaximumFormatRuns - totalFormatRuns) {
                        error =
                            "Recovery style-provenance override list is invalid";
                        return std::nullopt;
                    }
                    totalFormatRuns += encodedOverrides->size();
                    std::size_t previousEnd = 0;
                    for (const auto& encodedOverride : *encodedOverrides) {
                        if (!encodedOverride.is_object()) {
                            error =
                                "Recovery style-provenance override is not an object";
                            return std::nullopt;
                        }
                        const auto start64 =
                            encodedOverride.at("start").get<std::uint64_t>();
                        const auto end64 =
                            encodedOverride.at("end").get<std::uint64_t>();
                        if (start64 > std::numeric_limits<std::size_t>::max() ||
                            end64 > std::numeric_limits<std::size_t>::max()) {
                            error =
                                "Recovery style-provenance override is too large";
                            return std::nullopt;
                        }
                        const auto start = static_cast<std::size_t>(start64);
                        const auto end = static_cast<std::size_t>(end64);
                        core::CharacterFormatMask mask;
                        if (start >= end || end > text.size() ||
                            start < previousEnd ||
                            !core::isUtf16Boundary(text, start) ||
                            !core::isUtf16Boundary(text, end) ||
                            !decodeCharacterFormatMask(
                                encodedOverride.at("properties"), mask,
                                error) ||
                            mask.empty()) {
                            if (error.empty()) {
                                error =
                                    "Recovery style-provenance override range is invalid";
                            }
                            return std::nullopt;
                        }
                        provenance.character_overrides.push_back(
                            {start, end, mask});
                        previousEnd = end;
                    }
                    paragraphStyleProvenance = std::move(provenance);
                }
            }
            auto paragraph = core::Paragraph::restore(
                std::move(plainText), *id,
                std::move(paragraphMarkCharacterFormat),
                std::move(styleId));
            if (!paragraph) {
                error = paragraph.error().message;
                return std::nullopt;
            }
            paragraphs.push_back(std::move(paragraph.value()));
            if (paragraphStyleProvenance) {
                styleProvenance.push_back(
                    {*id, std::move(*paragraphStyleProvenance)});
            }
            equations.insert(equations.end(),
                             std::make_move_iterator(paragraphEquations.begin()),
                             std::make_move_iterator(paragraphEquations.end()));
            images.insert(images.end(),
                          std::make_move_iterator(paragraphImages.begin()),
                          std::make_move_iterator(paragraphImages.end()));

            core::ParagraphFormat paragraphFormat;
            if (!decodeParagraphFormat(encodedParagraph.at("format"), paragraphFormat, error))
                return std::nullopt;
            paragraphFormats.push_back({*id, std::move(paragraphFormat)});

            const auto& encodedRuns = encodedParagraph.at("runs");
            if (!encodedRuns.is_array() ||
                encodedRuns.size() >
                    kMaximumFormatRuns - totalFormatRuns) {
                error = "Recovery format runs are not an array";
                return std::nullopt;
            }
            totalFormatRuns += encodedRuns.size();
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

        if (version == 5 && seenLegacyImages != images.size()) {
            error = "Recovery inline picture references a missing paragraph";
            return std::nullopt;
        }

        auto document = core::Document::create(std::move(paragraphs));
        if (!document) {
            error = document.error().message;
            return std::nullopt;
        }
        if (version >= 6) {
            struct PendingObjectRef {
                core::NodeId paragraph_id;
                std::size_t offset{};
                bool image{false};
                std::size_t index{};
            };
            std::vector<PendingObjectRef> objects;
            objects.reserve(equations.size() + images.size());
            for (std::size_t index = 0; index < equations.size(); ++index) {
                objects.push_back({equations[index].paragraph_id,
                                   equations[index].offset, false, index});
            }
            for (std::size_t index = 0; index < images.size(); ++index) {
                objects.push_back({images[index].paragraph_id,
                                   images[index].offset, true, index});
            }
            std::sort(
                objects.begin(), objects.end(),
                [](const PendingObjectRef& left,
                   const PendingObjectRef& right) {
                    if (left.paragraph_id != right.paragraph_id) {
                        return left.paragraph_id < right.paragraph_id;
                    }
                    return left.offset < right.offset;
                });
            for (const auto& object : objects) {
                core::Result<void> inserted = [&]() {
                    if (!object.image) {
                        auto& equation = equations[object.index];
                        return document.value().insertEquation(
                            {equation.paragraph_id, equation.offset},
                            std::move(equation.latex), equation.display,
                            equation.equation_id);
                    }
                    auto& image = images[object.index];
                    return document.value().insertImage(
                        {image.paragraph_id, image.offset},
                        std::move(image.encoded_payload), image.format,
                        std::move(image.accessible_name), image.width_emu,
                        image.height_emu, image.image_id, std::nullopt,
                        image.layout);
                }();
                if (!inserted) {
                    error = inserted.error().message;
                    return std::nullopt;
                }
            }
        } else {
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
        for (auto& pending : styleProvenance) {
            const auto attached =
                document.value().setParagraphStyleProvenance(
                    pending.paragraph_id, std::move(pending.provenance));
            if (!attached) {
                error = attached.error().message;
                return std::nullopt;
            }
        }
        if (version == 5) {
            std::vector<std::size_t> imageOrder(images.size());
            for (std::size_t index = 0; index < imageOrder.size(); ++index) {
                imageOrder[index] = index;
            }
            std::stable_sort(
                imageOrder.begin(), imageOrder.end(),
                [&images](std::size_t leftIndex, std::size_t rightIndex) {
                    const auto& left = images[leftIndex];
                    const auto& right = images[rightIndex];
                    if (left.paragraph_id != right.paragraph_id) {
                        return left.paragraph_id < right.paragraph_id;
                    }
                    if (left.offset != right.offset) {
                        return left.offset < right.offset;
                    }
                    return left.source_order < right.source_order;
                });
            core::NodeId currentParagraph;
            std::size_t insertedInParagraph = 0;
            for (const std::size_t imageIndex : imageOrder) {
                auto& image = images[imageIndex];
                if (image.paragraph_id != currentParagraph) {
                    currentParagraph = image.paragraph_id;
                    insertedInParagraph = 0;
                }
                const auto* paragraph =
                    document.value().findParagraph(image.paragraph_id);
                if (!paragraph) {
                    error = "Recovery inline picture references a missing paragraph";
                    return std::nullopt;
                }
                const std::size_t targetOffset =
                    image.offset + insertedInParagraph;
                const std::size_t formatOffset =
                    targetOffset < paragraph->text().size()
                    ? targetOffset + 1U
                    : targetOffset;
                const auto characterFormat =
                    paragraph->characterFormatAt(formatOffset);
                const auto inserted = document.value().insertImage(
                    {image.paragraph_id, targetOffset},
                    std::move(image.encoded_payload), image.format,
                    std::move(image.accessible_name), image.width_emu,
                    image.height_emu, image.image_id, characterFormat);
                if (!inserted) {
                    error = inserted.error().message;
                    return std::nullopt;
                }
                ++insertedInParagraph;
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
                            !encodedRuns->is_array() ||
                            encodedRuns->size() >
                                kMaximumFormatRuns - totalFormatRuns) {
                            error = "Recovery table cell format runs are not an array";
                            return std::nullopt;
                        }
                        totalFormatRuns += encodedRuns->size();
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
        if (version >= 12) {
            std::u16string header;
            std::u16string footer;
            if (!decodeUtf16(root.at("header_text_utf16"), header,
                             totalCodeUnits, error) ||
                !decodeUtf16(root.at("footer_text_utf16"), footer,
                             totalCodeUnits, error)) {
                return std::nullopt;
            }
            const auto restoreStory = [&](bool isFooter,
                                          std::u16string story) {
                const Json* encodedImages = nullptr;
                if (version >= 13) {
                    encodedImages = &root.at(
                        isFooter ? "footer_images" : "header_images");
                    if (!encodedImages->is_array() ||
                        encodedImages->size() >
                            core::kMaximumInlineImagesPerDocument -
                                totalImages ||
                        static_cast<std::size_t>(std::count(
                            story.begin(), story.end(),
                            core::kInlineObjectReplacementCharacter)) !=
                            encodedImages->size()) {
                        return core::Result<void>(core::Error{
                            core::ErrorCode::invalid_operation,
                            "Recovery header/footer picture list is invalid"});
                    }
                }
                std::u16string plain;
                plain.reserve(story.size());
                std::copy_if(
                    story.begin(), story.end(), std::back_inserter(plain),
                    [](char16_t value) {
                        return value !=
                            core::kInlineObjectReplacementCharacter;
                    });
                auto result = isFooter
                    ? document.value().setFooterText(std::move(plain))
                    : document.value().setHeaderText(std::move(plain));
                if (!result || !encodedImages) return result;
                std::size_t previousOffset = 0;
                bool first = true;
                for (const auto& encodedImage : *encodedImages) {
                    if (!encodedImage.is_object()) {
                        return core::Result<void>(core::Error{
                            core::ErrorCode::invalid_operation,
                            "Recovery header/footer picture is invalid"});
                    }
                    const auto imageId = core::NodeId::parse(
                        encodedImage.at("id").get<std::string>());
                    const auto offset64 =
                        encodedImage.at("offset").get<std::uint64_t>();
                    const auto format = parseImageFormat(
                        encodedImage.at("format").get<std::string>());
                    const auto accessibleName =
                        encodedImage.at("accessible_name").get<std::string>();
                    const auto width =
                        encodedImage.at("width_emu").get<std::int64_t>();
                    const auto height =
                        encodedImage.at("height_emu").get<std::int64_t>();
                    const std::size_t remaining =
                        core::kMaximumDocumentEncodedImageBytes -
                        totalImageBytes;
                    auto bytes = base64Decode(
                        encodedImage.at("encoded_base64").get<std::string>(),
                        std::min(core::kMaximumEncodedImageBytes, remaining));
                    if (!imageId || !format || !bytes ||
                        !imageIds.insert(*imageId).second ||
                        offset64 > std::numeric_limits<std::size_t>::max() ||
                        offset64 >= story.size() ||
                        story[static_cast<std::size_t>(offset64)] !=
                            core::kInlineObjectReplacementCharacter ||
                        (!first && offset64 <= previousOffset) ||
                        accessibleName.size() >
                            core::kMaximumImageAccessibleNameBytes ||
                        width <= 0 || height <= 0) {
                        return core::Result<void>(core::Error{
                            core::ErrorCode::invalid_operation,
                            "Recovery header/footer picture is invalid"});
                    }
                    core::EncodedImagePayload imagePayload(
                        std::move(*bytes));
                    if (!validEncodedImage(imagePayload, *format)) {
                        return core::Result<void>(core::Error{
                            core::ErrorCode::invalid_operation,
                            "Recovery header/footer picture payload is invalid"});
                    }
                    totalImageBytes += imagePayload.size();
                    ++totalImages;
                    previousOffset = static_cast<std::size_t>(offset64);
                    first = false;
                    result = document.value().insertHeaderFooterImage(
                        isFooter, previousOffset, std::move(imagePayload), *format,
                        accessibleName, width, height, *imageId);
                    if (!result) return result;
                }
                return core::Result<void>{};
            };
            const auto headerResult = restoreStory(false, std::move(header));
            const auto footerResult = restoreStory(true, std::move(footer));
            if (!headerResult || !footerResult) {
                error = !headerResult ? headerResult.error().message
                                      : footerResult.error().message;
                return std::nullopt;
            }
        }
        return RecoveryDocument{std::move(document.value()), page};
    } catch (const std::exception& exception) {
        error = std::string("Recovery snapshot is malformed: ") + exception.what();
        return std::nullopt;
    }
}

}  // namespace docxstudio::app
