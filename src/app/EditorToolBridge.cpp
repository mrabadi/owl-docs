#include "docxstudio/app/EditorToolBridge.h"

#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/app/ExcalidrawScene.h"
#include "docxstudio/app/EditorSemanticOperations.h"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/core/academic.h"
#include "docxstudio/math/latex_parser.h"

#include <QRegularExpression>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace docxstudio::app {
namespace {

struct Target {
    core::NodeId block;
    std::size_t start{};
    std::optional<std::size_t> end;
};

QString fromUtf16(const std::u16string& value) {
    return QString::fromUtf16(value.data(), static_cast<qsizetype>(value.size()));
}

std::string toUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString normalizeInlineText(QString value) {
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    value.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return value;
}

void truncateUtf16Safely(QString& text, qsizetype maximum) {
    if (maximum < 0) maximum = 0;
    if (text.size() <= maximum) return;
    if (maximum > 0 && text.at(maximum - 1).isHighSurrogate() &&
        text.at(maximum).isLowSurrogate()) {
        --maximum;
    }
    text.truncate(maximum);
}

struct SearchContext {
    QString text;
    std::size_t start{};
    std::size_t end{};
};

constexpr std::size_t kMaximumSearchQueryCodePoints = 1024;
constexpr std::size_t kMaximumSearchResults = 500;
constexpr qsizetype kSearchContextRadiusUtf16 = 80;

std::size_t unicodeCodePointCount(const QString& text) {
    std::size_t result = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        if (text.at(index).isHighSurrogate() && index + 1 < text.size() &&
            text.at(index + 1).isLowSurrogate()) {
            ++index;
        }
        ++result;
    }
    return result;
}

bool isBoundaryAt(QTextBoundaryFinder& boundaries, const qsizetype position) {
    boundaries.setPosition(position);
    return boundaries.isAtBoundary();
}

SearchContext searchContextFor(const QString& text, const qsizetype matchStart,
                               const qsizetype matchEnd,
                               QTextBoundaryFinder& graphemeBoundaries) {
    qsizetype start = std::max<qsizetype>(
        0, matchStart - kSearchContextRadiusUtf16);
    qsizetype end = std::min<qsizetype>(
        text.size(), matchEnd + kSearchContextRadiusUtf16);

    // Keep the context bounded by moving an interior leading edge forward and
    // an interior trailing edge backward. Search matches themselves are
    // required to be grapheme-aligned, so these adjustments cannot omit any
    // part of the match, even for pathologically long combining sequences.
    if (!isBoundaryAt(graphemeBoundaries, start)) {
        const qsizetype adjusted = graphemeBoundaries.toNextBoundary();
        start = adjusted >= 0 && adjusted <= matchStart ? adjusted : matchStart;
    }
    if (!isBoundaryAt(graphemeBoundaries, end)) {
        const qsizetype adjusted = graphemeBoundaries.toPreviousBoundary();
        end = adjusted >= matchEnd ? adjusted : matchEnd;
    }
    return {text.mid(start, end - start), static_cast<std::size_t>(start),
            static_cast<std::size_t>(end)};
}

bool isWholeWordMatch(QTextBoundaryFinder& boundaries, const qsizetype start,
                      const qsizetype end) {
    boundaries.setPosition(start);
    if (!(boundaries.boundaryReasons() & QTextBoundaryFinder::StartOfItem)) {
        return false;
    }
    boundaries.setPosition(end);
    return boundaries.boundaryReasons() & QTextBoundaryFinder::EndOfItem;
}

std::optional<std::uint64_t> unsignedValue(const codex::Json& object,
                                           const char* name) {
    const auto found = object.find(name);
    if (found == object.end() ||
        !(found->is_number_unsigned() || found->is_number_integer())) {
        return std::nullopt;
    }
    if (!found->is_number_unsigned() && found->is_number_integer() && found->get<std::int64_t>() < 0) {
        return std::nullopt;
    }
    try {
        return found->get<std::uint64_t>();
    } catch (...) {
        return std::nullopt;
    }
}

bool parseTarget(const codex::Json& operation, Target& target, QString& error,
                 bool requireEnd) {
    const auto found = operation.find("target");
    if (found == operation.end() || !found->is_object()) {
        error = QObject::tr("An editor operation has no valid target.");
        return false;
    }
    auto id = found->find("blockId");
    if (id == found->end()) id = found->find("paragraphId");
    if (id == found->end() || !id->is_string()) {
        error = QObject::tr("An editor target has no blockId.");
        return false;
    }
    const auto parsed = core::NodeId::parse(id->get<std::string>());
    auto start = unsignedValue(*found, "start");
    if (!start) start = unsignedValue(*found, "offset");
    if (!start) start = unsignedValue(*found, "utf16Offset");
    if (!start) start = unsignedValue(*found, "startOffset");
    if (!parsed || !start ||
        *start > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = QObject::tr("An editor target has an invalid blockId or UTF-16 offset.");
        return false;
    }
    target.block = *parsed;
    target.start = static_cast<std::size_t>(*start);
    const auto end = unsignedValue(*found, "end");
    if (end) {
        if (*end > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            error = QObject::tr("An editor target end offset is too large.");
            return false;
        }
        target.end = static_cast<std::size_t>(*end);
    }
    if (requireEnd && !target.end) {
        error = QObject::tr("This editor operation requires a target end offset.");
        return false;
    }
    if (target.end && *target.end < target.start) {
        error = QObject::tr("An editor target ends before it starts.");
        return false;
    }
    return true;
}

std::string operationKind(const codex::Json& operation) {
    for (const char* key : {"kind", "type", "operation"}) {
        const auto value = operation.find(key);
        if (value != operation.end() && value->is_string()) {
            std::string kind = value->get<std::string>();
            if (kind == "insertExcalidrawFigure")
                return "insert_excalidraw_figure";
            if (kind == "replaceExcalidrawFigure")
                return "replace_excalidraw_figure";
            return kind;
        }
    }
    return {};
}

bool validExcalidrawColor(const std::string& value) {
    if (value.size() != 7 || value.front() != '#') return false;
    return std::all_of(value.begin() + 1, value.end(), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    });
}

std::optional<double> finiteNumber(const codex::Json& object,
                                   const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) return std::nullopt;
    const double number = value->get<double>();
    return std::isfinite(number) ? std::optional{number} : std::nullopt;
}

std::optional<codex::Json> normalizeExcalidrawElements(
    const codex::Json& source, QString& error) {
    if (!source.is_array() || source.empty() || source.size() > 128) {
        error = QObject::tr(
            "An Excalidraw figure requires between 1 and 128 elements.");
        return std::nullopt;
    }
    codex::Json result = codex::Json::array();
    std::set<std::string> ids;
    std::size_t generatedId = 0;
    for (const auto& input : source) {
        if (!input.is_object()) {
            error = QObject::tr("Every Excalidraw element must be an object.");
            return std::nullopt;
        }
        const auto typeValue = input.find("type");
        if (typeValue == input.end() || !typeValue->is_string()) {
            error = QObject::tr("Every Excalidraw element requires a type.");
            return std::nullopt;
        }
        const std::string type = typeValue->get<std::string>();
        if (type != "rectangle" && type != "ellipse" && type != "diamond" &&
            type != "text" && type != "line" && type != "arrow") {
            error = QObject::tr("Unsupported Excalidraw element type: %1")
                        .arg(QString::fromStdString(type));
            return std::nullopt;
        }
        if (type == "line" && input.contains("label")) {
            error = QObject::tr("A plain line cannot have a bound label; use an arrow or separate text.");
            return std::nullopt;
        }
        auto x = finiteNumber(input, "x");
        auto y = finiteNumber(input, "y");
        if (!x || !y || std::abs(*x) > 10000.0 ||
            std::abs(*y) > 10000.0) {
            error = QObject::tr("An Excalidraw element has invalid coordinates.");
            return std::nullopt;
        }
        std::string id;
        if (const auto idValue = input.find("id");
            idValue != input.end() && idValue->is_string()) {
            id = idValue->get<std::string>();
        }
        if (id.empty() || id.size() > 64 || ids.contains(id)) {
            do {
                id = "codex-element-" + std::to_string(++generatedId);
            } while (ids.contains(id));
        }
        ids.insert(id);
        codex::Json element{{"id", id}, {"type", type}, {"x", *x}, {"y", *y}};
        for (const char* colorKey : {"strokeColor", "backgroundColor"}) {
            const auto color = input.find(colorKey);
            if (color != input.end() && color->is_string() &&
                validExcalidrawColor(color->get<std::string>())) {
                element[colorKey] = *color;
            }
        }
        if (const auto stroke = finiteNumber(input, "strokeWidth"))
            element["strokeWidth"] = std::clamp(*stroke, 1.0, 4.0);
        if (const auto opacity = finiteNumber(input, "opacity"))
            element["opacity"] = static_cast<int>(
                std::llround(std::clamp(*opacity, 0.0, 100.0)));

        if (type == "rectangle" || type == "ellipse" || type == "diamond") {
            const auto width = finiteNumber(input, "width");
            const auto height = finiteNumber(input, "height");
            if (!width || !height || *width <= 0.0 || *height <= 0.0 ||
                *width > 10000.0 || *height > 10000.0) {
                error = QObject::tr("An Excalidraw shape has invalid dimensions.");
                return std::nullopt;
            }
            element["width"] = *width;
            element["height"] = *height;
        } else if (type == "text") {
            const auto text = input.find("text");
            if (text == input.end() || !text->is_string() ||
                text->get<std::string>().empty()) {
                error = QObject::tr("An Excalidraw text element has no text.");
                return std::nullopt;
            }
            element["text"] = text->get<std::string>();
            if (const auto size = finiteNumber(input, "fontSize"))
                element["fontSize"] = std::clamp(*size, 8.0, 96.0);
        } else {
            codex::Json points;
            if (const auto supplied = input.find("points");
                supplied != input.end() && supplied->is_array() &&
                supplied->size() >= 2 && supplied->size() <= 16) {
                points = codex::Json::array();
                for (const auto& point : *supplied) {
                    if (!point.is_array() || point.size() != 2 ||
                        !point.at(0).is_number() ||
                        !point.at(1).is_number()) {
                        error = QObject::tr(
                            "An Excalidraw line or arrow contains an invalid point.");
                        return std::nullopt;
                    }
                    const double pointX = point.at(0).get<double>();
                    const double pointY = point.at(1).get<double>();
                    if (!std::isfinite(pointX) || !std::isfinite(pointY) ||
                        std::abs(pointX) > 10000.0 ||
                        std::abs(pointY) > 10000.0) {
                        error = QObject::tr(
                            "An Excalidraw line or arrow contains an invalid point.");
                        return std::nullopt;
                    }
                    points.push_back(codex::Json::array({pointX, pointY}));
                }
            } else {
                const auto width = finiteNumber(input, "width");
                const auto height = finiteNumber(input, "height");
                if (!width || !height || std::abs(*width) > 10000.0 ||
                    std::abs(*height) > 10000.0) {
                    error = QObject::tr(
                        "An Excalidraw line or arrow requires points or width and height.");
                    return std::nullopt;
                }
                points = codex::Json::array(
                    {codex::Json::array({0.0, 0.0}),
                     codex::Json::array({*width, *height})});
            }
            element["points"] = std::move(points);
        }
        if (const auto label = input.find("label");
            label != input.end() && label->is_object() && type != "line" && type != "text") {
            const auto text = label->find("text");
            if (text != label->end() && text->is_string() &&
                !text->get<std::string>().empty()) {
                element["label"] = {{"text", text->get<std::string>()}};
                if (const auto labelId = label->find("id"); labelId != label->end()) {
                    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
                    if (!labelId->is_string() || !idPattern.match(QString::fromStdString(labelId->get<std::string>())).hasMatch()) {
                        error = QObject::tr("An Excalidraw label has an invalid stable identity."); return std::nullopt;
                    }
                    element["label"]["id"] = *labelId;
                }
                if (const auto size = finiteNumber(*label, "fontSize"))
                    element["label"]["fontSize"] = std::clamp(*size, 8.0, 96.0);
            }
        }
        result.push_back(std::move(element));
    }
    std::set<std::string> bindable;
    for (const auto& element : result) {
        const auto type = element.at("type").get<std::string>();
        if (type == "rectangle" || type == "ellipse" || type == "diamond") bindable.insert(element.at("id").get<std::string>());
        if (element.contains("label") && element.at("label").contains("id") &&
            !ids.insert(element.at("label").at("id").get<std::string>()).second) {
            error = QObject::tr("Excalidraw element and label identities must be unique."); return std::nullopt;
        }
    }
    for (std::size_t index = 0; index < source.size(); ++index) {
        for (const auto* key : {"start", "end"}) {
            const auto binding = source.at(index).find(key);
            if (binding == source.at(index).end()) continue;
            if (result.at(index).at("type") != "arrow" || !binding->is_object() || binding->size() != 1 ||
                !binding->contains("id") || !binding->at("id").is_string() || !bindable.contains(binding->at("id").get<std::string>())) {
                error = QObject::tr("An Excalidraw arrow binding must name an existing shape in its skeleton."); return std::nullopt;
            }
            result.at(index)[key] = *binding;
        }
    }
    return std::optional<codex::Json>(std::move(result));
}

std::string alignmentName(const core::ParagraphFormat& format) {
    switch (format.alignment.value_or(core::ParagraphAlignment::left)) {
        case core::ParagraphAlignment::left: return "left";
        case core::ParagraphAlignment::center: return "center";
        case core::ParagraphAlignment::right: return "right";
        case core::ParagraphAlignment::justified: return "justify";
        case core::ParagraphAlignment::distributed: return "distributed";
    }
    return "left";
}

codex::Json characterStyleFor(const core::CharacterFormat& source) {
    codex::Json style = codex::Json::object();
    if (source.font_family) style["fontFamily"] = *source.font_family;
    if (source.font_size_half_points) {
        style["fontSizePoints"] = *source.font_size_half_points / 2.0;
    }
    if (source.bold) style["bold"] = *source.bold;
    if (source.italic) style["italic"] = *source.italic;
    if (source.underline) {
        style["underline"] =
            *source.underline != core::UnderlineStyle::none;
    }
    if (source.strike) style["strike"] = *source.strike;
    const auto colorName = [](std::uint32_t argb) {
        return toUtf8(QStringLiteral("#%1")
                          .arg(argb & 0x00ffffffU, 6, 16,
                               QLatin1Char('0'))
                          .toUpper());
    };
    if (source.foreground_argb) {
        style["foregroundColor"] = colorName(*source.foreground_argb);
    }
    if (source.highlight_argb) {
        style["highlightColor"] = colorName(*source.highlight_argb);
    }
    if (source.baseline) {
        switch (*source.baseline) {
            case core::BaselinePosition::normal:
                style["verticalAlign"] = "baseline";
                break;
            case core::BaselinePosition::superscript:
                style["verticalAlign"] = "superscript";
                break;
            case core::BaselinePosition::subscript:
                style["verticalAlign"] = "subscript";
                break;
        }
    }
    return style;
}

codex::Json formattingFor(
    const std::vector<core::FormatRun>& characterFormats,
    const core::ParagraphFormat& paragraphFormat,
    std::size_t returnedTextStart,
    std::size_t returnedTextLength) {
    codex::Json runs = codex::Json::array();
    const std::size_t returnedTextEnd = returnedTextStart + returnedTextLength;
    for (const auto& run : characterFormats) {
        const std::size_t start = std::max(run.start, returnedTextStart);
        const std::size_t end = std::min(run.end, returnedTextEnd);
        if (start >= end) continue;
        auto format = characterStyleFor(run.format);
        runs.push_back({{"start", start - returnedTextStart},
                        {"end", end - returnedTextStart},
                        {"style", std::move(format)}});
    }
    codex::Json result = {{"alignment", alignmentName(paragraphFormat)}, {"characterRuns", std::move(runs)}};
    const auto measurement = [&](const char* name, const std::optional<std::int64_t>& value) {
        if (value) result[name] = static_cast<double>(*value) / 12700.0;
    };
    measurement("leftIndentPoints", paragraphFormat.left_indent_emu);
    measurement("rightIndentPoints", paragraphFormat.right_indent_emu);
    measurement("firstLineIndentPoints", paragraphFormat.first_line_indent_emu);
    measurement("spaceBeforePoints", paragraphFormat.space_before_emu);
    measurement("spaceAfterPoints", paragraphFormat.space_after_emu);
    measurement("lineSpacingPoints", paragraphFormat.line_spacing_emu);
    if (paragraphFormat.line_spacing_rule) {
        const auto rule = *paragraphFormat.line_spacing_rule;
        result["lineSpacingRule"] = rule == core::LineSpacingRule::exact ? "exact" : rule == core::LineSpacingRule::at_least ? "atLeast" : "automatic";
        if (rule == core::LineSpacingRule::automatic && paragraphFormat.line_spacing_emu)
            result["lineSpacing"] = static_cast<double>(*paragraphFormat.line_spacing_emu) / (12.0 * 12700.0);
    }
    if (paragraphFormat.keep_with_next) result["keepWithNext"] = *paragraphFormat.keep_with_next;
    if (paragraphFormat.keep_lines) result["keepLines"] = *paragraphFormat.keep_lines;
    if (paragraphFormat.page_break_before) result["pageBreakBefore"] = *paragraphFormat.page_break_before;
    if (paragraphFormat.list_id) result["listId"] = paragraphFormat.list_id->toString();
    if (paragraphFormat.list_level) result["listLevel"] = *paragraphFormat.list_level + 1;
    if (paragraphFormat.list_layout) {
        result["listLevels"] = codex::Json::array();
        for (const auto& level : paragraphFormat.list_layout->levels) result["listLevels"].push_back({
            {"bulletIndentSpaces", level.bullet_indent_spaces}, {"textIndentSpaces", level.text_indent_spaces}});
    }
    return result;
}

codex::Json formattingFor(const core::Paragraph& paragraph,
                          std::size_t returnedTextStart,
                          std::size_t returnedTextLength) {
    auto result = formattingFor(
        paragraph.characterFormats(), paragraph.format(), returnedTextStart,
        returnedTextLength);
    if (!paragraph.paragraphMarkCharacterFormat().empty()) {
        result["paragraphMarkStyle"] = characterStyleFor(
            paragraph.paragraphMarkCharacterFormat());
    }
    return result;
}

codex::Json formattingFor(const core::TableCell& cell,
                          std::size_t returnedTextStart,
                          std::size_t returnedTextLength) {
    auto result = formattingFor(
        cell.character_formats, cell.paragraph_format, returnedTextStart,
        returnedTextLength);
    if (!cell.default_character_format.empty()) {
        result["paragraphMarkStyle"] = characterStyleFor(
            cell.default_character_format);
    }
    return result;
}

void appendParagraphStyleMetadata(const core::Paragraph& paragraph,
                                  codex::Json& attributes) {
    if (!paragraph.styleId()) return;
    attributes["styleId"] = *paragraph.styleId();
    const auto* definition =
        core::findBuiltInParagraphStyle(*paragraph.styleId());
    if (!definition) return;
    attributes["styleDisplayName"] = definition->display_name;
    if (definition->outline_level) {
        attributes["outlineLevel"] = *definition->outline_level;
    }
}

codex::Json equationsFor(const core::Paragraph& paragraph,
                         std::size_t returnedTextStart,
                         std::size_t returnedTextLength) {
    codex::Json equations = codex::Json::array();
    const std::size_t returnedTextEnd = returnedTextStart + returnedTextLength;
    for (const auto& equation : paragraph.equations()) {
        if (equation.utf16_offset < returnedTextStart ||
            equation.utf16_offset >= returnedTextEnd) {
            continue;
        }
        equations.push_back(
            {{"id", equation.id.toString()},
             {"utf16Offset", equation.utf16_offset - returnedTextStart},
             {"canonicalLatex", equation.canonical_latex},
             {"display", equation.display}});
    }
    return equations;
}

const char* fieldKindName(core::FieldKind kind) {
    switch (kind) {
        case core::FieldKind::note_reference: return "note_reference";
        case core::FieldKind::caption_number: return "caption_number";
        case core::FieldKind::bibliography_number: return "bibliography_number";
        case core::FieldKind::cross_reference: return "cross_reference";
        case core::FieldKind::citation: return "citation";
    }
    return "unknown";
}
void appendAcademicMetadata(const core::Paragraph& paragraph, codex::Json& attributes,
                            const core::AcademicResolution& resolution,
                            std::size_t start, std::size_t length) {
    attributes["fields"] = codex::Json::array();
    for (const auto& field : paragraph.fields()) {
        if (field.utf16_offset < start || field.utf16_offset >= start + length) continue;
        const auto* resolved = resolution.findField(field.id);
        attributes["fields"].push_back({{"id", field.id.toString()},
            {"utf16Offset", field.utf16_offset - start}, {"kind", fieldKindName(field.kind)},
            {"targetId", field.target_id.toString()}, {"text", toUtf8(fromUtf16(resolution.fieldText(field.id)))},
            {"locator", toUtf8(fromUtf16(field.locator))}, {"unresolved", resolved && resolved->unresolved}});
    }
    if (paragraph.caption()) attributes["caption"] = {
        {"label", toUtf8(fromUtf16(core::captionLabelText(paragraph.caption()->label)))},
        {"targetId", paragraph.caption()->target_id.toString()}};
    if (paragraph.isBibliographyEntry()) attributes["bibliographyEntry"] = true;
}

codex::Json imagesFor(const std::vector<core::ImageAtom>& sourceImages,
                      std::size_t returnedTextStart,
                      std::size_t returnedTextLength,
                      bool includeNativeScene = false,
                      std::size_t* remainingContextBytes = nullptr) {
    codex::Json images = codex::Json::array();
    std::size_t localContextBudget = 256U * 1024U;
    auto& remaining = remainingContextBytes ? *remainingContextBytes : localContextBudget;
    // Leave room for empty arrays if subsequent pictures exhaust their payload
    // allowance. This applies across all calls sharing a document-read budget.
    constexpr std::size_t arrayReserve = 4U * core::kMaximumInlineImagesPerDocument;
    const std::size_t returnedTextEnd = returnedTextStart + returnedTextLength;
    for (const auto& image : sourceImages) {
        if (image.utf16_offset < returnedTextStart ||
            image.utf16_offset >= returnedTextEnd) {
            continue;
        }
        const auto placement = [&image] {
            switch (image.layout.placement) {
                case core::ImagePlacement::inline_with_text:
                    return "inline";
                case core::ImagePlacement::square:
                    return "square";
                case core::ImagePlacement::top_and_bottom:
                    return "topBottom";
            }
            return "inline";
        }();
        codex::Json encodedImage =
            {{"id", image.id.toString()},
             {"utf16Offset", image.utf16_offset - returnedTextStart},
             {"accessibleName", image.accessible_name},
             {"mimeType", core::imageContentType(image.format)},
             {"widthEmu", image.width_emu},
             {"heightEmu", image.height_emu},
             {"layout",
              {{"placement", placement},
               {"distanceTopEmu", image.layout.distance_top_emu},
               {"distanceRightEmu", image.layout.distance_right_emu},
               {"distanceBottomEmu", image.layout.distance_bottom_emu},
               {"distanceLeftEmu", image.layout.distance_left_emu},
               {"moveWithText", image.layout.move_with_text}}},
             {"encodedBytes", image.encoded_payload.size()}};
        if (image.format == core::ImageFormat::png) {
            const auto bytes = image.encoded_payload.bytes();
            const QByteArray png(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<qsizetype>(bytes.size()));
            if (const auto scene = excalidrawSceneFromPng(png)) {
                encodedImage["kind"] = "excalidrawFigure";
                const auto inspected = inspectExcalidrawScene(*scene);
                encodedImage["authoringMode"] = inspected.complete() ? "professional" : "preserved";
                if (includeNativeScene) {
                    const auto native = codex::Json::parse(scene->constData(), scene->constData() + scene->size());
                    const auto contextBytes = native.dump().size();
                    const bool fits = remaining >= arrayReserve && contextBytes <= remaining - arrayReserve;
                    encodedImage["nativeScene"] = fits ? native : codex::Json(nullptr);
                    encodedImage["nativeSceneTruncated"] = !fits;
                    if (fits) remaining -= contextBytes;
                }
                codex::Json elements = codex::Json::array();
                bool truncated = inspected.truncated;
                remaining -= std::min<std::size_t>(remaining, 2);
                for (const auto& value : inspected.elements) {
                    const QByteArray json = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
                    auto element = codex::Json::parse(json.constData(), json.constData() + json.size());
                    const auto contextBytes = element.dump().size() + (elements.empty() ? 0U : 1U);
                    if (remaining < arrayReserve || contextBytes > remaining - arrayReserve) { truncated = true; break; }
                    remaining -= contextBytes;
                    elements.push_back(std::move(element));
                }
                encodedImage["editableElements"] = std::move(elements);
                encodedImage["editableElementsTruncated"] = truncated;
                encodedImage["editableElementsComplete"] = inspected.complete() && !truncated;
                auto reasons = inspected.unsupported;
                if (truncated && !inspected.truncated) reasons.append(QStringLiteral("The shared figure read context budget was exhausted."));
                codex::Json unsupported = codex::Json::array();
                bool reasonsTruncated = false;
                remaining -= std::min<std::size_t>(remaining, 2);
                for (const auto& reason : reasons) {
                    const auto encoded = codex::Json(toUtf8(reason));
                    const auto contextBytes = encoded.dump().size() + (unsupported.empty() ? 0U : 1U);
                    if (remaining < arrayReserve || contextBytes > remaining - arrayReserve) { reasonsTruncated = true; break; }
                    remaining -= contextBytes;
                    unsupported.push_back(encoded);
                }
                encodedImage["editableElementsUnsupported"] = std::move(unsupported);
                encodedImage["editableElementsUnsupportedTruncated"] = reasonsTruncated;
            }
        }
        images.push_back(std::move(encodedImage));
    }
    return images;
}

bool validateDocumentId(const QString& documentId,
                        const codex::Json& arguments,
                        QString& error) {
    const auto value = arguments.find("documentId");
    if (value == arguments.end() || !value->is_string() ||
        QString::fromStdString(value->get<std::string>()) != documentId) {
        error = QObject::tr("The tool documentId does not match its Codex thread.");
        return false;
    }
    return true;
}

codex::Json pageLayoutFor(const DocumentCanvas& canvas) {
    codex::Json layout = {{"widthPoints", canvas.pageWidthPoints()}, {"heightPoints", canvas.pageHeightPoints()},
        {"marginTopPoints", canvas.marginTopPoints()}, {"marginRightPoints", canvas.marginRightPoints()},
        {"marginBottomPoints", canvas.marginBottomPoints()}, {"marginLeftPoints", canvas.marginLeftPoints()}};
    const auto fingerprint = QCryptographicHash::hash(QByteArray::fromStdString(layout.dump()), QCryptographicHash::Sha256);
    std::uint64_t state = 0;
    for (int index = 0; index < 8; ++index) state = (state << 8U) | static_cast<unsigned char>(fingerprint[index]);
    layout["stateId"] = state & ((std::uint64_t{1} << 53U) - 1U);
    return layout;
}

codex::Json readDocument(DocumentCanvas& canvas,
                         const QString& documentId,
                         const codex::Json& arguments,
                         QString& error) {
    if (!arguments.is_object() || !validateDocumentId(documentId, arguments, error)) {
        return {};
    }
    const auto snapshot = canvas.snapshot();
    if (const auto found = arguments.find("expectedRevision");
        found != arguments.end()) {
        const auto expected = unsignedValue(arguments, "expectedRevision");
        if (!expected) {
            error = QObject::tr(
                "expectedRevision must be a nonnegative integer.");
            return {};
        }
        if (*expected != snapshot.revision.value()) {
            error = QObject::tr("REVISION_CONFLICT: expected %1, current %2")
                        .arg(*expected)
                        .arg(snapshot.revision.value());
            return {};
        }
    }
    std::string scope = "selection";
    if (const auto found = arguments.find("scope"); found != arguments.end()) {
        if (!found->is_string()) {
            error = QObject::tr("scope must be a string.");
            return {};
        }
        scope = found->get<std::string>();
    }
    static const std::set<std::string> supportedScopes = {
        "selection", "document", "outline", "blocks", "tables", "stories", "images", "metadata"};
    if (!supportedScopes.contains(scope)) {
        error = QObject::tr("Unsupported editor read scope: %1. Supported scopes are selection, document, outline, blocks, tables, stories, images and metadata.")
                    .arg(QString::fromStdString(scope));
        return {};
    }
    bool includeFormatting = true;
    if (const auto found = arguments.find("includeFormatting");
        found != arguments.end()) {
        if (!found->is_boolean()) {
            error = QObject::tr("includeFormatting must be true or false.");
            return {};
        }
        includeFormatting = found->get<bool>();
    }
    std::size_t maximum = 50000;
    if (const auto found = arguments.find("maxCharacters");
        found != arguments.end()) {
        const auto requested = unsignedValue(arguments, "maxCharacters");
        if (!requested || *requested == 0 || *requested > 200000) {
            error = QObject::tr(
                "maxCharacters must be between 1 and 200000.");
            return {};
        }
        maximum = static_cast<std::size_t>(*requested);
    }

    std::set<core::NodeId> requestedIds;
    const auto ids = arguments.find("blockIds");
    if (ids != arguments.end()) {
        if (!ids->is_array() || ids->size() > 256) {
            error = QObject::tr(
                "blockIds must be an array of at most 256 stable IDs.");
            return {};
        }
        for (const auto& encoded : *ids) {
            if (!encoded.is_string()) {
                error = QObject::tr(
                    "Every blockIds entry must be a stable ID string.");
                return {};
            }
            const auto parsed =
                core::NodeId::parse(encoded.get<std::string>());
            if (!parsed) {
                error = QObject::tr(
                    "A blockIds entry contains an invalid stable ID.");
                return {};
            }
            requestedIds.insert(*parsed);
        }
    }

    std::set<std::pair<core::NodeId, core::NodeId>> requestedTableCells;
    const auto cellTargets = arguments.find("tableCellTargets");
    if (cellTargets != arguments.end()) {
        if (!cellTargets->is_array() || cellTargets->size() > 256) {
            error = QObject::tr(
                "tableCellTargets must be an array of at most 256 targets.");
            return {};
        }
        for (const auto& encoded : *cellTargets) {
            if (!encoded.is_object()) {
                error = QObject::tr(
                    "Every table-cell read target must be an object.");
                return {};
            }
            const auto tableValue = encoded.find("tableId");
            const auto cellValue = encoded.find("cellId");
            if (tableValue == encoded.end() || !tableValue->is_string() ||
                cellValue == encoded.end() || !cellValue->is_string()) {
                error = QObject::tr(
                    "Every table-cell read target requires tableId and cellId.");
                return {};
            }
            const auto tableId =
                core::NodeId::parse(tableValue->get<std::string>());
            const auto cellId =
                core::NodeId::parse(cellValue->get<std::string>());
            if (!tableId || !cellId) {
                error = QObject::tr(
                    "A table-cell read target contains an invalid stable ID.");
                return {};
            }
            requestedTableCells.emplace(*tableId, *cellId);
        }
    }

    std::optional<core::NormalizedRange> selected;
    if (scope == "selection") {
        const auto normalized = snapshot.document.normalizeRange(canvas.selection());
        if (normalized) selected = normalized.value();
    }

    codex::Json blocks = codex::Json::array();
    std::size_t characters = 0;
    std::size_t remainingFigureContextBytes = 256U * 1024U;
    bool truncated = false;
    const auto academic = core::resolveAcademic(snapshot.document);
    const auto activeStory = scope == "selection" ? canvas.activeStorySelection() : std::nullopt;
    const auto selectedCells = scope == "selection" && !activeStory ? canvas.selectedTableCellIds() : std::vector<core::NodeId>{};
    const auto cellRange = scope == "selection" && !activeStory ? canvas.selectedTableCellRange() : std::nullopt;
    const bool selectedTable = !selectedCells.empty();
    if (selectedTable) {
        requestedIds.clear();
        requestedIds.insert(selectedCells.begin(), selectedCells.end());
    }
    std::set<core::NodeId> selectionParagraphIds;
    if (scope == "selection" && !activeStory && !selectedTable && selected) {
        for (std::size_t index = selected->start_paragraph_index; index <= selected->end_paragraph_index; ++index)
            selectionParagraphIds.insert(snapshot.document.paragraphs()[index].id());
    }
    {
        const auto requestedMaximum = unsignedValue(arguments, "maxBlocks");
        const auto requestedOffset = unsignedValue(arguments, "blockOffset");
        if ((arguments.contains("maxBlocks") && (!requestedMaximum || *requestedMaximum < 1 || *requestedMaximum > 2048)) ||
            (arguments.contains("blockOffset") && !requestedOffset)) {
            error = QObject::tr("maxBlocks must be 1–2048 and blockOffset a nonnegative integer.");
            return {};
        }
        const auto maximumBlocks = requestedMaximum.value_or(512);
        const auto offset = requestedOffset.value_or(0);
        std::uint64_t ordinal = 0;
        std::optional<std::uint64_t> nextOffset;
        std::size_t metadataBytes = 0;
        const auto appendBlock = [&](const std::string& id, const char* type, const std::u16string& sourceText,
                                     const auto& attributes) {
            const auto current = ordinal++;
            if (current < offset) return true;
            if (blocks.size() >= maximumBlocks || (characters >= maximum && !sourceText.empty())) {
                truncated = true;
                nextOffset = current;
                return false;
            }
            QString boundedText = fromUtf16(sourceText);
            if (static_cast<std::size_t>(boundedText.size()) > maximum - characters) {
                truncateUtf16Safely(boundedText, static_cast<qsizetype>(maximum - characters));
                truncated = true;
            }
            const auto length = static_cast<std::size_t>(boundedText.size());
            codex::Json entry = {{"id", id}, {"type", type}, {"text", toUtf8(boundedText)}, {"attributes", attributes(length)}};
            const auto encodedSize = entry.dump().size();
            if (metadataBytes + encodedSize > 4U * 1024U * 1024U) {
                if (blocks.empty()) error = QObject::tr("READ_LIMIT: narrow this read with the images scope or explicit stable targets.");
                truncated = true;
                nextOffset = current;
                return false;
            }
            blocks.push_back(std::move(entry));
            metadataBytes += encodedSize;
            characters += length;
            return true;
        };
        const auto tableAttributes = [](const core::Table& table) {
            codex::Json widths = codex::Json::array(), heights = codex::Json::array();
            for (auto value : table.geometry().column_widths_twips) widths.push_back(value / 20.0);
            for (auto value : table.geometry().row_minimum_heights_twips) heights.push_back(value / 20.0);
            const char* names[] = {"plain", "grid", "light-gray", "light-blue", "light-orange", "medium-blue", "medium-green",
                "medium-orange", "aubergine", "orange-accent", "banded-blue", "banded-aubergine", "dark-header"};
            codex::Json style = table.style() ? codex::Json(names[static_cast<std::size_t>(*table.style())]) : codex::Json(nullptr);
            return codex::Json{{"rows", table.rowCount()}, {"columns", table.columnCount()}, {"headerRow", table.hasHeaderRow()},
                {"style", std::move(style)}, {"columnWidthsPoints", std::move(widths)}, {"rowMinimumHeightsPoints", std::move(heights)}};
        };
        const auto cellAttributes = [&](const core::Table& table, std::size_t row, std::size_t column, const core::TableCell& cell, std::size_t length) {
            auto attributes = includeFormatting ? formattingFor(cell, 0, length) : codex::Json::object();
            attributes["tableId"] = table.id().toString(); attributes["row"] = row; attributes["column"] = column;
            attributes["images"] = imagesFor(cell.images, 0, length, false, &remainingFigureContextBytes);
            if (selectedTable) {
                attributes["selectionStart"] = cellRange && cellRange->cellId == cell.id ? std::min(cellRange->start, length) : 0;
                attributes["selectionEnd"] = cellRange && cellRange->cellId == cell.id ? std::min(cellRange->end, length) : length;
            }
            if (includeFormatting) {
                const auto vertical = cell.layout.vertical_alignment;
                attributes["verticalAlignment"] = !vertical ? codex::Json(nullptr)
                    : codex::Json(*vertical == core::TableCellVerticalAlignment::top ? "top"
                        : *vertical == core::TableCellVerticalAlignment::center ? "center" : "bottom");
                const auto border = [](const std::optional<core::TableBorder>& edge) -> codex::Json {
                    if (!edge) return nullptr;
                    return {{"style", edge->style == core::TableBorderStyle::none ? "none" : "single"},
                        {"widthPoints", edge->width_eighth_points / 8.0},
                        {"color", QColor::fromRgba(edge->argb).name().toStdString()}};
                };
                attributes["borders"] = {{"top", border(cell.layout.border_top)}, {"right", border(cell.layout.border_right)},
                    {"bottom", border(cell.layout.border_bottom)}, {"left", border(cell.layout.border_left)}};
            }
            return attributes;
        };
        bool keepReading = true;
        if (!activeStory && scope != "stories" && scope != "images" && scope != "metadata") {
            for (const auto& body : snapshot.document.bodyBlocks()) {
                if (!keepReading) break;
                if (body.kind == core::BodyBlockKind::paragraph) {
                    if (scope == "tables" || selectedTable) continue;
                    const auto* paragraph = snapshot.document.findParagraph(body.id);
                    if (!paragraph || (scope == "blocks" && !requestedIds.contains(body.id)) ||
                        (scope == "selection" && !selectionParagraphIds.contains(body.id))) continue;
                    std::size_t returnedTextStart = 0;
                    auto returnedText = paragraph->text();
                    if (scope == "outline") {
                        const auto original = fromUtf16(returnedText);
                        qsizetype first = 0, last = original.size();
                        while (first < last && original.at(first).isSpace()) ++first;
                        while (last > first && original.at(last - 1).isSpace()) --last;
                        auto trimmed = original.mid(first, last - first);
                        truncateUtf16Safely(trimmed, 240);
                        returnedTextStart = static_cast<std::size_t>(first);
                        returnedText = trimmed.toStdU16String();
                    }
                    keepReading = appendBlock(body.id.toString(), "paragraph", returnedText, [&](std::size_t length) {
                        auto attributes = includeFormatting ? formattingFor(*paragraph, returnedTextStart, length) : codex::Json::object();
                        appendParagraphStyleMetadata(*paragraph, attributes);
                        attributes["equations"] = equationsFor(*paragraph, returnedTextStart, length);
                        attributes["images"] = imagesFor(paragraph->images(), returnedTextStart, length, false, &remainingFigureContextBytes);
                        appendAcademicMetadata(*paragraph, attributes, academic, returnedTextStart, length);
                        if (scope == "selection" && selected) {
                            const auto begin = paragraph->id() == selected->start.paragraph_id ? selected->start.utf16_offset : 0;
                            const auto finish = paragraph->id() == selected->end.paragraph_id ? selected->end.utf16_offset : paragraph->text().size();
                            attributes["selectionStart"] = std::min(begin, length);
                            attributes["selectionEnd"] = std::min(finish, length);
                        }
                        return attributes;
                    });
                } else if (body.kind == core::BodyBlockKind::table) {
                    if (scope == "outline" || (scope == "selection" && !selectedTable)) continue;
                    const auto* table = snapshot.document.findTable(body.id);
                    if (!table) continue;
                    const bool fullTable = !selectedTable && (scope != "blocks" || requestedIds.contains(body.id));
                    if (fullTable) keepReading = appendBlock(body.id.toString(), "table", {}, [&](std::size_t) { return tableAttributes(*table); });
                    for (std::size_t row = 0; keepReading && row < table->rowCount(); ++row)
                        for (std::size_t column = 0; keepReading && column < table->columnCount(); ++column) {
                            const auto* cell = table->cell(row, column);
                            if (!cell || (!fullTable && !requestedTableCells.contains({table->id(), cell->id}) && !requestedIds.contains(cell->id))) continue;
                            keepReading = appendBlock(cell->id.toString(), "tableCell", cell->text,
                                [&](std::size_t length) { return cellAttributes(*table, row, column, *cell, length); });
                        }
                } else if (body.kind == core::BodyBlockKind::table_of_contents && scope != "tables" && scope != "selection" && !selectedTable) {
                    if (scope == "blocks" && !requestedIds.contains(body.id)) continue;
                    for (const auto& toc : snapshot.document.tablesOfContents()) if (toc.id == body.id)
                        keepReading = appendBlock(toc.id.toString(), "tableOfContents", {}, [&](std::size_t) {
                            return codex::Json{{"maxLevel", toc.max_level}};
                        });
                }
            }
        }
        if (keepReading && (scope == "document" || scope == "blocks" || scope == "outline")) {
            for (const auto& note : snapshot.document.notes()) {
                if (!keepReading) break;
                if (scope == "blocks" && !requestedIds.contains(note.id)) continue;
                std::size_t number = 0;
                for (const auto& resolved : academic.notes) if (resolved.id == note.id) number = resolved.number;
                keepReading = appendBlock(note.id.toString(), "note", note.text, [&](std::size_t) {
                    return codex::Json{{"noteKind", note.kind == core::NoteKind::footnote ? "footnote" : "endnote"}, {"number", number}};
                });
            }
        }
        if (scope == "document" || scope == "stories" || activeStory) {
            for (const bool footer : {false, true}) {
                if (!keepReading) break;
                if (activeStory && activeStory->footer != footer) continue;
                const auto& story = footer ? snapshot.document.footerText() : snapshot.document.headerText();
                const auto& pictures = footer ? snapshot.document.footerImages() : snapshot.document.headerImages();
                if (story.empty() && pictures.empty() && scope != "stories" && !activeStory) continue;
                const char* name = footer ? "footer" : "header";
                keepReading = appendBlock(std::string("story:") + name, name, story, [&](std::size_t length) {
                    codex::Json attributes = {{"story", name}, {"images", imagesFor(pictures, 0, length, false, &remainingFigureContextBytes)},
                        {"regionSeparator", "\t"}, {"pageFields", codex::Json::array({"{PAGE}", "{PAGES}"})}};
                    if (activeStory) {
                        const char* regions[] = {"left", "center", "right"};
                        attributes["activeRegion"] = regions[activeStory->region];
                        attributes["regionSelectionStart"] = activeStory->start;
                        attributes["regionSelectionEnd"] = activeStory->end;
                        attributes["selectionOffsetUnit"] = "UTF-16 within activeRegion";
                    }
                    return attributes;
                });
            }
        }
        if (scope == "images") {
            std::set<std::string> imageIds;
            if (const auto requestedImages = arguments.find("imageIds"); requestedImages != arguments.end()) {
                if (!requestedImages->is_array() || requestedImages->size() > 64) { error = QObject::tr("imageIds must be an array of at most 64 stable IDs."); return {}; }
                for (const auto& encodedId : *requestedImages) {
                    if (!encodedId.is_string() || !core::NodeId::parse(encodedId.get<std::string>())) { error = QObject::tr("Invalid imageId."); return {}; }
                    imageIds.insert(encodedId.get<std::string>());
                }
            }
            const auto appendImages = [&](const std::vector<core::ImageAtom>& pictures, const std::string& owner, const char* story) {
                for (const auto& picture : pictures) {
                    if (!keepReading) break;
                    if (!imageIds.empty() && !imageIds.contains(picture.id.toString())) continue;
                    keepReading = appendBlock(picture.id.toString(), "image", {}, [&](std::size_t) {
                        auto attributes = imagesFor({picture}, 0, picture.utf16_offset + 1, !imageIds.empty(), &remainingFigureContextBytes).front();
                        attributes["ownerBlockId"] = owner;
                        if (story) attributes["story"] = story;
                        return attributes;
                    });
                }
            };
            for (const auto& paragraph : snapshot.document.paragraphs()) appendImages(paragraph.images(), paragraph.id().toString(), nullptr);
            for (const auto& table : snapshot.document.tables()) for (const auto& cell : table.cells()) appendImages(cell.images, cell.id.toString(), nullptr);
            appendImages(snapshot.document.headerImages(), "story:header", "header");
            appendImages(snapshot.document.footerImages(), "story:footer", "footer");
        }
        if (!error.isEmpty()) return {};
        codex::Json result = {{"documentId", toUtf8(documentId)}, {"revision", snapshot.revision.value()}, {"scope", scope},
            {"truncated", truncated}, {"blocks", std::move(blocks)}, {"pageLayout", pageLayoutFor(canvas)}};
        if (nextOffset) result["nextBlockOffset"] = *nextOffset;
        return result;
    }

}

codex::Json searchDocument(DocumentCanvas& canvas,
                           const QString& documentId,
                           const codex::Json& arguments,
                           QString& error) {
    if (!arguments.is_object() ||
        !validateDocumentId(documentId, arguments, error)) {
        return {};
    }

    const auto queryValue = arguments.find("query");
    if (queryValue == arguments.end() || !queryValue->is_string()) {
        error = QObject::tr("Editor search requires a nonempty query.");
        return {};
    }
    const std::string queryUtf8 = queryValue->get<std::string>();
    const QString query = QString::fromUtf8(
        queryUtf8.data(), static_cast<qsizetype>(queryUtf8.size()));
    if (query.toUtf8() != QByteArray(queryUtf8.data(),
                                     static_cast<qsizetype>(queryUtf8.size()))) {
        error = QObject::tr("Editor search requires a valid UTF-8 query.");
        return {};
    }
    if (query.isEmpty()) {
        error = QObject::tr("Editor search requires a nonempty query.");
        return {};
    }
    if (unicodeCodePointCount(query) > kMaximumSearchQueryCodePoints) {
        error = QObject::tr(
                    "Editor search queries are limited to %1 Unicode code points.")
                    .arg(kMaximumSearchQueryCodePoints);
        return {};
    }

    const auto booleanArgument = [&arguments, &error](
                                     const char* name, const bool fallback,
                                     bool& value) {
        const auto found = arguments.find(name);
        if (found == arguments.end()) {
            value = fallback;
            return true;
        }
        if (!found->is_boolean()) {
            error = QObject::tr("%1 must be true or false.")
                        .arg(QString::fromLatin1(name));
            return false;
        }
        value = found->get<bool>();
        return true;
    };
    bool caseSensitive = false;
    bool wholeWord = false;
    if (!booleanArgument("caseSensitive", false, caseSensitive) ||
        !booleanArgument("wholeWord", false, wholeWord)) {
        return {};
    }

    std::size_t maximum = 100;
    if (const auto found = arguments.find("maxResults");
        found != arguments.end()) {
        const auto requested = unsignedValue(arguments, "maxResults");
        if (!requested || *requested == 0 ||
            *requested > kMaximumSearchResults) {
            error = QObject::tr("maxResults must be between 1 and %1.")
                        .arg(kMaximumSearchResults);
            return {};
        }
        maximum = static_cast<std::size_t>(*requested);
    }

    const auto snapshot = canvas.snapshot();
    if (const auto found = arguments.find("expectedRevision");
        found != arguments.end()) {
        const auto expected = unsignedValue(arguments, "expectedRevision");
        if (!expected) {
            error = QObject::tr("expectedRevision must be a nonnegative integer.");
            return {};
        }
        if (*expected != snapshot.revision.value()) {
            error = QObject::tr("REVISION_CONFLICT: expected %1, current %2")
                        .arg(*expected)
                        .arg(snapshot.revision.value());
            return {};
        }
    }

    codex::Json results = codex::Json::array();
    bool truncated = false;
    const Qt::CaseSensitivity sensitivity =
        caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;

    const auto searchText = [&](const QString& text,
                                const std::string& blockId,
                                const std::optional<std::string>& tableId,
                                const std::optional<std::string>& cellId,
                                const std::optional<std::size_t> row,
                                const std::optional<std::size_t> column) {
        // QTextBoundaryFinder builds a boundary table for the complete input.
        // Build each table once per block, not once per candidate match.
        QTextBoundaryFinder wordBoundaries(QTextBoundaryFinder::Word, text);
        QTextBoundaryFinder graphemeBoundaries(QTextBoundaryFinder::Grapheme,
                                               text);
        qsizetype offset = 0;
        while (offset <= text.size() - query.size()) {
            const qsizetype start = text.indexOf(query, offset, sensitivity);
            if (start < 0) break;
            const qsizetype end = start + query.size();
            offset = end > start ? end : start + 1;
            // Keep every returned semantic offset safe to reuse. Paragraph
            // hits may become preview targets, while table-cell hits currently
            // feed the typed read target; neither should split a grapheme.
            if (!isBoundaryAt(graphemeBoundaries, start) ||
                !isBoundaryAt(graphemeBoundaries, end)) {
                continue;
            }
            if (wholeWord &&
                !isWholeWordMatch(wordBoundaries, start, end)) {
                continue;
            }
            if (results.size() >= maximum) {
                truncated = true;
                return false;
            }
            const SearchContext context = searchContextFor(
                text, start, end, graphemeBoundaries);
            codex::Json match = {
                {"blockId", blockId},
                {"blockType", tableId ? "tableCell" : "paragraph"},
                {"start", static_cast<std::size_t>(start)},
                {"end", static_cast<std::size_t>(end)},
                {"context", toUtf8(context.text)},
                {"contextStart", context.start},
                {"contextEnd", context.end},
            };
            if (tableId && cellId && row && column) {
                match["tableId"] = *tableId;
                match["cellId"] = *cellId;
                match["row"] = *row;
                match["column"] = *column;
            }
            results.push_back(std::move(match));
        }
        return true;
    };

    for (const core::BodyBlockRef& block : snapshot.document.bodyBlocks()) {
        if (block.kind == core::BodyBlockKind::paragraph) {
            const core::Paragraph* paragraph =
                snapshot.document.findParagraph(block.id);
            if (!paragraph) continue;
            if (!searchText(fromUtf16(paragraph->text()), block.id.toString(),
                            std::nullopt, std::nullopt, std::nullopt,
                            std::nullopt)) {
                break;
            }
            continue;
        }

        const core::Table* table = snapshot.document.findTable(block.id);
        if (!table) continue;
        const std::string tableId = table->id().toString();
        bool keepSearching = true;
        for (std::size_t row = 0;
             keepSearching && row < table->rowCount(); ++row) {
            for (std::size_t column = 0;
                 column < table->columnCount(); ++column) {
                const core::TableCell* cell = table->cell(row, column);
                if (!cell) continue;
                if (!searchText(fromUtf16(cell->text), cell->id.toString(),
                                tableId,
                                cell->id.toString(), row, column)) {
                    keepSearching = false;
                    break;
                }
            }
        }
        if (!keepSearching) break;
    }

    return {{"documentId", toUtf8(documentId)},
            {"revision", snapshot.revision.value()},
            {"query", toUtf8(query)},
            {"caseSensitive", caseSensitive},
            {"wholeWord", wholeWord},
            {"truncated", truncated},
            {"results", std::move(results)}};
}

bool appendTextStyle(const codex::Json& operation, const Target& target,
                     std::vector<core::Operation>& operations, QString& error) {
    const auto style = operation.find("style");
    if (style == operation.end() || !style->is_object() || !target.end) {
        error = QObject::tr("set_text_style requires a style and target end offset.");
        return false;
    }
    core::CharacterFormatDelta delta;
    if (!parseEditorCharacterFormat(*style, delta, error)) return false;
    if (delta.empty()) {
        error = QObject::tr("set_text_style contains no supported style property.");
        return false;
    }
    operations.push_back(core::SetCharacterFormat{
        {{target.block, target.start}, {target.block, *target.end}}, delta});
    return true;
}

bool appendParagraphStyle(const codex::Json& operation, const Target& target,
                          const core::Document& working,
                          const DocumentCanvas& canvas,
                          std::vector<core::Operation>& operations,
                          QString& error) {
    const auto style = operation.find("style");
    if (style == operation.end() || !style->is_object()) {
        error = QObject::tr("set_paragraph_style requires a style object.");
        return false;
    }
    const auto* paragraph = working.findParagraph(target.block);
    if (!paragraph) {
        error = QObject::tr(
            "set_paragraph_style must target a paragraph block.");
        return false;
    }

    // Body and cell formatting share one validated paragraph-format parser;
    // the native style plan below retains body style identity/provenance.
    core::ParagraphFormatDelta delta;
    if (!parseEditorParagraphFormat(*style, delta, error)) return false;

    bool changesIdentity = false;
    std::optional<std::string> styleId;
    const core::ParagraphStyleDefinition* definition = nullptr;
    if (const auto value = style->find("styleId"); value != style->end()) {
        changesIdentity = true;
        if (value->is_null()) {
            styleId.reset();
        } else if (value->is_string()) {
            const auto& encoded = value->get_ref<const std::string&>();
            const auto validation = core::validateParagraphStyleId(encoded);
            if (!validation) {
                error = QString::fromStdString(validation.error().message);
                return false;
            }
            styleId = encoded;
            definition = core::findBuiltInParagraphStyle(encoded);
        } else {
            error = QObject::tr("styleId must be a string or null.");
            return false;
        }
        if (definition) {
            auto planned = canvas.planParagraphStyleOperations(
                working, {target.block}, *definition, !delta.empty());
            operations.insert(
                operations.end(),
                std::make_move_iterator(planned.begin()),
                std::make_move_iterator(planned.end()));
            // A missing style ID is semantically implicit Normal. Preserve an
            // explicit Codex request to assign Normal even when no visual
            // transition or provenance initialization is necessary.
            if (planned.empty() && paragraph->styleId() != styleId) {
                operations.push_back(
                    core::SetParagraphStyle{{target.block}, styleId});
            }
        } else {
            operations.push_back(
                core::SetParagraphStyle{{target.block}, styleId});
        }
    } else if (!delta.empty()) {
        // Formatting-only requests still need a baseline when the current
        // paragraph has an implicit or legacy provenance-free built-in style.
        // Otherwise an explicit value equal to that baseline cannot remain
        // distinguishable from inheritance during the next style transition.
        const std::string currentStyleId =
            paragraph->styleId().value_or("Normal");
        if (const auto* currentDefinition =
                core::findBuiltInParagraphStyle(currentStyleId)) {
            auto planned = canvas.planParagraphStyleOperations(
                working, {target.block}, *currentDefinition, true);
            operations.insert(
                operations.end(),
                std::make_move_iterator(planned.begin()),
                std::make_move_iterator(planned.end()));
        }
    }

    if (!delta.empty()) {
        operations.push_back(core::SetParagraphFormat{{target.block}, delta});
    }

    if (!changesIdentity && delta.empty()) {
        error = QObject::tr("set_paragraph_style contains no supported property.");
        return false;
    }
    return true;
}

bool appendWorkingOperation(core::Document& working,
                            std::vector<core::Operation>& operations,
                            core::Operation operation,
                            QString& error) {
    const auto applied = core::applyDocumentOperation(working, operation);
    if (!applied) {
        error = QObject::tr("Editor operation %1 is invalid: %2")
                    .arg(operations.size() + 1)
                    .arg(QString::fromStdString(applied.error().message));
        return false;
    }
    operations.push_back(std::move(operation));
    return true;
}

std::optional<core::NodeId> paragraphContainingImage(
    const core::Document& document, const core::NodeId imageId) {
    for (const auto& paragraph : document.paragraphs()) {
        if (std::any_of(paragraph.images().begin(), paragraph.images().end(),
                        [&](const auto& image) { return image.id == imageId; })) {
            return paragraph.id();
        }
    }
    return std::nullopt;
}

std::optional<ExcalidrawFigureEditor::Result> renderFigure(
    const codex::Json& source, const QString& defaultAccessibleName,
    QString& accessibleName,
    std::optional<double>& widthPoints, std::optional<double>& heightPoints,
    QString& error, const ExcalidrawRenderer& figureRenderer) {
    codex::Json figure = codex::Json::object();
    if (const auto nested = source.find("figure");
        nested != source.end() && nested->is_object()) {
        figure = *nested;
    }
    const codex::Json* elements = nullptr;
    if (const auto nestedElements = figure.find("elements");
        nestedElements != figure.end()) {
        elements = &*nestedElements;
    } else if (const auto topLevelElements = source.find("elements");
               topLevelElements != source.end()) {
        elements = &*topLevelElements;
    }
    if (!elements) {
        error = QObject::tr("An Excalidraw operation requires an elements array.");
        return std::nullopt;
    }
    const auto normalizedElements = normalizeExcalidrawElements(*elements, error);
    if (!normalizedElements) return std::nullopt;

    const auto assignName = [&](const codex::Json& object) {
        const auto name = object.find("accessibleName");
        if (name == object.end() || !name->is_string()) return false;
        const std::string encoded = name->get<std::string>();
        accessibleName = QString::fromUtf8(
            encoded.data(), static_cast<qsizetype>(encoded.size())).left(500);
        return !accessibleName.trimmed().isEmpty();
    };
    if (!assignName(figure) && !assignName(source)) {
        accessibleName = defaultAccessibleName.trimmed().left(500);
        if (accessibleName.isEmpty())
            accessibleName = QObject::tr("Editable Excalidraw figure");
    }
    const auto dimension = [&](const char* canonicalKey,
                               const char* pixelAlias,
                               std::optional<double>& output) -> bool {
        const codex::Json* value = nullptr;
        bool pixels = false;
        const auto findValue = [&](const codex::Json& object,
                                   const char* key) -> const codex::Json* {
            const auto found = object.find(key);
            return found == object.end() ? nullptr : &*found;
        };
        value = findValue(figure, canonicalKey);
        if (!value) value = findValue(source, canonicalKey);
        if (!value) {
            value = findValue(figure, pixelAlias);
            pixels = value != nullptr;
        }
        if (!value) {
            value = findValue(source, pixelAlias);
            pixels = value != nullptr;
        }
        if (!value) return true;
        if (!value->is_number()) return false;
        double points = value->get<double>();
        if (pixels) points *= 0.75;
        if (!std::isfinite(points) || points < 36.0 || points > 936.0)
            return false;
        output = points;
        return true;
    };
    if (!dimension("widthPoints", "width", widthPoints) ||
        !dimension("heightPoints", "height", heightPoints)) {
        error = QObject::tr("Excalidraw figure dimensions are outside the supported range.");
        return std::nullopt;
    }
    const std::string serialized = normalizedElements->dump();
    if (serialized.size() > 256U * 1024U) {
        error = QObject::tr("The Excalidraw element request is too large.");
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(
        QByteArray(serialized.data(), static_cast<qsizetype>(serialized.size())),
        &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        error = QObject::tr("The Excalidraw element request is invalid.");
        return std::nullopt;
    }
    return figureRenderer ? figureRenderer(document.array(), error)
                          : renderProfessionalExcalidrawSkeleton(document.array(), error);
}

core::EncodedImagePayload imagePayload(const QByteArray& bytes) {
    return core::EncodedImagePayload(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            bytes.size()));
}

std::int64_t pointsToEmu(const double points) {
    return static_cast<std::int64_t>(std::llround(points * 12700.0));
}

bool appendCurrentBuiltInStyleProvenance(
    core::Document& working, const DocumentCanvas& canvas,
    core::NodeId paragraphId, std::vector<core::Operation>& operations,
    QString& error) {
    const auto* paragraph = working.findParagraph(paragraphId);
    if (!paragraph || paragraph->styleProvenance()) return true;
    const std::string styleId = paragraph->styleId().value_or("Normal");
    const auto* definition = core::findBuiltInParagraphStyle(styleId);
    if (!definition) return true;

    auto planned = canvas.planParagraphStyleOperations(
        working, {paragraphId}, *definition, true);
    for (auto& operation : planned) {
        if (!appendWorkingOperation(
                working, operations, std::move(operation), error)) {
            return false;
        }
    }
    return true;
}

bool academicToolText(const codex::Json& source, const char* key, QString& text,
                      QString& error, std::size_t limit, bool required = true) {
    const auto value = source.find(key);
    if (value == source.end()) {
        if (!required) { text.clear(); return true; }
        error = QObject::tr("The academic operation requires %1.").arg(QString::fromLatin1(key)); return false;
    }
    if (!value->is_string()) { error = QObject::tr("%1 must be text.").arg(QString::fromLatin1(key)); return false; }
    const auto bytes = value->get<std::string>();
    if (bytes.size() > limit * 4U) { error = QObject::tr("Academic text exceeds the size limit."); return false; }
    text = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    if (text.toUtf8().toStdString() != bytes || static_cast<std::size_t>(text.size()) > limit || text.contains(QChar(core::kInlineObjectReplacementCharacter))) {
        error = QObject::tr("Academic text is invalid or exceeds the size limit."); return false;
    }
    return true;
}
std::optional<core::NodeId> academicToolId(const codex::Json& source, const char* key, QString& error) {
    const auto value = source.find(key);
    const auto id = value != source.end() && value->is_string() ? core::NodeId::parse(value->get<std::string>()) : std::nullopt;
    if (!id) error = QObject::tr("%1 must be a valid stable identifier.").arg(QString::fromLatin1(key));
    return id;
}
bool appendAcademicParagraph(core::Document& working, const DocumentCanvas& canvas,
                             const Target& target, bool bibliography, core::CaptionLabel label,
                             core::NodeId objectId, const QString& text,
                             std::vector<core::Operation>& operations, QString& error) {
    const auto paragraph = core::NodeId::generate();
    const auto following = core::NodeId::generate();
    if (!appendWorkingOperation(working, operations, core::SplitParagraph{{target.block, target.start}, paragraph}, error) ||
        !appendWorkingOperation(working, operations, core::SplitParagraph{{paragraph, 0}, following}, error)) return false;
    if (const auto* normal = core::findBuiltInParagraphStyle("Normal")) {
        auto planned = canvas.planParagraphStyleOperations(working, {paragraph}, *normal, true);
        for (auto& operation : planned) if (!appendWorkingOperation(working, operations, std::move(operation), error)) return false;
    }
    if (bibliography) {
        if (!appendWorkingOperation(working, operations, core::SetBibliographyEntry{paragraph, true}, error)) return false;
    } else if (!appendWorkingOperation(working, operations, core::SetCaption{paragraph, core::CaptionMetadata{label, objectId}}, error)) return false;
    if (!appendWorkingOperation(working, operations, core::InsertField{{paragraph, 0},
        {core::NodeId::generate(), 0, bibliography ? core::FieldKind::bibliography_number : core::FieldKind::caption_number, paragraph}, std::nullopt}, error)) return false;
    return appendWorkingOperation(working, operations, core::InsertText{{paragraph, 1},
        (bibliography ? QStringLiteral(". ") : QStringLiteral(": ")) .append(normalizeInlineText(text)).toStdU16String(), std::nullopt}, error);
}

codex::Json createPreview(DocumentCanvas& canvas,
                          const QString& documentId,
                          const codex::Json& arguments,
                          QString& previewSummary,
                          QString& error, const ExcalidrawRenderer& figureRenderer,
                          const std::function<bool()>& stillCurrent,
                          const ExcalidrawSceneRenderer& nativeFigureRenderer) {
    if (!arguments.is_object() || !validateDocumentId(documentId, arguments, error)) {
        return {};
    }
    const auto revision = unsignedValue(arguments, "baseRevision");
    const auto sourceOperations = arguments.find("operations");
    const auto labelValue = arguments.find("summary");
    if (!revision || sourceOperations == arguments.end() || !sourceOperations->is_array() ||
        sourceOperations->empty() || sourceOperations->size() > 256 ||
        labelValue == arguments.end() || !labelValue->is_string()) {
        error = QObject::tr("The preview request is missing a valid revision, summary, or operations array.");
        return {};
    }
    std::vector<core::Operation> operations;
    const auto baseSnapshot = canvas.snapshot();
    if (*revision != baseSnapshot.revision.value()) {
        error = QObject::tr("REVISION_CONFLICT: expected %1, current %2")
                    .arg(*revision)
                    .arg(baseSnapshot.revision.value());
        return {};
    }
    auto working = baseSnapshot.document;
    operations.reserve(sourceOperations->size());
    std::set<std::string> affected;
    std::set<core::NodeId> touchedFigureIds;
    std::vector<std::string> warnings;
    std::size_t figureOperationCount = 0;
    std::size_t rasterInputBytes = 0;
    bool refreshNumbering = false;
    std::optional<PageLayoutSettings> proposedLayout;
    std::optional<std::uint64_t> expectedLayoutState;
    for (const auto& rawSource : *sourceOperations) {
        if (!rawSource.is_object()) {
            error = QObject::tr("Every preview operation must be an object.");
            return {};
        }
        codex::Json source = rawSource;
        const std::string kind = operationKind(source);
        source["kind"] = kind;
        if (kind == "set_page_layout") {
            const auto state = unsignedValue(source, "expectedLayoutStateId");
            const auto layout = source.find("layout");
            if (proposedLayout || !state || layout == source.end() || !layout->is_object() || layout->size() != 6) {
                error = QObject::tr("A single page-layout proposal requires its complete geometry and read stateId.");
                return {};
            }
            PageLayoutSettings candidate;
            const std::array<std::pair<const char*, double*>, 6> fields{{
                {"widthPoints", &candidate.widthPoints}, {"heightPoints", &candidate.heightPoints},
                {"marginTopPoints", &candidate.marginTopPoints}, {"marginRightPoints", &candidate.marginRightPoints},
                {"marginBottomPoints", &candidate.marginBottomPoints}, {"marginLeftPoints", &candidate.marginLeftPoints}}};
            for (const auto& [key, value] : fields) {
                const auto found = layout->find(key);
                if (found == layout->end() || !found->is_number() || !std::isfinite(found->get<double>())) {
                    error = QObject::tr("Page geometry must contain six finite point measurements.");
                    return {};
                }
                *value = found->get<double>();
            }
            if (*state != pageLayoutFor(canvas).at("stateId").get<std::uint64_t>()) {
                error = QObject::tr("LAYOUT_CONFLICT: Page settings changed. Read metadata and retry.");
                return {};
            }
            expectedLayoutState = *state;
            proposedLayout = candidate;
            continue;
        }
        if (kind == "set_list" || kind == "set_list_level" || kind == "set_list_layout") {
            const auto ids = source.find("blockIds");
            ListEditRequest request;
            if (ids == source.end() || !ids->is_array() || ids->empty() || ids->size() > 2048) {
                error = QObject::tr("List edits require a bounded array of body paragraph identities.");
                return {};
            }
            for (const auto& id : *ids) {
                const auto parsed = id.is_string() ? core::NodeId::parse(id.get<std::string>()) : std::nullopt;
                if (!parsed) { error = QObject::tr("A list paragraph identity is invalid."); return {}; }
                request.blockIds.push_back(*parsed);
                affected.insert(parsed->toString());
            }
            if (kind == "set_list") {
                const auto value = source.find("listKind");
                if (value == source.end() || !value->is_string()) { error = QObject::tr("A list kind is required."); return {}; }
                request.listKind = QString::fromStdString(value->get<std::string>());
            } else if (kind == "set_list_level") {
                request.kind = ListEditRequest::Kind::set_level;
                const auto value = unsignedValue(source, "level");
                if (!value || *value < 1 || *value > 10) { error = QObject::tr("List levels run from 1 through 10."); return {}; }
                request.level = static_cast<int>(*value);
            } else {
                request.kind = ListEditRequest::Kind::set_layout;
                const auto levels = source.find("levels");
                if (levels == source.end() || !levels->is_array() || levels->size() != core::kListLevelCount) {
                    error = QObject::tr("List layout requires all ten level settings."); return {};
                }
                for (std::size_t index = 0; index < core::kListLevelCount; ++index) {
                    const auto& level = levels->at(index);
                    const auto bullet = unsignedValue(level, "bulletIndentSpaces"), text = unsignedValue(level, "textIndentSpaces");
                    if (!bullet || *bullet > 400 || !text || *text > 64) { error = QObject::tr("List indentation is outside supported bounds."); return {}; }
                    request.layout.levels[index].bullet_indent_spaces = static_cast<std::int32_t>(*bullet);
                    request.layout.levels[index].text_indent_spaces = static_cast<std::int32_t>(*text);
                }
            }
            std::vector<core::Operation> planned;
            if (!canvas.planListOperations(working, request, planned, error)) return {};
            for (auto& operation : planned) if (!appendWorkingOperation(working, operations, std::move(operation), error)) return {};
            continue;
        }
        if (handlesSemanticOperation(kind)) {
            refreshNumbering = refreshNumbering || kind == "move_block" || kind == "split_paragraph" || kind == "merge_paragraph" ||
                kind == "insert_table" || kind == "delete_table";
            const auto previousOperations = operations.size();
            if (!appendSemanticOperation(working, source, operations, affected, error)) return {};
            if (kind == "insert_raster_image") {
                for (std::size_t index = previousOperations; index < operations.size(); ++index)
                    std::visit([&](const auto& inserted) {
                        using Inserted = std::decay_t<decltype(inserted)>;
                        if constexpr (std::is_same_v<Inserted, core::InsertImage> || std::is_same_v<Inserted, core::InsertTableCellImage> ||
                                      std::is_same_v<Inserted, core::InsertHeaderFooterImage>) rasterInputBytes += inserted.encoded_payload.size();
                    }, operations[index]);
                if (rasterInputBytes > 4U * 1024U * 1024U) {
                    error = QObject::tr("Codex raster input is limited to 4 MiB per preview.");
                    return {};
                }
            }
            continue;
        }
        if (kind == "insert_excalidraw_figure") {
            codex::Json target = codex::Json::object();
            if (const auto supplied = source.find("target");
                supplied != source.end() && supplied->is_object()) {
                target = *supplied;
            }
            if (!target.contains("blockId") && target.contains("paragraphId"))
                target["blockId"] = target["paragraphId"];
            if (!target.contains("start")) {
                for (const char* alias : {"offset", "utf16Offset", "startOffset"}) {
                    if (target.contains(alias)) {
                        target["start"] = target[alias];
                        break;
                    }
                }
            }
            const auto cursor = canvas.selection().focus;
            if (!target.contains("story") && !target.contains("blockId"))
                target["blockId"] = cursor.paragraph_id.toString();
            if (!target.contains("start")) {
                const auto targetId = target.find("blockId");
                const auto parsed = targetId != target.end() && targetId->is_string()
                    ? core::NodeId::parse(targetId->get<std::string>())
                    : std::nullopt;
                target["start"] = parsed && *parsed == cursor.paragraph_id
                    ? cursor.utf16_offset
                    : 0;
            }
            source["target"] = std::move(target);
        }
        const bool needsEnd = kind == "replace_text" || kind == "delete_range" ||
                              kind == "set_text_style";
        Target target;
        const bool storyFigure = kind == "insert_excalidraw_figure" && source.at("target").contains("story");
        const bool needsTarget = !storyFigure && kind != "patch_excalidraw_figure" && kind != "replace_excalidraw_figure" && kind != "replace_equation" &&
            kind != "set_note_text" && kind != "configure_toc" && kind != "delete_toc";
        if (needsTarget) {
            if (!parseTarget(source, target, error, needsEnd)) return {};
            affected.insert(target.block.toString());
        }
        if (kind == "insert_text" || kind == "replace_text") {
            const auto text = source.find("text");
            if (text == source.end() || !text->is_string()) {
                error = QObject::tr("A text operation has no text value.");
                return {};
            }
            const QString normalized = normalizeInlineText(
                QString::fromStdString(text->get<std::string>()));
            if (normalized.contains(QChar::LineSeparator))
                warnings.push_back("Line breaks are represented inline in this foundation build.");
            if (kind == "insert_text") {
                if (!appendWorkingOperation(
                        working, operations,
                        core::InsertText{{target.block, target.start},
                                         normalized.toStdU16String(), std::nullopt},
                        error)) {
                    return {};
                }
            } else {
                std::optional<core::CharacterFormat> sourceFormat;
                if (const auto* paragraph =
                        working.findParagraph(target.block);
                    paragraph && target.end && target.start < *target.end &&
                    *target.end <= paragraph->text().size()) {
                    // A present-but-empty format means explicit default
                    // formatting. A disengaged optional means "inherit after
                    // deletion", which can incorrectly recolor a default run
                    // from the following formatted run at a boundary.
                    sourceFormat = paragraph->characterFormatAt(
                        target.start + 1);
                }
                if (!appendWorkingOperation(
                        working, operations,
                        core::ReplaceRange{
                            {{target.block, target.start},
                             {target.block, *target.end}},
                            normalized.toStdU16String(), std::move(sourceFormat)},
                        error)) {
                    return {};
                }
            }
        } else if (kind == "delete_range") {
            if (!appendWorkingOperation(
                    working, operations,
                    core::DeleteRange{{{target.block, target.start},
                                       {target.block, *target.end}}},
                    error)) {
                return {};
            }
        } else if (kind == "set_text_style") {
            std::vector<core::Operation> generated;
            if (!appendTextStyle(source, target, generated, error)) return {};
            if (!appendCurrentBuiltInStyleProvenance(
                    working, canvas, target.block, operations, error)) {
                return {};
            }
            if (!appendWorkingOperation(working, operations,
                                        std::move(generated.front()), error)) {
                return {};
            }
        } else if (kind == "set_paragraph_style") {
            std::vector<core::Operation> generated;
            if (!appendParagraphStyle(
                    source, target, working, canvas, generated, error)) {
                return {};
            }
            for (auto& operation : generated) {
                if (!appendWorkingOperation(
                        working, operations, std::move(operation), error)) {
                    return {};
                }
            }
        } else if (kind == "insert_page_break") {
            const auto newId = core::NodeId::generate();
            if (!appendWorkingOperation(
                    working, operations,
                    core::SplitParagraph{{target.block, target.start}, newId},
                    error)) {
                return {};
            }
            core::ParagraphFormatDelta delta;
            delta.page_break_before = core::PropertyDelta<bool>::set(true);
            if (!appendWorkingOperation(
                    working, operations,
                    core::SetParagraphFormat{{newId}, delta}, error)) {
                return {};
            }
            affected.insert(newId.toString());
        } else if (kind == "insert_note") {
            QString text;
            QString noteKind;
            if (!academicToolText(source, "text", text, error, core::kMaximumNoteCodeUnits) ||
                !academicToolText(source, "noteKind", noteKind, error, 16, false)) return {};
            if (noteKind.isEmpty()) noteKind = QStringLiteral("footnote");
            if (noteKind != QStringLiteral("footnote") && noteKind != QStringLiteral("endnote")) {
                error = QObject::tr("Note kind must be footnote or endnote."); return {};
            }
            const auto note = core::NodeId::generate();
            if (!appendWorkingOperation(working, operations,
                core::SetNote{{note, noteKind == QStringLiteral("footnote") ? core::NoteKind::footnote : core::NoteKind::endnote, text.toStdU16String()}}, error) ||
                !appendWorkingOperation(working, operations,
                core::InsertField{{target.block, target.start}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, note}, std::nullopt}, error)) return {};
        } else if (kind == "set_note_text") {
            const auto id = academicToolId(source, "noteId", error);
            QString text;
            if (!id || !academicToolText(source, "text", text, error, core::kMaximumNoteCodeUnits)) return {};
            const auto* note = working.findNote(*id);
            if (!note) { error = QObject::tr("The note does not exist."); return {}; }
            if (!appendWorkingOperation(working, operations, core::SetNote{{*id, note->kind, text.toStdU16String()}}, error)) return {};
            affected.insert(id->toString());
        } else if (kind == "replace_equation") {
            const auto id = academicToolId(source, "equationId", error);
            QString latex;
            if (!id || !academicToolText(source, "latex", latex, error, 8192)) return {};
            const auto* equation = working.findEquation(*id);
            if (!equation) { error = QObject::tr("The equation does not exist."); return {}; }
            bool display = equation->display;
            if (const auto value = source.find("display"); value != source.end()) {
                if (!value->is_boolean()) { error = QObject::tr("display must be a boolean."); return {}; }
                display = value->get<bool>();
            }
            math::ParseLimits limits;
            limits.max_input_bytes = 8192; limits.max_depth = 32; limits.max_nodes = 2048;
            limits.max_matrix_rows = 32; limits.max_matrix_columns = 32;
            const auto parsed = math::parseLatex(latex.toUtf8().toStdString(), limits);
            if (!parsed) { error = QString::fromStdString(parsed.error().message); return {}; }
            for (const auto& paragraph : working.paragraphs()) if (paragraph.equationAt(equation->utf16_offset) &&
                paragraph.equationAt(equation->utf16_offset)->id == *id) affected.insert(paragraph.id().toString());
            if (!appendWorkingOperation(working, operations, core::ReplaceEquation{*id, math::toCanonicalLatex(parsed.value()), display}, error)) return {};
        } else if (kind == "insert_cross_reference" || kind == "insert_citation") {
            const bool citation = kind == "insert_citation";
            const auto id = academicToolId(source, citation ? "entryId" : "targetId", error);
            if (!id) return {};
            const auto* paragraph = working.findParagraph(*id);
            const auto* style = paragraph && paragraph->styleId() ? core::findBuiltInParagraphStyle(*paragraph->styleId()) : nullptr;
            if (!paragraph || (citation ? !paragraph->isBibliographyEntry() : (!paragraph->caption() && !(style && style->outline_level)))) {
                error = QObject::tr("The reference target is not a supported heading, caption, or bibliography entry."); return {};
            }
            QString locator, format;
            if (!academicToolText(source, "locator", locator, error, 512, false) ||
                !academicToolText(source, "format", format, error, 32, false)) return {};
            if (!format.isEmpty() && format != QStringLiteral("label_number") && format != QStringLiteral("full_text")) {
                error = QObject::tr("Cross-reference format is unsupported."); return {};
            }
            core::FieldAtom field{core::NodeId::generate(), 0, citation ? core::FieldKind::citation : core::FieldKind::cross_reference,
                *id, locator.toStdU16String(), format == QStringLiteral("full_text") ? core::CrossReferenceFormat::full_text : core::CrossReferenceFormat::label_number};
            if (!appendWorkingOperation(working, operations, core::InsertField{{target.block, target.start}, std::move(field), std::nullopt}, error)) return {};
        } else if (kind == "insert_caption" || kind == "insert_bibliography_entry") {
            QString text, label;
            const bool bibliography = kind == "insert_bibliography_entry";
            if (!academicToolText(source, "text", text, error, 32768) ||
                !academicToolText(source, "label", label, error, 16, false)) return {};
            core::CaptionLabel captionLabel = core::CaptionLabel::figure;
            if (!label.isEmpty() && label != QStringLiteral("Figure") && label != QStringLiteral("Table") && label != QStringLiteral("Equation")) {
                error = QObject::tr("Caption label is unsupported."); return {};
            }
            if (label == QStringLiteral("Table")) captionLabel = core::CaptionLabel::table;
            else if (label == QStringLiteral("Equation")) captionLabel = core::CaptionLabel::equation;
            auto objectId = target.block;
            if (source.contains("objectId")) {
                const auto parsed = academicToolId(source, "objectId", error);
                if (!parsed) return {};
                if (!working.findParagraph(*parsed) && !working.findTable(*parsed) && !working.findImage(*parsed) && !working.findEquation(*parsed)) {
                    error = QObject::tr("Caption object does not exist."); return {};
                }
                objectId = *parsed;
            }
            if (!appendAcademicParagraph(working, canvas, target, bibliography, captionLabel, objectId, text, operations, error)) return {};
        } else if (kind == "insert_toc" || kind == "configure_toc" || kind == "delete_toc") {
            std::optional<core::NodeId> id;
            if (kind != "insert_toc") { id = academicToolId(source, "tocId", error); if (!id) return {}; }
            if (kind == "delete_toc") {
                if (!appendWorkingOperation(working, operations, core::DeleteToc{*id}, error)) return {};
            } else {
                const auto requested = unsignedValue(source, "maxLevel");
                if ((source.contains("maxLevel") && !requested) || (requested && (*requested < 1 || *requested > 9))) {
                    error = QObject::tr("TOC maxLevel must be 1–9."); return {};
                }
                const auto level = static_cast<std::uint8_t>(requested.value_or(3));
                if (kind == "configure_toc") {
                    if (!appendWorkingOperation(working, operations, core::ConfigureToc{*id, level}, error)) return {};
                } else {
                    auto before = target.block;
                    if (target.start != 0) {
                        before = core::NodeId::generate();
                        if (!appendWorkingOperation(working, operations, core::SplitParagraph{{target.block, target.start}, before}, error)) return {};
                    }
                    if (!appendWorkingOperation(working, operations, core::InsertToc{before, {core::NodeId::generate(), level}}, error)) return {};
                }
            }
        } else if (kind == "insert_equation") {
            const auto latex = source.find("latex");
            if (latex == source.end() || !latex->is_string()) {
                error = QObject::tr("insert_equation has no LaTeX source.");
                return {};
            }
            bool display = false;
            if (const auto displayValue = source.find("display");
                displayValue != source.end()) {
                if (!displayValue->is_boolean()) {
                    error = QObject::tr("insert_equation display must be a boolean.");
                    return {};
                }
                display = displayValue->get<bool>();
            }
            const std::string source_latex = latex->get<std::string>();
            math::ParseLimits limits;
            limits.max_input_bytes = 8U * 1024U;
            limits.max_depth = 32;
            limits.max_nodes = 2'048;
            limits.max_matrix_rows = 32;
            limits.max_matrix_columns = 32;
            const auto parsed = math::parseLatex(source_latex, limits);
            if (!parsed) {
                error = QObject::tr("Invalid equation at byte %1: %2")
                            .arg(static_cast<qulonglong>(parsed.error().byte_offset))
                            .arg(QString::fromStdString(parsed.error().message));
                return {};
            }
            if (!appendWorkingOperation(
                    working, operations,
                    core::InsertEquation{
                        {target.block, target.start},
                        math::toCanonicalLatex(parsed.value()), display,
                        core::NodeId::generate(), std::nullopt},
                    error)) {
                return {};
            }
        } else if (kind == "patch_excalidraw_figure") {
            if (++figureOperationCount > 8) { error = QObject::tr("A preview may create or revise at most eight Excalidraw figures."); return {}; }
            const auto encodedId = source.find("imageId");
            const auto imageId = encodedId != source.end() && encodedId->is_string() ? core::NodeId::parse(encodedId->get<std::string>()) : std::nullopt;
            const auto* current = imageId ? working.findImage(*imageId) : nullptr;
            if (!current) { error = QObject::tr("The editable Excalidraw image no longer exists."); return {}; }
            const auto originalBytes = current->encoded_payload.bytes();
            const QByteArray originalPng(reinterpret_cast<const char*>(originalBytes.data()), static_cast<qsizetype>(originalBytes.size()));
            const auto scene = current->format == core::ImageFormat::png ? excalidrawSceneFromPng(originalPng) : std::nullopt;
            if (!scene) { error = QObject::tr("A native patch requires an editable Excalidraw figure."); return {}; }
            const auto edits = source.find("edits");
            if (edits == source.end() || !edits->is_array()) { error = QObject::tr("A native figure patch requires an edits array."); return {}; }
            const auto editBytes = QByteArray::fromStdString(edits->dump());
            const auto patch = prepareExcalidrawScenePatch(*scene, QJsonDocument::fromJson(editBytes).array(), error);
            if (!patch) return {};
            const QPointer<DocumentCanvas> guardedCanvas(&canvas);
            const auto rendered = nativeFigureRenderer ? nativeFigureRenderer(patch->scene, patch->renderElementIds, error)
                : renderProfessionalExcalidrawScene(patch->scene, patch->renderElementIds, error);
            if (!guardedCanvas) { error = QObject::tr("The document closed while its figure was rendering."); return {}; }
            if (stillCurrent && !stillCurrent()) { error = QObject::tr("The Codex session changed; its figure preview was canceled."); return {}; }
            if (!rendered) { if (error.isEmpty()) error = QObject::tr("The local Excalidraw renderer did not return a figure."); return {}; }
            const auto authoritativeScene = finishExcalidrawScenePatch(*patch, rendered->scene, error);
            if (authoritativeScene.isEmpty()) return {};
            const auto authoritativePng = pngWithExcalidrawScene(rendered->png, authoritativeScene, error);
            const auto image = QImage::fromData(authoritativePng, "PNG");
            const auto display = fitExcalidrawFigureDisplaySize(image.size(), static_cast<double>(current->width_emu) / 12700.0, std::nullopt);
            if (authoritativePng.isEmpty() || !display) { if (error.isEmpty()) error = QObject::tr("The native renderer returned an invalid figure preview."); return {}; }
            if (const auto owner = paragraphContainingImage(working, *imageId)) affected.insert(owner->toString());
            for (const auto& table : working.tables()) for (const auto& cell : table.cells())
                for (const auto& picture : cell.images) if (picture.id == *imageId) affected.insert(cell.id.toString());
            for (const auto& picture : working.headerImages()) if (picture.id == *imageId) affected.insert("story:header");
            for (const auto& picture : working.footerImages()) if (picture.id == *imageId) affected.insert("story:footer");
            if (!appendWorkingOperation(working, operations, core::ReplaceImagePayload{*imageId, imagePayload(authoritativePng),
                core::ImageFormat::png, pointsToEmu(display->width()), pointsToEmu(display->height())}, error)) return {};
            touchedFigureIds.insert(*imageId);
        } else if (kind == "insert_excalidraw_figure" ||
                   kind == "replace_excalidraw_figure") {
            if (++figureOperationCount > 8) {
                error = QObject::tr(
                    "A preview may create or revise at most eight Excalidraw figures.");
                return {};
            }
            std::optional<core::NodeId> imageId;
            const core::ImageAtom* current = nullptr;
            std::optional<bool> footerStory;
            std::optional<core::NodeId> targetTable;
            std::size_t targetRow = 0;
            std::size_t targetColumn = 0;
            if (kind == "insert_excalidraw_figure") {
                if (const auto suppliedId = source.find("imageId"); suppliedId != source.end()) {
                    imageId = suppliedId->is_string() ? core::NodeId::parse(suppliedId->get<std::string>()) : std::nullopt;
                    if (!imageId) { error = QObject::tr("An inserted figure requires a valid imageId."); return {}; }
                } else imageId = core::NodeId::generate();
                if (storyFigure) {
                    const auto& storyTarget = source.at("target");
                    const auto story = storyTarget.find("story");
                    const auto start = unsignedValue(storyTarget, "start");
                    if (story == storyTarget.end() || !story->is_string() ||
                        (*story != "header" && *story != "footer") || !start) {
                        error = QObject::tr("A figure story target requires header or footer and a valid UTF-16 start."); return {};
                    }
                    footerStory = *story == "footer";
                    target.start = *start;
                    affected.insert(*footerStory ? "story:footer" : "story:header");
                } else {
                    for (const auto& table : working.tables()) {
                        for (std::size_t row = 0; row < table.rowCount(); ++row) {
                            for (std::size_t column = 0; column < table.columnCount(); ++column) {
                                if (table.cell(row, column)->id == target.block) {
                                    targetTable = table.id(); targetRow = row; targetColumn = column;
                                }
                            }
                        }
                    }
                }
            }
            const auto insertion = [&](const QByteArray& png, const QString& name,
                                       const std::int64_t width, const std::int64_t height) -> core::Operation {
                if (footerStory) return core::InsertHeaderFooterImage{*footerStory, target.start,
                    imagePayload(png), core::ImageFormat::png, toUtf8(name), width, height, *imageId};
                if (targetTable) return core::InsertTableCellImage{*targetTable, targetRow, targetColumn, target.start,
                    imagePayload(png), core::ImageFormat::png, toUtf8(name), width, height, *imageId};
                return core::InsertImage{{target.block, target.start}, imagePayload(png), core::ImageFormat::png,
                    toUtf8(name), width, height, *imageId};
            };
            if (kind == "insert_excalidraw_figure") {
                // Exercise the same core insertion before starting a nested renderer:
                // this checks stable identities, story/cell UTF-16 anchors and quotas.
                QImage probeImage(1, 1, QImage::Format_ARGB32);
                probeImage.fill(Qt::transparent);
                QByteArray probePng;
                QBuffer buffer(&probePng);
                buffer.open(QIODevice::WriteOnly);
                if (!probeImage.save(&buffer, "PNG")) { error = QObject::tr("Could not validate the figure destination."); return {}; }
                auto probeDocument = working;
                const auto valid = core::applyDocumentOperation(probeDocument, insertion(probePng, QStringLiteral("Figure"), 12700, 12700));
                if (!valid) { error = QObject::tr("Invalid figure destination: %1").arg(QString::fromStdString(valid.error().message)); return {}; }
            }
            if (kind == "replace_excalidraw_figure") {
                const auto encodedId = source.find("imageId");
                if (encodedId == source.end() || !encodedId->is_string()) {
                    error = QObject::tr("replace_excalidraw_figure requires an imageId.");
                    return {};
                }
                imageId = core::NodeId::parse(encodedId->get<std::string>());
                current = imageId ? working.findImage(*imageId) : nullptr;
                if (!imageId || !current) {
                    error = QObject::tr("The editable Excalidraw image no longer exists.");
                    return {};
                }
                const auto currentBytes = current->encoded_payload.bytes();
                const QByteArray currentPng(
                    reinterpret_cast<const char*>(currentBytes.data()),
                    static_cast<qsizetype>(currentBytes.size()));
                const auto originalScene = current->format == core::ImageFormat::png ? excalidrawSceneFromPng(currentPng) : std::nullopt;
                if (!originalScene) {
                    error = QObject::tr(
                        "replace_excalidraw_figure can only revise an editable Excalidraw figure.");
                    return {};
                }
                const auto reconstruct = source.find("reconstructScene");
                if (reconstruct != source.end() && !reconstruct->is_boolean()) { error = QObject::tr("reconstructScene must be true or false."); return {}; }
                const bool reconstructScene = reconstruct != source.end() && reconstruct->get<bool>();
                const auto inspected = inspectExcalidrawScene(*originalScene);
                if (!reconstructScene && !inspected.complete()) {
                    error = QObject::tr("This scene has native properties that a replacement cannot preserve. Use patch_excalidraw_figure, or explicitly reconstruct the scene."); return {};
                }
                const codex::Json* elements = nullptr;
                if (const auto figure = source.find("figure"); figure != source.end() && figure->is_object() && figure->contains("elements")) elements = &figure->at("elements");
                else if (source.contains("elements")) elements = &source.at("elements");
                if (!elements) { error = QObject::tr("An Excalidraw operation requires an elements array."); return {}; }
                const auto normalized = normalizeExcalidrawElements(*elements, error);
                if (!normalized) return {};
                std::set<std::string> incomingIds;
                for (const auto& element : *normalized) {
                    incomingIds.insert(element.at("id").get<std::string>());
                    if (element.contains("label") && element.at("label").contains("id")) incomingIds.insert(element.at("label").at("id").get<std::string>());
                }
                QStringList omitted;
                for (const auto& value : QJsonDocument::fromJson(*originalScene).object().value(QStringLiteral("elements")).toArray()) {
                    const auto element = value.toObject(); const auto id = element.value(QStringLiteral("id")).toString();
                    if (!element.value(QStringLiteral("isDeleted")).toBool() && !id.isEmpty() && !incomingIds.contains(toUtf8(id))) omitted.append(id);
                }
                if (!reconstructScene && !omitted.isEmpty()) {
                    error = QObject::tr("Replacement must retain every original scene and label identity. Use a native patch or an explicit reconstruction. Missing: %1").arg(omitted.join(QStringLiteral(", "))); return {};
                }
                if (reconstructScene) {
                    QString warning = QObject::tr("Explicit reconstruction recreates native scene structure and styling.");
                    if (!omitted.isEmpty()) warning += QObject::tr(" Omitted identities: %1.").arg(omitted.mid(0, 16).join(QStringLiteral(", ")));
                    if (omitted.size() > 16) warning += QObject::tr(" %1 additional identities are omitted.").arg(omitted.size() - 16);
                    if (!inspected.complete()) warning += QObject::tr(" Native editing limits: %1").arg(inspected.unsupported.mid(0, 8).join(QStringLiteral("; ")));
                    if (inspected.truncated) warning += QObject::tr(" The original source exceeds the canonical element limit.");
                    warnings.push_back(toUtf8(warning));
                }
            }
            QString accessibleName;
            std::optional<double> widthPoints;
            std::optional<double> heightPoints;
            const QPointer<DocumentCanvas> guardedCanvas(&canvas);
            const auto rendered = renderFigure(
                source, current ? QString::fromStdString(current->accessible_name) : QString::fromStdString(labelValue->get<std::string>()),
                accessibleName, widthPoints, heightPoints, error, figureRenderer);
            if (!guardedCanvas) {
                error = QObject::tr("The document closed while its figure was rendering.");
                return {};
            }
            if (stillCurrent && !stillCurrent()) {
                error = QObject::tr("The Codex session changed; its figure preview was canceled.");
                return {};
            }
            if (!rendered) {
                if (error.isEmpty())
                    error = QObject::tr("The local Excalidraw renderer did not return a figure.");
                return {};
            }
            const QImage previewImage = QImage::fromData(rendered->png, "PNG");
            if (previewImage.isNull() || previewImage.width() <= 0 ||
                previewImage.height() <= 0) {
                error = QObject::tr("The local Excalidraw renderer returned an invalid PNG.");
                return {};
            }
            if (kind == "insert_excalidraw_figure") {
                const auto displaySize = fitExcalidrawFigureDisplaySize(
                    previewImage.size(), widthPoints, heightPoints);
                if (!displaySize) {
                    error = QObject::tr(
                        "The rendered Excalidraw figure has invalid display dimensions.");
                    return {};
                }
                if (!appendWorkingOperation(
                        working, operations,
                        insertion(rendered->png, accessibleName,
                            pointsToEmu(displaySize->width()),
                            pointsToEmu(displaySize->height())),
                        error)) {
                    return {};
                }
            } else {
                std::int64_t width = current->width_emu;
                std::int64_t height = current->height_emu;
                const std::string previousAccessibleName =
                    current->accessible_name;
                if (widthPoints || heightPoints) {
                    const auto displaySize = fitExcalidrawFigureDisplaySize(
                        previewImage.size(), widthPoints, heightPoints);
                    if (!displaySize) {
                        error = QObject::tr(
                            "The rendered Excalidraw figure has invalid display dimensions.");
                        return {};
                    }
                    width = pointsToEmu(displaySize->width());
                    height = pointsToEmu(displaySize->height());
                } else {
                    const auto displaySize = fitExcalidrawFigureDisplaySize(previewImage.size(), static_cast<double>(width) / 12700.0, std::nullopt);
                    if (!displaySize) { error = QObject::tr("The rendered figure has invalid display dimensions."); return {}; }
                    width = pointsToEmu(displaySize->width()); height = pointsToEmu(displaySize->height());
                }
                if (const auto owner = paragraphContainingImage(working, *imageId))
                    affected.insert(owner->toString());
                if (!appendWorkingOperation(
                        working, operations,
                        core::ReplaceImagePayload{
                            *imageId, imagePayload(rendered->png),
                            core::ImageFormat::png, width, height},
                        error)) {
                    return {};
                }
                if (previousAccessibleName != toUtf8(accessibleName) &&
                    !appendWorkingOperation(
                        working, operations,
                        core::SetImageAccessibleName{
                            *imageId, toUtf8(accessibleName)}, error)) {
                    return {};
                }
            }
            touchedFigureIds.insert(*imageId);
        } else if (kind == "insert_image") {
            error = QObject::tr("No user-granted image capability is attached to this request.");
            return {};
        } else {
            error = QObject::tr("Unsupported editor operation: %1")
                        .arg(QString::fromStdString(kind));
            return {};
        }
    }
    if (stillCurrent && !stillCurrent()) {
        error = QObject::tr("The Codex session changed; its preview was canceled.");
        return {};
    }
    // Several public routes (page breaks, captions, bibliography and TOCs)
    // emit paragraph splits internally. Resequence their native list runs
    // with the same baseline as explicit structural operations.
    refreshNumbering = refreshNumbering || std::any_of(operations.begin(), operations.end(), [](const auto& operation) {
        return std::holds_alternative<core::SplitParagraph>(operation);
    });
    if (refreshNumbering) {
        ListEditRequest request;
        request.kind = ListEditRequest::Kind::refresh_numbering;
        request.numberingBaseline = &baseSnapshot.document;
        std::vector<core::Operation> planned;
        if (!canvas.planListOperations(working, request, planned, error)) return {};
        for (auto& operation : planned) {
            if (const auto* changed = std::get_if<core::ReplaceRange>(&operation)) affected.insert(changed->range.anchor.paragraph_id.toString());
            if (!appendWorkingOperation(working, operations, std::move(operation), error)) return {};
        }
    }
    // Fit only figures authored by this proposal, after all geometry and page
    // operations have run. Later table resizing must not use an earlier cell
    // width or leave oversized stored dimensions that Word renders differently.
    std::vector<core::Operation> figureCellResizes;
    for (const auto& table : working.tables()) for (const auto& cell : table.cells()) {
        for (const auto& picture : cell.images) {
            if (!touchedFigureIds.contains(picture.id)) continue;
            const auto fitted = canvas.fitTableCellImageSize(working, table.id(), cell.id,
                QSizeF(static_cast<double>(picture.width_emu) / 12700.0, static_cast<double>(picture.height_emu) / 12700.0), proposedLayout);
            if (!fitted) { error = QObject::tr("The figure's table-cell writing area is unavailable."); return {}; }
            const auto width = pointsToEmu(fitted->width()), height = pointsToEmu(fitted->height());
            if (width != picture.width_emu || height != picture.height_emu)
                figureCellResizes.emplace_back(core::ResizeImage{picture.id, width, height});
            affected.insert(cell.id.toString());
        }
    }
    for (auto& operation : figureCellResizes) if (!appendWorkingOperation(working, operations, std::move(operation), error)) return {};
    if (expectedLayoutState && *expectedLayoutState != pageLayoutFor(canvas).at("stateId").get<std::uint64_t>()) {
        error = QObject::tr("LAYOUT_CONFLICT: Page settings changed while preparing the proposal. Read metadata and retry.");
        return {};
    }
    const QString label = QString::fromStdString(labelValue->get<std::string>()).left(512);
    if (!canvas.createOperationsPreview(core::Revision(*revision), operations,
                                        label, previewSummary, error, proposedLayout)) {
        return {};
    }
    for (const auto& warning : warnings) previewSummary += QStringLiteral("\n\n") + QString::fromStdString(warning);
    codex::Json affectedIds = codex::Json::array();
    for (const auto& id : affected) affectedIds.push_back(id);
    return {{"previewId", canvas.previewIdString().toStdString()},
            {"documentId", toUtf8(documentId)},
            {"baseRevision", *revision},
            {"committed", false},
            {"summary", toUtf8(label)},
            {"affectedBlockIds", std::move(affectedIds)},
            {"warnings", std::move(warnings)}};
}

}  // namespace

codex::Json invokeEditorTool(DocumentCanvas& canvas,
                             const QString& documentId,
                             const QString& tool,
                             const codex::Json& arguments,
                             QString& previewSummary,
                             QString& error,
                             const ExcalidrawRenderer& figureRenderer,
                             const std::function<bool()>& stillCurrent,
                             const ExcalidrawSceneRenderer& nativeFigureRenderer) {
    previewSummary.clear();
    error.clear();
    if (!arguments.is_object()) {
        error = QObject::tr("Editor tool arguments must be an object.");
        return {};
    }
    if (tool == QString::fromLatin1(codex::kEditorCapabilitiesTool.data(), static_cast<qsizetype>(codex::kEditorCapabilitiesTool.size()))) {
        if (!validateDocumentId(documentId, arguments, error)) return {};
        const auto snapshot = canvas.snapshot();
        if (arguments.contains("expectedRevision")) {
            const auto expected = unsignedValue(arguments, "expectedRevision");
            if (!expected || *expected != snapshot.revision.value()) {
                error = QObject::tr("REVISION_CONFLICT: Read capabilities for the current document revision.");
                return {};
            }
        }
        auto result = codex::editorV1Capabilities();
        result["documentId"] = toUtf8(documentId);
        result["revision"] = snapshot.revision.value();
        codex::Json styles = codex::Json::array();
        for (const auto& style : core::builtInParagraphStyles()) {
            codex::Json encoded = {{"id", style.id}, {"name", style.display_name}};
            if (style.outline_level) encoded["outlineLevel"] = *style.outline_level;
            styles.push_back(std::move(encoded));
        }
        result["runtime"] = {{"previewPending", canvas.hasPreview()}, {"pageLayout", pageLayoutFor(canvas)},
            {"editorDefaults", {{"fontFamily", toUtf8(canvas.defaultFontFamily())}, {"fontSizePoints", canvas.defaultFontPointSize()},
                                {"tabWidthSpaces", canvas.tabWidthSpaces()}}}, {"paragraphStyles", std::move(styles)}};
        return result;
    }
    if (tool == QString::fromLatin1(codex::kEditorReadTool.data(),
                                    static_cast<qsizetype>(codex::kEditorReadTool.size()))) {
        return readDocument(canvas, documentId, arguments, error);
    }
    if (tool == QString::fromLatin1(
                    codex::kEditorSearchTool.data(),
                    static_cast<qsizetype>(codex::kEditorSearchTool.size()))) {
        return searchDocument(canvas, documentId, arguments, error);
    }
    if (tool == QString::fromLatin1(codex::kEditorPreviewTool.data(),
                                    static_cast<qsizetype>(codex::kEditorPreviewTool.size()))) {
        return createPreview(canvas, documentId, arguments, previewSummary, error, figureRenderer, stillCurrent, nativeFigureRenderer);
    }
    if (tool == QString::fromLatin1(codex::kEditorFileCapabilityTool.data(),
                                    static_cast<qsizetype>(codex::kEditorFileCapabilityTool.size()))) {
        error = QObject::tr("This document has no user-granted file capability.");
        return {};
    }
    error = QObject::tr("Unknown editor tool.");
    return {};
}

}  // namespace docxstudio::app
