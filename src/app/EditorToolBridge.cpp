#include "docxstudio/app/EditorToolBridge.h"

#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/math/latex_parser.h"

#include <QRegularExpression>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
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
    if (found->is_number_integer() && found->get<std::int64_t>() < 0) {
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
            if (const auto label = input.find("label");
                label != input.end() && label->is_object()) {
                const auto text = label->find("text");
                if (text != label->end() && text->is_string() &&
                    !text->get<std::string>().empty()) {
                    element["label"] = {{"text", text->get<std::string>()}};
                    if (const auto size = finiteNumber(*label, "fontSize"))
                        element["label"]["fontSize"] =
                            std::clamp(*size, 8.0, 96.0);
                }
            }
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
        result.push_back(std::move(element));
    }
    return std::optional<codex::Json>(std::move(result));
}

std::optional<std::uint32_t> parseRgb(const codex::Json& style,
                                      const char* name,
                                      QString& error) {
    const auto found = style.find(name);
    if (found == style.end()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        error = QObject::tr("%1 must be a #RRGGBB color.")
                    .arg(QString::fromLatin1(name));
        return std::nullopt;
    }
    const QString encoded = QString::fromStdString(found->get<std::string>());
    static const QRegularExpression pattern(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    if (!pattern.match(encoded).hasMatch()) {
        error = QObject::tr("%1 must be a #RRGGBB color.")
                    .arg(QString::fromLatin1(name));
        return std::nullopt;
    }
    bool valid = false;
    const std::uint32_t rgb = encoded.mid(1).toUInt(&valid, 16);
    if (!valid) {
        error = QObject::tr("%1 contains an invalid color.")
                    .arg(QString::fromLatin1(name));
        return std::nullopt;
    }
    return 0xff000000U | rgb;
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
    return {{"alignment", alignmentName(paragraphFormat)},
            {"characterRuns", std::move(runs)}};
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

codex::Json imagesFor(const core::Paragraph& paragraph,
                      std::size_t returnedTextStart,
                      std::size_t returnedTextLength) {
    codex::Json images = codex::Json::array();
    const std::size_t returnedTextEnd = returnedTextStart + returnedTextLength;
    for (const auto& image : paragraph.images()) {
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
                encodedImage["authoringMode"] = "professional";
                QJsonParseError parseError;
                const auto sceneDocument =
                    QJsonDocument::fromJson(*scene, &parseError);
                if (parseError.error == QJsonParseError::NoError &&
                    sceneDocument.isObject()) {
                    const auto sourceElements =
                        sceneDocument.object().value(
                            QStringLiteral("elements")).toArray();
                    codex::Json elements = codex::Json::array();
                    const qsizetype maximum =
                        std::min<qsizetype>(sourceElements.size(), 128);
                    for (qsizetype index = 0; index < maximum; ++index) {
                        const auto source = sourceElements.at(index).toObject();
                        codex::Json element = codex::Json::object();
                        const auto copyString = [&](const char* name) {
                            const QString key = QString::fromLatin1(name);
                            if (source.value(key).isString())
                                element[name] = toUtf8(source.value(key).toString());
                        };
                        const auto copyNumber = [&](const char* name) {
                            const QString key = QString::fromLatin1(name);
                            if (source.value(key).isDouble())
                                element[name] = source.value(key).toDouble();
                        };
                        copyString("id"); copyString("type");
                        copyString("text"); copyString("strokeColor");
                        copyString("backgroundColor");
                        copyNumber("x"); copyNumber("y");
                        copyNumber("width"); copyNumber("height");
                        copyNumber("fontSize"); copyNumber("strokeWidth");
                        copyNumber("opacity");
                        if (source.value(QStringLiteral("points")).isArray()) {
                            const QByteArray points = QJsonDocument(
                                source.value(QStringLiteral("points")).toArray())
                                                          .toJson(QJsonDocument::Compact);
                            element["points"] = codex::Json::parse(
                                points.constData(),
                                points.constData() + points.size());
                        }
                        elements.push_back(std::move(element));
                    }
                    encodedImage["editableElements"] = std::move(elements);
                    encodedImage["editableElementsTruncated"] =
                        sourceElements.size() > maximum;
                }
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
        "selection", "document", "outline", "blocks"};
    if (!supportedScopes.contains(scope)) {
        error = QObject::tr("Unsupported editor read scope: %1. Supported scopes are selection, document, outline, and blocks.")
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
    bool truncated = false;
    if (scope == "blocks") {
        bool keepReading = true;
        for (const core::BodyBlockRef& bodyBlock :
             snapshot.document.bodyBlocks()) {
            if (!keepReading) break;
            if (bodyBlock.kind == core::BodyBlockKind::paragraph) {
                const core::Paragraph* paragraph =
                    snapshot.document.findParagraph(bodyBlock.id);
                if (!paragraph || !requestedIds.contains(paragraph->id())) {
                    continue;
                }
                if (characters >= maximum) {
                    truncated = true;
                    break;
                }
                QString text = fromUtf16(paragraph->text());
                const std::size_t remaining = maximum - characters;
                if (static_cast<std::size_t>(text.size()) > remaining) {
                    truncateUtf16Safely(
                        text, static_cast<qsizetype>(remaining));
                    truncated = true;
                    keepReading = false;
                }
                const std::size_t textLength =
                    static_cast<std::size_t>(text.size());
                codex::Json attributes = includeFormatting
                                             ? formattingFor(*paragraph, 0,
                                                             textLength)
                                             : codex::Json::object();
                appendParagraphStyleMetadata(*paragraph, attributes);
                attributes["equations"] =
                    equationsFor(*paragraph, 0, textLength);
                attributes["images"] = imagesFor(*paragraph, 0, textLength);
                blocks.push_back({{"id", paragraph->id().toString()},
                                  {"type", "paragraph"},
                                  {"text", toUtf8(text)},
                                  {"attributes", std::move(attributes)}});
                characters += textLength;
                continue;
            }

            const core::Table* table =
                snapshot.document.findTable(bodyBlock.id);
            if (!table) continue;
            for (std::size_t row = 0;
                 keepReading && row < table->rowCount(); ++row) {
                for (std::size_t column = 0;
                     column < table->columnCount(); ++column) {
                    const core::TableCell* cell = table->cell(row, column);
                    if (!cell ||
                        !requestedTableCells.contains(
                            std::pair{table->id(), cell->id})) {
                        continue;
                    }
                    if (characters >= maximum) {
                        truncated = true;
                        keepReading = false;
                        break;
                    }
                    QString text = fromUtf16(cell->text);
                    const std::size_t remaining = maximum - characters;
                    if (static_cast<std::size_t>(text.size()) > remaining) {
                        truncateUtf16Safely(
                            text, static_cast<qsizetype>(remaining));
                        truncated = true;
                        keepReading = false;
                    }
                    const std::size_t textLength =
                        static_cast<std::size_t>(text.size());
                    codex::Json attributes =
                        includeFormatting
                            ? formattingFor(*cell, 0, textLength)
                            : codex::Json::object();
                    attributes["tableId"] = table->id().toString();
                    attributes["row"] = row;
                    attributes["column"] = column;
                    blocks.push_back({{"id", cell->id.toString()},
                                      {"type", "tableCell"},
                                      {"text", toUtf8(text)},
                                      {"attributes", std::move(attributes)}});
                    characters += textLength;
                    if (!keepReading) break;
                }
            }
        }
        return {{"documentId", toUtf8(documentId)},
                {"revision", snapshot.revision.value()},
                {"scope", scope},
                {"truncated", truncated},
                {"blocks", std::move(blocks)}};
    }

    for (std::size_t index = 0; index < snapshot.document.paragraphs().size(); ++index) {
        const auto& paragraph = snapshot.document.paragraphs()[index];
        if (scope == "selection" && selected &&
            (index < selected->start_paragraph_index || index > selected->end_paragraph_index)) {
            continue;
        }
        QString text = fromUtf16(paragraph.text());
        std::size_t returnedTextStart = 0;
        if (scope == "outline") {
            qsizetype first = 0;
            while (first < text.size() && text.at(first).isSpace()) ++first;
            qsizetype last = text.size();
            while (last > first && text.at(last - 1).isSpace()) --last;
            returnedTextStart = static_cast<std::size_t>(first);
            text = text.mid(first, last - first);
            truncateUtf16Safely(text, 240);
        }
        if (characters >= maximum) {
            truncated = true;
            break;
        }
        const std::size_t remaining = maximum - characters;
        if (static_cast<std::size_t>(text.size()) > remaining) {
            truncateUtf16Safely(text, static_cast<qsizetype>(remaining));
            truncated = true;
        }
        codex::Json block = {{"id", paragraph.id().toString()},
                             {"type", "paragraph"},
                             {"text", toUtf8(text)}};
        const std::size_t returnedTextLength = static_cast<std::size_t>(text.size());
        if (includeFormatting) {
            block["attributes"] = formattingFor(
                paragraph, returnedTextStart, returnedTextLength);
        }
        else block["attributes"] = codex::Json::object();
        appendParagraphStyleMetadata(paragraph, block["attributes"]);
        block["attributes"]["equations"] = equationsFor(
            paragraph, returnedTextStart, returnedTextLength);
        block["attributes"]["images"] = imagesFor(
            paragraph, returnedTextStart, returnedTextLength);
        if (scope == "selection" && selected) {
            const std::size_t start = index == selected->start_paragraph_index
                                          ? selected->start.utf16_offset : 0;
            const std::size_t end = index == selected->end_paragraph_index
                                        ? selected->end.utf16_offset
                                        : paragraph.text().size();
            block["attributes"]["selectionStart"] =
                std::min(start, returnedTextLength);
            block["attributes"]["selectionEnd"] =
                std::min(end, returnedTextLength);
        }
        blocks.push_back(std::move(block));
        characters += static_cast<std::size_t>(text.size());
        if (truncated) break;
    }
    return {{"documentId", toUtf8(documentId)},
            {"revision", snapshot.revision.value()},
            {"scope", scope},
            {"truncated", truncated},
            {"blocks", std::move(blocks)}};
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
    if (const auto value = style->find("fontFamily"); value != style->end() && value->is_string())
        delta.font_family = core::PropertyDelta<std::string>::set(value->get<std::string>());
    if (const auto value = style->find("fontSizePoints"); value != style->end() && value->is_number()) {
        const double points = value->get<double>();
        if (!(points > 0.0 && points <= 1000.0)) {
            error = QObject::tr("fontSizePoints is outside the supported range.");
            return false;
        }
        delta.font_size_half_points = core::PropertyDelta<std::int32_t>::set(
            static_cast<std::int32_t>(std::llround(points * 2.0)));
    }
    if (const auto value = style->find("bold"); value != style->end() && value->is_boolean())
        delta.bold = core::PropertyDelta<bool>::set(value->get<bool>());
    if (const auto value = style->find("italic"); value != style->end() && value->is_boolean())
        delta.italic = core::PropertyDelta<bool>::set(value->get<bool>());
    if (const auto value = style->find("underline"); value != style->end() && value->is_boolean())
        delta.underline = core::PropertyDelta<core::UnderlineStyle>::set(
            value->get<bool>() ? core::UnderlineStyle::single : core::UnderlineStyle::none);
    if (const auto value = style->find("strike"); value != style->end() && value->is_boolean())
        delta.strike = core::PropertyDelta<bool>::set(value->get<bool>());
    if (style->contains("foregroundColor")) {
        const auto color = parseRgb(*style, "foregroundColor", error);
        if (!color) return false;
        delta.foreground_argb = core::PropertyDelta<std::uint32_t>::set(*color);
    }
    if (style->contains("highlightColor")) {
        const auto color = parseRgb(*style, "highlightColor", error);
        if (!color) return false;
        delta.highlight_argb = core::PropertyDelta<std::uint32_t>::set(*color);
    }
    if (const auto value = style->find("verticalAlign");
        value != style->end() && value->is_string()) {
        const auto name = value->get<std::string>();
        const auto baseline = name == "superscript" ? core::BaselinePosition::superscript
                              : name == "subscript" ? core::BaselinePosition::subscript
                                                     : core::BaselinePosition::normal;
        delta.baseline = core::PropertyDelta<core::BaselinePosition>::set(baseline);
    }
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

    for (auto property = style->begin(); property != style->end(); ++property) {
        const std::string& name = property.key();
        if (name != "styleId" && name != "alignment" &&
            name != "lineSpacing" && name != "spaceBeforePoints" &&
            name != "spaceAfterPoints" && name != "keepWithNext") {
            error = QObject::tr(
                        "set_paragraph_style contains an unsupported property: %1")
                        .arg(QString::fromStdString(name));
            return false;
        }
    }

    // Explicit tool properties are appended after the native style plan.
    // They therefore override its inherited baseline and are recorded as
    // direct properties by the attached target provenance.
    core::ParagraphFormatDelta delta;
    if (const auto value = style->find("alignment"); value != style->end()) {
        if (!value->is_string()) {
            error = QObject::tr("alignment must be a string.");
            return false;
        }
        const auto name = value->get<std::string>();
        const auto alignment = name == "left"
            ? std::optional{core::ParagraphAlignment::left}
            : name == "center"
            ? std::optional{core::ParagraphAlignment::center}
            : name == "right"
            ? std::optional{core::ParagraphAlignment::right}
            : name == "justify"
            ? std::optional{core::ParagraphAlignment::justified}
            : std::nullopt;
        if (!alignment) {
            error = QObject::tr(
                "alignment must be left, center, right, or justify.");
            return false;
        }
        delta.alignment =
            core::PropertyDelta<core::ParagraphAlignment>::set(*alignment);
    }
    if (const auto value = style->find("lineSpacing"); value != style->end()) {
        if (!value->is_number()) {
            error = QObject::tr("lineSpacing must be a number.");
            return false;
        }
        const double multiple = value->get<double>();
        if (!std::isfinite(multiple) || multiple <= 0.0 || multiple > 20.0) {
            error = QObject::tr("lineSpacing is outside the supported range.");
            return false;
        }
        delta.line_spacing_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(multiple * 12.0 * 12700.0)));
        delta.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(
            core::LineSpacingRule::automatic);
    }
    if (const auto value = style->find("spaceBeforePoints");
        value != style->end()) {
        if (!value->is_number()) {
            error = QObject::tr("spaceBeforePoints must be a number.");
            return false;
        }
        const double points = value->get<double>();
        if (!std::isfinite(points) || points < 0.0 || points > 10000.0) {
            error = QObject::tr(
                "spaceBeforePoints is outside the supported range.");
            return false;
        }
        delta.space_before_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(points * 12700.0)));
    }
    if (const auto value = style->find("spaceAfterPoints");
        value != style->end()) {
        if (!value->is_number()) {
            error = QObject::tr("spaceAfterPoints must be a number.");
            return false;
        }
        const double points = value->get<double>();
        if (!std::isfinite(points) || points < 0.0 || points > 10000.0) {
            error = QObject::tr(
                "spaceAfterPoints is outside the supported range.");
            return false;
        }
        delta.space_after_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(points * 12700.0)));
    }
    if (const auto value = style->find("keepWithNext");
        value != style->end()) {
        if (!value->is_boolean()) {
            error = QObject::tr("keepWithNext must be a boolean.");
            return false;
        }
        delta.keep_with_next = core::PropertyDelta<bool>::set(value->get<bool>());
    }

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
    const auto applied = std::visit(
        [&working](const auto& typed) -> core::Result<void> {
            using Type = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Type, core::InsertText>) {
                return working.insertText(typed.position, typed.text, typed.format);
            } else if constexpr (std::is_same_v<Type, core::InsertEquation>) {
                return working.insertEquation(
                    typed.position, typed.canonical_latex, typed.display,
                    typed.equation_id, typed.format);
            } else if constexpr (std::is_same_v<Type, core::InsertImage>) {
                return working.insertImage(
                    typed.position, typed.encoded_payload, typed.image_format,
                    typed.accessible_name, typed.width_emu, typed.height_emu,
                    typed.image_id, typed.character_format, typed.layout);
            } else if constexpr (
                std::is_same_v<Type, core::ReplaceImagePayload>) {
                return working.replaceImagePayload(
                    typed.image_id, typed.encoded_payload,
                    typed.image_format, typed.width_emu, typed.height_emu);
            } else if constexpr (
                std::is_same_v<Type, core::SetImageAccessibleName>) {
                return working.setImageAccessibleName(
                    typed.image_id, typed.accessible_name);
            } else if constexpr (std::is_same_v<Type, core::DeleteRange>) {
                return working.deleteRange(
                    typed.range, typed.empty_paragraph_format);
            } else if constexpr (std::is_same_v<Type, core::ReplaceRange>) {
                return working.replaceRange(typed.range, typed.text, typed.format);
            } else if constexpr (std::is_same_v<Type, core::SetCharacterFormat>) {
                return working.applyCharacterFormat(typed.range, typed.delta);
            } else if constexpr (
                std::is_same_v<Type,
                               core::SetParagraphMarkCharacterFormat>) {
                return working.applyParagraphMarkCharacterFormat(
                    typed.paragraph_id, typed.delta);
            } else if constexpr (std::is_same_v<Type, core::SetParagraphFormat>) {
                return working.applyParagraphFormat(typed.paragraph_ids, typed.delta);
            } else if constexpr (std::is_same_v<Type,
                                                core::SetParagraphStyle>) {
                return working.setParagraphStyle(
                    typed.paragraph_ids, typed.style_id);
            } else if constexpr (
                std::is_same_v<Type,
                               core::SetParagraphStyleProvenance>) {
                return working.setParagraphStyleProvenance(
                    typed.paragraph_id, typed.provenance);
            } else if constexpr (std::is_same_v<Type, core::SplitParagraph>) {
                return working.splitParagraph(typed.position, typed.new_paragraph_id);
            } else if constexpr (std::is_same_v<Type,
                                                core::MergeWithNextParagraph>) {
                return working.mergeWithNext(typed.paragraph_id);
            } else {
                return core::Error{
                    core::ErrorCode::invalid_operation,
                    "This semantic operation is not exposed by the current Codex tool schema"};
            }
        },
        operation);
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
    QString& error) {
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
    return renderProfessionalExcalidrawSkeleton(document.array(), error);
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

codex::Json createPreview(DocumentCanvas& canvas,
                          const QString& documentId,
                          const codex::Json& arguments,
                          QString& previewSummary,
                          QString& error) {
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
    std::vector<std::string> warnings;
    std::size_t figureOperationCount = 0;
    for (const auto& rawSource : *sourceOperations) {
        if (!rawSource.is_object()) {
            error = QObject::tr("Every preview operation must be an object.");
            return {};
        }
        codex::Json source = rawSource;
        const std::string kind = operationKind(source);
        source["kind"] = kind;
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
            if (!target.contains("blockId"))
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
        const bool needsTarget = kind != "replace_excalidraw_figure";
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
        } else if (kind == "insert_excalidraw_figure" ||
                   kind == "replace_excalidraw_figure") {
            if (++figureOperationCount > 8) {
                error = QObject::tr(
                    "A preview may create or revise at most eight Excalidraw figures.");
                return {};
            }
            QString accessibleName;
            std::optional<double> widthPoints;
            std::optional<double> heightPoints;
            const auto rendered = renderFigure(
                source, QString::fromStdString(labelValue->get<std::string>()),
                accessibleName, widthPoints, heightPoints, error);
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
            const double aspect = static_cast<double>(previewImage.height()) /
                                  static_cast<double>(previewImage.width());
            if (kind == "insert_excalidraw_figure") {
                const double width = widthPoints.value_or(432.0);
                const double height = heightPoints.value_or(
                    std::clamp(width * aspect, 36.0, 936.0));
                if (!appendWorkingOperation(
                        working, operations,
                        core::InsertImage{
                            {target.block, target.start},
                            imagePayload(rendered->png), core::ImageFormat::png,
                            toUtf8(accessibleName), pointsToEmu(width),
                            pointsToEmu(height)},
                        error)) {
                    return {};
                }
            } else {
                const auto encodedId = source.find("imageId");
                if (encodedId == source.end() || !encodedId->is_string()) {
                    error = QObject::tr("replace_excalidraw_figure requires an imageId.");
                    return {};
                }
                const auto imageId = core::NodeId::parse(
                    encodedId->get<std::string>());
                const core::ImageAtom* current =
                    imageId ? working.findImage(*imageId) : nullptr;
                if (!imageId || !current) {
                    error = QObject::tr("The editable Excalidraw image no longer exists.");
                    return {};
                }
                const auto currentBytes = current->encoded_payload.bytes();
                const QByteArray currentPng(
                    reinterpret_cast<const char*>(currentBytes.data()),
                    static_cast<qsizetype>(currentBytes.size()));
                if (current->format != core::ImageFormat::png ||
                    !excalidrawSceneFromPng(currentPng)) {
                    error = QObject::tr(
                        "replace_excalidraw_figure can only revise an editable Excalidraw figure.");
                    return {};
                }
                std::int64_t width = current->width_emu;
                std::int64_t height = current->height_emu;
                const std::string previousAccessibleName =
                    current->accessible_name;
                if (widthPoints && heightPoints) {
                    width = pointsToEmu(*widthPoints);
                    height = pointsToEmu(*heightPoints);
                } else if (widthPoints) {
                    width = pointsToEmu(*widthPoints);
                    height = pointsToEmu(
                        std::clamp(*widthPoints * aspect, 36.0, 936.0));
                } else if (heightPoints) {
                    height = pointsToEmu(*heightPoints);
                    width = pointsToEmu(
                        std::clamp(*heightPoints / aspect, 36.0, 936.0));
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
        } else if (kind == "insert_image") {
            error = QObject::tr("No user-granted image capability is attached to this request.");
            return {};
        } else {
            error = QObject::tr("Unsupported editor operation: %1")
                        .arg(QString::fromStdString(kind));
            return {};
        }
    }
    const QString label = QString::fromStdString(labelValue->get<std::string>()).left(512);
    if (!canvas.createOperationsPreview(core::Revision(*revision), operations,
                                        label, previewSummary, error)) {
        return {};
    }
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
                             QString& error) {
    previewSummary.clear();
    error.clear();
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
        return createPreview(canvas, documentId, arguments, previewSummary, error);
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
