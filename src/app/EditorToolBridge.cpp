#include "docxstudio/app/EditorToolBridge.h"

#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/math/latex_parser.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
    const auto id = found->find("blockId");
    if (id == found->end() || !id->is_string()) {
        error = QObject::tr("An editor target has no blockId.");
        return false;
    }
    const auto parsed = core::NodeId::parse(id->get<std::string>());
    const auto start = unsignedValue(*found, "start");
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

codex::Json formattingFor(const core::Paragraph& paragraph,
                          std::size_t returnedTextStart,
                          std::size_t returnedTextLength) {
    codex::Json runs = codex::Json::array();
    const std::size_t returnedTextEnd = returnedTextStart + returnedTextLength;
    for (const auto& run : paragraph.characterFormats()) {
        const std::size_t start = std::max(run.start, returnedTextStart);
        const std::size_t end = std::min(run.end, returnedTextEnd);
        if (start >= end) continue;
        codex::Json format = codex::Json::object();
        if (run.format.font_family) format["fontFamily"] = *run.format.font_family;
        if (run.format.font_size_half_points)
            format["fontSizePoints"] = *run.format.font_size_half_points / 2.0;
        if (run.format.bold) format["bold"] = *run.format.bold;
        if (run.format.italic) format["italic"] = *run.format.italic;
        if (run.format.underline)
            format["underline"] = *run.format.underline != core::UnderlineStyle::none;
        if (run.format.strike) format["strike"] = *run.format.strike;
        const auto colorName = [](std::uint32_t argb) {
            return toUtf8(QStringLiteral("#%1")
                              .arg(argb & 0x00ffffffU, 6, 16,
                                   QLatin1Char('0'))
                              .toUpper());
        };
        if (run.format.foreground_argb)
            format["foregroundColor"] = colorName(*run.format.foreground_argb);
        if (run.format.highlight_argb)
            format["highlightColor"] = colorName(*run.format.highlight_argb);
        if (run.format.baseline) {
            switch (*run.format.baseline) {
                case core::BaselinePosition::normal:
                    format["verticalAlign"] = "baseline";
                    break;
                case core::BaselinePosition::superscript:
                    format["verticalAlign"] = "superscript";
                    break;
                case core::BaselinePosition::subscript:
                    format["verticalAlign"] = "subscript";
                    break;
            }
        }
        runs.push_back({{"start", start - returnedTextStart},
                        {"end", end - returnedTextStart},
                        {"style", std::move(format)}});
    }
    return {{"alignment", alignmentName(paragraph.format())},
            {"characterRuns", std::move(runs)}};
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
    if (const auto expected = unsignedValue(arguments, "expectedRevision");
        expected && *expected != snapshot.revision.value()) {
        error = QObject::tr("REVISION_CONFLICT: expected %1, current %2")
                    .arg(*expected)
                    .arg(snapshot.revision.value());
        return {};
    }
    const std::string scope = arguments.value("scope", std::string("selection"));
    static const std::set<std::string> supportedScopes = {
        "selection", "document", "outline", "blocks"};
    if (!supportedScopes.contains(scope)) {
        error = QObject::tr("Unsupported editor read scope: %1. Supported scopes are selection, document, outline, and blocks.")
                    .arg(QString::fromStdString(scope));
        return {};
    }
    const bool includeFormatting = arguments.value("includeFormatting", true);
    std::size_t maximum = 50000;
    if (const auto requested = unsignedValue(arguments, "maxCharacters")) {
        maximum = static_cast<std::size_t>(std::min<std::uint64_t>(*requested, 200000));
    }

    std::set<core::NodeId> requestedIds;
    const auto ids = arguments.find("blockIds");
    if (ids != arguments.end() && ids->is_array()) {
        for (const auto& encoded : *ids) {
            if (!encoded.is_string()) continue;
            if (const auto parsed = core::NodeId::parse(encoded.get<std::string>())) {
                requestedIds.insert(*parsed);
            }
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
    for (std::size_t index = 0; index < snapshot.document.paragraphs().size(); ++index) {
        const auto& paragraph = snapshot.document.paragraphs()[index];
        if (scope == "blocks" && !requestedIds.contains(paragraph.id())) continue;
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
        block["attributes"]["equations"] = equationsFor(
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
                          std::vector<core::Operation>& operations, QString& error) {
    const auto style = operation.find("style");
    if (style == operation.end() || !style->is_object()) {
        error = QObject::tr("set_paragraph_style requires a style object.");
        return false;
    }
    core::ParagraphFormatDelta delta;
    if (const auto value = style->find("alignment"); value != style->end() && value->is_string()) {
        const auto name = value->get<std::string>();
        const auto alignment = name == "center" ? core::ParagraphAlignment::center
                               : name == "right" ? core::ParagraphAlignment::right
                               : name == "justify" ? core::ParagraphAlignment::justified
                                                  : core::ParagraphAlignment::left;
        delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(alignment);
    }
    if (const auto value = style->find("lineSpacing"); value != style->end() && value->is_number()) {
        const double multiple = value->get<double>();
        if (!(multiple > 0.0 && multiple <= 20.0)) {
            error = QObject::tr("lineSpacing is outside the supported range.");
            return false;
        }
        delta.line_spacing_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(multiple * 12.0 * 12700.0)));
        delta.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(
            core::LineSpacingRule::automatic);
    }
    if (const auto value = style->find("spaceBeforePoints"); value != style->end() && value->is_number())
        delta.space_before_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(value->get<double>() * 12700.0)));
    if (const auto value = style->find("spaceAfterPoints"); value != style->end() && value->is_number())
        delta.space_after_emu = core::PropertyDelta<std::int64_t>::set(
            static_cast<std::int64_t>(std::llround(value->get<double>() * 12700.0)));
    if (const auto value = style->find("keepWithNext"); value != style->end() && value->is_boolean())
        delta.keep_with_next = core::PropertyDelta<bool>::set(value->get<bool>());
    if (delta.empty()) {
        error = QObject::tr("set_paragraph_style contains no supported property.");
        return false;
    }
    operations.push_back(core::SetParagraphFormat{{target.block}, delta});
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
            } else if constexpr (std::is_same_v<Type, core::DeleteRange>) {
                return working.deleteRange(typed.range);
            } else if constexpr (std::is_same_v<Type, core::ReplaceRange>) {
                return working.replaceRange(typed.range, typed.text, typed.format);
            } else if constexpr (std::is_same_v<Type, core::SetCharacterFormat>) {
                return working.applyCharacterFormat(typed.range, typed.delta);
            } else if constexpr (std::is_same_v<Type, core::SetParagraphFormat>) {
                return working.applyParagraphFormat(typed.paragraph_ids, typed.delta);
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
    for (const auto& source : *sourceOperations) {
        if (!source.is_object()) {
            error = QObject::tr("Every preview operation must be an object.");
            return {};
        }
        const std::string kind = source.value("kind", std::string());
        const bool needsEnd = kind == "replace_text" || kind == "delete_range" ||
                              kind == "set_text_style";
        Target target;
        if (!parseTarget(source, target, error, needsEnd)) return {};
        affected.insert(target.block.toString());
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
            if (!appendWorkingOperation(working, operations,
                                        std::move(generated.front()), error)) {
                return {};
            }
        } else if (kind == "set_paragraph_style") {
            std::vector<core::Operation> generated;
            if (!appendParagraphStyle(source, target, generated, error)) return {};
            if (!appendWorkingOperation(working, operations,
                                        std::move(generated.front()), error)) {
                return {};
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
