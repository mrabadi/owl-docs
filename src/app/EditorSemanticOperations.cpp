#include "docxstudio/app/EditorSemanticOperations.h"
#include "docxstudio/codex/authoring_tools.hpp"
#include "docxstudio/app/RasterDecoder.h"

#include <QByteArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>

namespace docxstudio::app {
namespace {
using codex::Json;
QString string(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::u16string inlineText(const Json& value) {
    auto result = string(value.get<std::string>());
    result.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    result.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    result.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return result.toStdU16String();
}
bool validate(const Json& value, const Json& schema, const Json& definitions, QString& error, const QString& path) {
    const auto fail = [&] { error = QStringLiteral("Invalid semantic operation property: ") + path; return false; };
    if (schema.contains("$ref")) {
        const auto name = schema.at("$ref").get<std::string>().substr(8);
        return definitions.contains(name) && validate(value, definitions.at(name), definitions, error, path);
    }
    if (schema.contains("oneOf")) {
        for (const auto& alternative : schema.at("oneOf")) {
            QString ignored;
            if (validate(value, alternative, definitions, ignored, path)) return true;
        }
        return fail();
    }
    if (schema.contains("const") && value != schema.at("const")) return fail();
    if (schema.contains("enum") && std::find(schema.at("enum").begin(), schema.at("enum").end(), value) == schema.at("enum").end()) return fail();
    const auto type = schema.value("type", std::string{});
    if ((!type.empty()) && !((type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
        (type == "string" && value.is_string()) || (type == "boolean" && value.is_boolean()) ||
        (type == "number" && value.is_number()) || (type == "integer" && (value.is_number_integer() || value.is_number_unsigned())) ||
        (type == "null" && value.is_null()))) return fail();
    if (value.is_number()) {
        const double number = value.get<double>();
        if (!std::isfinite(number) || (schema.contains("minimum") && number < schema.at("minimum").get<double>()) ||
            (schema.contains("maximum") && number > schema.at("maximum").get<double>()) ||
            (schema.contains("exclusiveMinimum") && number <= schema.at("exclusiveMinimum").get<double>())) return fail();
    }
    if (value.is_string()) {
        const auto encoded = value.get<std::string>();
        const auto text = string(encoded);
        if (text.toUtf8().toStdString() != encoded) return fail();
        const auto length = text.toUcs4().size();
        if ((schema.contains("minLength") && length < schema.at("minLength").get<qsizetype>()) ||
            (schema.contains("maxLength") && length > schema.at("maxLength").get<qsizetype>()) ||
            (schema.contains("pattern") && !QRegularExpression(string(schema.at("pattern").get<std::string>())).match(text).hasMatch())) return fail();
    }
    if (value.is_array()) {
        if ((schema.contains("minItems") && value.size() < schema.at("minItems").get<std::size_t>()) ||
            (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<std::size_t>())) return fail();
        if (schema.contains("items")) for (std::size_t index = 0; index < value.size(); ++index)
            if (!validate(value.at(index), schema.at("items"), definitions, error, path + QStringLiteral("[%1]").arg(index))) return false;
    }
    if (value.is_object()) {
        if (schema.contains("minProperties") && value.size() < schema.at("minProperties").get<std::size_t>()) return fail();
        if (schema.contains("required")) for (const auto& name : schema.at("required"))
            if (!value.contains(name.get<std::string>())) return fail();
        if (schema.contains("properties")) {
            const auto& properties = schema.at("properties");
            for (auto item = value.begin(); item != value.end(); ++item) {
                if (!properties.contains(item.key())) {
                    if (!schema.value("additionalProperties", true)) {
                        error = path + QLatin1Char('.') + string(item.key()).left(256) + QStringLiteral(" is an unsupported property.");
                        return false;
                    }
                } else if (!validate(item.value(), properties.at(item.key()), definitions, error, path + QLatin1Char('.') + string(item.key()))) return false;
            }
        }
    }
    return true;
}
std::optional<core::NodeId> id(const Json& source, const char* name, QString& error) {
    if (source.contains(name) && source.at(name).is_string()) {
        if (auto parsed = core::NodeId::parse(source.at(name).get<std::string>())) return parsed;
    }
    error = QStringLiteral("Invalid stable identity: ") + QString::fromLatin1(name);
    return std::nullopt;
}
struct Cell { core::NodeId table; std::size_t row; std::size_t column; };
std::optional<Cell> cellTarget(const core::Document& document, const Json& target, QString& error) {
    const auto tableId = id(target, "tableId", error);
    const auto cellId = id(target, "cellId", error);
    const auto* table = tableId ? document.findTable(*tableId) : nullptr;
    if (table && cellId) {
        for (std::size_t row = 0; row < table->rowCount(); ++row)
            for (std::size_t column = 0; column < table->columnCount(); ++column)
                if (table->cell(row, column)->id == *cellId) return Cell{*tableId, row, column};
    }
    error = QStringLiteral("The stable table-cell target is unavailable.");
    return std::nullopt;
}
bool append(core::Document& document, std::vector<core::Operation>& operations, core::Operation operation, QString& error) {
    const auto applied = core::applyDocumentOperation(document, operation);
    if (!applied) { error = QString::fromStdString(applied.error().message); return false; }
    operations.push_back(std::move(operation));
    return true;
}
std::uint32_t color(const Json& value) {
    return 0xff000000U | string(value.get<std::string>()).mid(1).toUInt(nullptr, 16);
}
std::int64_t emu(const Json& value) { return static_cast<std::int64_t>(std::llround(value.get<double>() * 12700.0)); }
std::uint32_t twips(const Json& value) { return static_cast<std::uint32_t>(std::llround(value.get<double>() * 20.0)); }
core::CharacterFormatDelta textStyle(const Json& style) {
    core::CharacterFormatDelta delta;
    if (style.contains("fontFamily")) delta.font_family = core::PropertyDelta<std::string>::set(style.at("fontFamily").get<std::string>());
    if (style.contains("fontSizePoints")) delta.font_size_half_points = core::PropertyDelta<std::int32_t>::set(static_cast<std::int32_t>(std::llround(style.at("fontSizePoints").get<double>() * 2.0)));
    if (style.contains("bold")) delta.bold = core::PropertyDelta<bool>::set(style.at("bold").get<bool>());
    if (style.contains("italic")) delta.italic = core::PropertyDelta<bool>::set(style.at("italic").get<bool>());
    if (style.contains("strike")) delta.strike = core::PropertyDelta<bool>::set(style.at("strike").get<bool>());
    if (style.contains("underline")) delta.underline = core::PropertyDelta<core::UnderlineStyle>::set(style.at("underline").get<bool>() ? core::UnderlineStyle::single : core::UnderlineStyle::none);
    if (style.contains("foregroundColor")) delta.foreground_argb = core::PropertyDelta<std::uint32_t>::set(color(style.at("foregroundColor")));
    if (style.contains("highlightColor")) delta.highlight_argb = style.at("highlightColor").is_null()
        ? core::PropertyDelta<std::uint32_t>::clear() : core::PropertyDelta<std::uint32_t>::set(color(style.at("highlightColor")));
    if (style.contains("verticalAlign")) {
        const auto value = style.at("verticalAlign").get<std::string>();
        delta.baseline = core::PropertyDelta<core::BaselinePosition>::set(value == "superscript" ? core::BaselinePosition::superscript : value == "subscript" ? core::BaselinePosition::subscript : core::BaselinePosition::normal);
    }
    return delta;
}
core::ParagraphAlignment alignment(const std::string& value) {
    return value == "center" ? core::ParagraphAlignment::center : value == "right" ? core::ParagraphAlignment::right : value == "justify" ? core::ParagraphAlignment::justified : core::ParagraphAlignment::left;
}
const std::map<std::string, core::TableStyle> tableStyles{
    {"plain", core::TableStyle::plain}, {"grid", core::TableStyle::grid}, {"light-gray", core::TableStyle::light_gray},
    {"light-blue", core::TableStyle::light_blue}, {"light-orange", core::TableStyle::light_orange}, {"medium-blue", core::TableStyle::medium_blue},
    {"medium-green", core::TableStyle::medium_green}, {"medium-orange", core::TableStyle::medium_orange}, {"aubergine", core::TableStyle::aubergine},
    {"orange-accent", core::TableStyle::orange_accent}, {"banded-blue", core::TableStyle::banded_blue}, {"banded-aubergine", core::TableStyle::banded_aubergine}, {"dark-header", core::TableStyle::dark_header}
};
bool borders(core::Document& document, const Json& source, const core::Table& table,
             std::vector<core::Operation>& operations, QString& error) {
    using Coordinate = std::pair<std::size_t, std::size_t>;
    std::set<Coordinate> selected;
    std::set<std::string> ids;
    if (source.contains("cellIds")) for (const auto& value : source.at("cellIds")) ids.insert(value.get<std::string>());
    for (std::size_t row = 0; row < table.rowCount(); ++row)
        for (std::size_t column = 0; column < table.columnCount(); ++column)
            if (!source.contains("cellIds") || ids.contains(table.cell(row, column)->id.toString())) selected.emplace(row, column);
    if (selected.empty() || (source.contains("cellIds") && selected.size() != ids.size())) {
        error = QStringLiteral("A border selection requires available stable cell identities."); return false;
    }
    std::size_t firstRow = table.rowCount(), lastRow = 0, firstColumn = table.columnCount(), lastColumn = 0;
    for (const auto& [row, column] : selected) {
        firstRow = std::min(firstRow, row); lastRow = std::max(lastRow, row);
        firstColumn = std::min(firstColumn, column); lastColumn = std::max(lastColumn, column);
    }
    if (selected.size() != (lastRow - firstRow + 1) * (lastColumn - firstColumn + 1)) {
        error = QStringLiteral("Border presets require a rectangular cell selection."); return false;
    }
    const auto preset = source.at("preset").get<std::string>();
    const core::TableBorder border{preset == "none" ? core::TableBorderStyle::none : core::TableBorderStyle::single,
        preset == "none" ? std::uint16_t{0} : static_cast<std::uint16_t>(std::llround(source.value("widthPoints", 0.5) * 8.0)),
        source.contains("color") ? color(source.at("color")) : 0xff000000U};
    std::map<Coordinate, core::TableCellLayout> layouts;
    const auto get = [&](Coordinate coordinate) -> core::TableCellLayout& {
        return layouts.try_emplace(coordinate, table.cell(coordinate.first, coordinate.second)->layout).first->second;
    };
    const auto set = [](core::TableCellLayout& layout, int edge, const core::TableBorder& value) {
        if (edge == 0) layout.border_top = value;
        else if (edge == 1) layout.border_right = value;
        else if (edge == 2) layout.border_bottom = value;
        else layout.border_left = value;
    };
    for (const auto& coordinate : selected) {
        const auto [row, column] = coordinate;
        for (int edge = 0; edge < 4; ++edge) {
            const bool outside = edge == 0 ? row == firstRow : edge == 1 ? column == lastColumn : edge == 2 ? row == lastRow : column == firstColumn;
            const bool apply = preset == "all" || preset == "none" || (preset == "outside" && outside) || (preset == "inside" && !outside) ||
                (preset == "top" && edge == 0 && outside) || (preset == "right" && edge == 1 && outside) ||
                (preset == "bottom" && edge == 2 && outside) || (preset == "left" && edge == 3 && outside);
            if (!apply) continue;
            set(get(coordinate), edge, border);
            if (edge == 0 && row > 0) set(get({row - 1, column}), 2, border);
            if (edge == 1 && column + 1 < table.columnCount()) set(get({row, column + 1}), 3, border);
            if (edge == 2 && row + 1 < table.rowCount()) set(get({row + 1, column}), 0, border);
            if (edge == 3 && column > 0) set(get({row, column - 1}), 1, border);
        }
    }
    const auto tableId = table.id();
    for (auto& [coordinate, layout] : layouts)
        if (!append(document, operations, core::SetTableCellLayout{tableId, coordinate.first, coordinate.second, std::move(layout)}, error)) return false;
    return true;
}
std::optional<std::string> imageContainer(const core::Document& document, core::NodeId imageId) {
    for (const auto& paragraph : document.paragraphs()) for (const auto& image : paragraph.images())
        if (image.id == imageId) return paragraph.id().toString();
    for (const auto& table : document.tables()) for (const auto& cell : table.cells()) for (const auto& image : cell.images)
        if (image.id == imageId) return table.id().toString();
    for (const auto& image : document.headerImages()) if (image.id == imageId) return "header";
    for (const auto& image : document.footerImages()) if (image.id == imageId) return "footer";
    return std::nullopt;
}
bool deleteImage(core::Document& document, core::NodeId imageId, std::vector<core::Operation>& operations, QString& error) {
    for (const auto& paragraph : document.paragraphs()) for (const auto& image : paragraph.images())
        if (image.id == imageId) return append(document, operations, core::DeleteRange{{{paragraph.id(), image.utf16_offset}, {paragraph.id(), image.utf16_offset + 1}}}, error);
    for (const auto& table : document.tables()) for (std::size_t row = 0; row < table.rowCount(); ++row)
        for (std::size_t column = 0; column < table.columnCount(); ++column) for (const auto& image : table.cell(row, column)->images)
            if (image.id == imageId) return append(document, operations, core::ReplaceTableCellRange{table.id(), row, column, image.utf16_offset, image.utf16_offset + 1, {}, std::nullopt}, error);
    for (const bool footer : {false, true}) for (const auto& image : footer ? document.footerImages() : document.headerImages()) {
        if (image.id != imageId) continue;
        auto text = footer ? document.footerText() : document.headerText();
        const auto offset = image.utf16_offset;
        text.erase(offset, 1);
        return footer ? append(document, operations, core::SetFooterText{std::move(text), core::StoryTextEditRange{offset, 1}}, error)
            : append(document, operations, core::SetHeaderText{std::move(text), core::StoryTextEditRange{offset, 1}}, error);
    }
    error = QStringLiteral("The picture target is unavailable."); return false;
}
bool plan(core::Document& document, const Json& source, std::vector<core::Operation>& operations,
          std::set<std::string>& affected, QString& error) {
    const auto kind = source.at("kind").get<std::string>();
    if (kind == "insert_table") {
        const auto& target = source.at("target");
        const auto block = id(target, "blockId", error);
        if (!block) return false;
        const auto* paragraph = document.findParagraph(*block);
        const auto start = target.at("start").get<std::size_t>();
        if (!paragraph || start > paragraph->text().size() || !core::isUtf16Boundary(paragraph->text(), start)) {
            error = QStringLiteral("A table must be inserted at a valid body paragraph boundary."); return false;
        }
        const auto rows = source.at("rows").get<std::size_t>();
        const auto columns = source.at("columns").get<std::size_t>();
        const auto tableId = source.contains("tableId") ? id(source, "tableId", error) : std::optional{core::NodeId::generate()};
        if (!tableId) return false;
        auto table = core::Table::create(rows, columns, source.value("headerRow", false), *tableId);
        if (source.contains("cellIds")) {
            const auto& identities = source.at("cellIds");
            if (identities.size() != rows) { error = QStringLiteral("Table cell identities must match the row count."); return false; }
            std::vector<core::TableCell> cells;
            for (std::size_t row = 0; row < rows; ++row) {
                if (identities.at(row).size() != columns) { error = QStringLiteral("Table cell identities must match the column count."); return false; }
                for (std::size_t column = 0; column < columns; ++column) {
                    const auto identity = core::NodeId::parse(identities.at(row).at(column).get<std::string>());
                    if (!identity) { error = QStringLiteral("A requested cell identity is invalid."); return false; }
                    cells.emplace_back(*identity, std::u16string{});
                }
            }
            table = core::Table::restore(rows, columns, source.value("headerRow", false), *tableId, std::move(cells));
        }
        if (!table) { error = QString::fromStdString(table.error().message); return false; }
        auto before = *block;
        if (start != 0) {
            before = core::NodeId::generate();
            if (!append(document, operations, core::SplitParagraph{{*block, start}, before}, error)) return false;
        }
        if (!append(document, operations, core::InsertTable{before, std::move(table.value())}, error)) return false;
        if (source.contains("cells")) {
            const auto& cells = source.at("cells");
            if (cells.size() != rows) { error = QStringLiteral("Table cell data must match the row count."); return false; }
            for (std::size_t row = 0; row < rows; ++row) {
                if (cells.at(row).size() != columns) { error = QStringLiteral("Table cell data must match the column count."); return false; }
                for (std::size_t column = 0; column < columns; ++column)
                    if (!append(document, operations, core::SetTableCellText{*tableId, row, column, inlineText(cells.at(row).at(column))}, error)) return false;
            }
        }
        affected.insert(tableId->toString());
        affected.insert(block->toString());
        return true;
    }
    if (kind == "replace_table_cell_range" || kind == "set_table_cell_text" || kind == "set_table_cell_style" || kind == "set_table_cell_alignment" || kind == "set_table_cell_paragraph_format") {
        const auto& target = source.at("target");
        const auto cell = cellTarget(document, target, error);
        if (!cell) return false;
        if (kind == "replace_table_cell_range") {
            if (!append(document, operations, core::ReplaceTableCellRange{cell->table, cell->row, cell->column,
                target.at("start").get<std::size_t>(), target.at("end").get<std::size_t>(), inlineText(source.at("text")), std::nullopt}, error)) return false;
        } else if (kind == "set_table_cell_text") {
            if (!append(document, operations, core::SetTableCellText{cell->table, cell->row, cell->column, inlineText(source.at("text"))}, error)) return false;
        } else if (kind == "set_table_cell_style") {
            core::CharacterFormatDelta delta;
            if (!parseEditorCharacterFormat(source.at("style"), delta, error)) return false;
            if (!append(document, operations, core::SetTableCellCharacterFormat{cell->table, cell->row, cell->column,
                target.at("start").get<std::size_t>(), target.at("end").get<std::size_t>(), delta}, error)) return false;
        } else if (kind == "set_table_cell_paragraph_format") {
            core::ParagraphFormatDelta delta;
            if (!parseEditorParagraphFormat(source.at("style"), delta, error)) return false;
            if (!append(document, operations, core::SetTableCellParagraphFormat{cell->table, cell->row, cell->column, delta}, error)) return false;
        } else {
            auto layout = document.findTable(cell->table)->cell(cell->row, cell->column)->layout;
            const auto vertical = source.at("vertical").get<std::string>();
            layout.vertical_alignment = vertical == "top" ? core::TableCellVerticalAlignment::top : vertical == "center" ? core::TableCellVerticalAlignment::center : core::TableCellVerticalAlignment::bottom;
            core::ParagraphFormatDelta delta;
            delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(alignment(source.at("horizontal").get<std::string>()));
            if (!append(document, operations, core::SetTableCellParagraphFormat{cell->table, cell->row, cell->column, delta}, error) ||
                !append(document, operations, core::SetTableCellLayout{cell->table, cell->row, cell->column, std::move(layout)}, error)) return false;
        }
        affected.insert(cell->table.toString());
        return true;
    }
    if (source.contains("tableId")) {
        const auto tableId = id(source, "tableId", error);
        const auto* found = tableId ? document.findTable(*tableId) : nullptr;
        if (!found) { error = QStringLiteral("The table target is unavailable."); return false; }
        const auto table = *found;
        bool result = false;
        if (kind == "set_table_style") result = append(document, operations, core::SetTableStyle{*tableId, tableStyles.at(source.at("style").get<std::string>())}, error);
        else if (kind == "delete_table") result = append(document, operations, core::DeleteTable{*tableId}, error);
        else if (kind == "set_table_geometry") {
            auto geometry = table.geometry();
            if (!source.contains("columnWidthsPoints") && !source.contains("rowMinimumHeightsPoints")) { error = QStringLiteral("Specify column widths or row minimum heights."); return false; }
            if (source.contains("columnWidthsPoints")) {
                geometry.column_widths_twips.clear();
                for (const auto& value : source.at("columnWidthsPoints")) geometry.column_widths_twips.push_back(twips(value));
            }
            if (source.contains("rowMinimumHeightsPoints")) {
                geometry.row_minimum_heights_twips.clear();
                for (const auto& value : source.at("rowMinimumHeightsPoints")) geometry.row_minimum_heights_twips.push_back(twips(value));
            }
            result = append(document, operations, core::SetTableGeometry{*tableId, std::move(geometry)}, error);
        } else if (kind == "insert_table_row" || kind == "insert_table_column") {
            const bool row = kind == "insert_table_row";
            std::vector<core::NodeId> identities(row ? table.columnCount() : table.rowCount());
            std::generate(identities.begin(), identities.end(), [] { return core::NodeId::generate(); });
            const auto inheritance = source.value("inheritFrom", std::string("preceding")) == "following" ? core::TableInsertionSource::following : core::TableInsertionSource::preceding;
            const auto index = source.at("index").get<std::size_t>();
            result = row ? append(document, operations, core::InsertTableRow{*tableId, index, std::move(identities), inheritance}, error)
                : append(document, operations, core::InsertTableColumn{*tableId, index, std::move(identities), inheritance}, error);
        } else if (kind == "delete_table_rows" || kind == "delete_table_columns") {
            const auto index = source.at("index").get<std::size_t>(), count = source.at("count").get<std::size_t>();
            result = kind == "delete_table_rows" ? append(document, operations, core::DeleteTableRows{*tableId, index, count}, error)
                : append(document, operations, core::DeleteTableColumns{*tableId, index, count}, error);
        } else if (kind == "distribute_table_columns" || kind == "distribute_table_rows") {
            auto geometry = table.geometry();
            if (kind == "distribute_table_columns") {
                const auto total = source.contains("totalWidthPoints") ? twips(source.at("totalWidthPoints")) :
                    std::accumulate(geometry.column_widths_twips.begin(), geometry.column_widths_twips.end(), std::uint32_t{0});
                if (total < table.columnCount()) { error = QStringLiteral("An automatic grid requires a valid totalWidthPoints."); return false; }
                geometry.column_widths_twips.assign(table.columnCount(), total / static_cast<std::uint32_t>(table.columnCount()));
                for (std::size_t index = 0; index < total % table.columnCount(); ++index) ++geometry.column_widths_twips[index];
            } else {
                const auto total = std::accumulate(geometry.row_minimum_heights_twips.begin(), geometry.row_minimum_heights_twips.end(), std::uint32_t{0});
                const auto common = source.contains("minimumHeightPoints") ? twips(source.at("minimumHeightPoints")) : total / static_cast<std::uint32_t>(table.rowCount());
                geometry.row_minimum_heights_twips.assign(table.rowCount(), common);
            }
            result = append(document, operations, core::SetTableGeometry{*tableId, std::move(geometry)}, error);
        } else if (kind == "set_table_borders") result = borders(document, source, table, operations, error);
        if (result) affected.insert(tableId->toString());
        return result;
    }
    if (kind == "insert_raster_image") {
        const auto encoded = QByteArray::fromStdString(source.at("base64Data").get<std::string>());
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || static_cast<std::size_t>(decoded.decoded.size()) > 4U * 1024U * 1024U) {
            error = QStringLiteral("The picture requires valid bounded base64 image bytes."); return false;
        }
        std::vector<std::uint8_t> bytes(decoded.decoded.begin(), decoded.decoded.end());
        const auto raster = decodeRasterImage(bytes);
        const bool png = source.at("mimeType") == "image/png";
        if (!raster.ok() || raster.format != (png ? RasterImageFormat::png : RasterImageFormat::jpeg)) {
            error = QStringLiteral("The picture is not a valid bounded PNG/JPEG of the specified MIME type."); return false;
        }
        const auto imageId = source.contains("imageId") ? id(source, "imageId", error) : std::optional{core::NodeId::generate()};
        if (!imageId) return false;
        const auto format = png ? core::ImageFormat::png : core::ImageFormat::jpeg;
        const auto name = source.at("accessibleName").get<std::string>();
        const auto width = emu(source.at("widthPoints")), height = emu(source.at("heightPoints"));
        core::EncodedImagePayload payload(std::move(bytes));
        const auto& target = source.at("target");
        const auto start = target.at("start").get<std::size_t>();
        bool result = false;
        if (target.contains("story")) {
            const bool footer = target.at("story") == "footer";
            result = append(document, operations, core::InsertHeaderFooterImage{footer, start, payload, format, name, width, height, *imageId}, error);
            if (result) affected.insert(footer ? "footer" : "header");
        } else if (target.contains("cellId")) {
            const auto cell = cellTarget(document, target, error);
            if (!cell) return false;
            result = append(document, operations, core::InsertTableCellImage{cell->table, cell->row, cell->column, start, payload, format, name, width, height, *imageId}, error);
            if (result) affected.insert(cell->table.toString());
        } else {
            const auto block = id(target, "blockId", error);
            if (!block) return false;
            result = append(document, operations, core::InsertImage{{*block, start}, payload, format, name, width, height, *imageId}, error);
            if (result) affected.insert(block->toString());
        }
        return result;
    }
    if (source.contains("imageId")) {
        const auto imageId = id(source, "imageId", error);
        const auto* image = imageId ? document.findImage(*imageId) : nullptr;
        const auto container = imageId ? imageContainer(document, *imageId) : std::nullopt;
        if (!image || !container) { error = QStringLiteral("The picture target is unavailable."); return false; }
        bool result = false;
        if (kind == "resize_image") result = append(document, operations, core::ResizeImage{*imageId, emu(source.at("widthPoints")), emu(source.at("heightPoints"))}, error);
        else if (kind == "set_image_alt_text") result = append(document, operations, core::SetImageAccessibleName{*imageId, source.at("text").get<std::string>()}, error);
        else if (kind == "delete_image") result = deleteImage(document, *imageId, operations, error);
        else if (kind == "set_image_layout") {
            auto layout = image->layout;
            const auto placement = source.at("placement").get<std::string>();
            layout.placement = placement == "square" ? core::ImagePlacement::square : placement == "topBottom" ? core::ImagePlacement::top_and_bottom : core::ImagePlacement::inline_with_text;
            if (source.contains("moveWithText")) layout.move_with_text = source.at("moveWithText").get<bool>();
            if (layout.placement == core::ImagePlacement::inline_with_text) layout.move_with_text = true;
            if (source.contains("distanceTopPoints")) layout.distance_top_emu = emu(source.at("distanceTopPoints"));
            if (source.contains("distanceRightPoints")) layout.distance_right_emu = emu(source.at("distanceRightPoints"));
            if (source.contains("distanceBottomPoints")) layout.distance_bottom_emu = emu(source.at("distanceBottomPoints"));
            if (source.contains("distanceLeftPoints")) layout.distance_left_emu = emu(source.at("distanceLeftPoints"));
            result = append(document, operations, core::SetImageLayout{*imageId, layout}, error);
        }
        if (result) affected.insert(*container);
        return result;
    }
    if (kind == "set_story_text") {
        const bool footer = source.at("story") == "footer";
        auto story = string(source.at("text").get<std::string>());
        story.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        story.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        auto updated = story.toStdU16String();
        std::optional<core::StoryTextEditRange> replacement;
        if (source.contains("region")) {
            if (updated.find(u'\t') != std::u16string::npos) { error = QStringLiteral("A story region cannot contain region-separator tabs."); return false; }
            auto parts = QString::fromStdU16String(footer ? document.footerText() : document.headerText()).split(QLatin1Char('\t'), Qt::KeepEmptyParts);
            while (parts.size() < 3) parts.push_back(QString{});
            if (parts.size() > 3) { error = QStringLiteral("This story has unsupported additional regions."); return false; }
            const bool canonical = parts.join(QLatin1Char('\t')).toStdU16String() == (footer ? document.footerText() : document.headerText());
            const auto region = source.at("region").get<std::string>();
            const qsizetype index = region == "left" ? 0 : region == "center" ? 1 : 2;
            std::size_t start = 0;
            for (qsizetype previous = 0; previous < index; ++previous) start += static_cast<std::size_t>(parts.at(previous).size()) + 1;
            if (canonical) replacement = core::StoryTextEditRange{start, static_cast<std::size_t>(parts.at(index).size())};
            parts[index] = QString::fromStdU16String(updated);
            updated = parts.join(QLatin1Char('\t')).toStdU16String();
        } else if (std::count(updated.begin(), updated.end(), u'\t') > 2) {
            error = QStringLiteral("A default story has at most three regions."); return false;
        }
        const bool result = footer ? append(document, operations, core::SetFooterText{std::move(updated), replacement}, error)
            : append(document, operations, core::SetHeaderText{std::move(updated), replacement}, error);
        if (result) affected.insert(footer ? "footer" : "header");
        return result;
    }
    if (kind == "split_paragraph") {
        const auto block = id(source.at("target"), "blockId", error);
        const auto newId = source.contains("newParagraphId") ? id(source, "newParagraphId", error) : std::optional{core::NodeId::generate()};
        if (!block || !newId) return false;
        if (!append(document, operations, core::SplitParagraph{{*block, source.at("target").at("start").get<std::size_t>()}, *newId}, error)) return false;
        affected.insert(block->toString()); affected.insert(newId->toString());
        return true;
    }
    if (kind == "merge_paragraph" || kind == "move_block") {
        const auto block = id(source, "blockId", error);
        if (!block) return false;
        std::optional<core::NodeId> before;
        if (source.contains("beforeBlockId") && !source.at("beforeBlockId").is_null()) {
            before = id(source, "beforeBlockId", error);
            if (!before) return false;
        }
        const bool result = kind == "merge_paragraph" ? append(document, operations, core::MergeWithNextParagraph{*block}, error)
            : append(document, operations, core::MoveBodyBlock{*block, before}, error);
        if (result) affected.insert(block->toString());
        return result;
    }
    if (kind == "set_academic_field") {
        const auto fieldId = id(source, "fieldId", error);
        const auto* current = fieldId ? document.findField(*fieldId) : nullptr;
        if (!current) { error = QStringLiteral("The academic field target is unavailable."); return false; }
        if (current->kind != core::FieldKind::citation && current->kind != core::FieldKind::cross_reference && current->kind != core::FieldKind::note_reference) {
            error = QStringLiteral("Generated numbering fields are computed from document state."); return false;
        }
        auto field = *current;
        if (!source.contains("targetId") && !source.contains("locator") && !source.contains("format")) {
            error = QStringLiteral("Specify a field target, citation locator or reference format."); return false;
        }
        if (source.contains("targetId")) {
            const auto target = id(source, "targetId", error);
            if (!target) return false;
            const auto* paragraph = document.findParagraph(*target);
            const auto* definition = paragraph && paragraph->styleId() ? core::findBuiltInParagraphStyle(*paragraph->styleId()) : nullptr;
            const bool valid = field.kind == core::FieldKind::citation ? paragraph && paragraph->isBibliographyEntry() :
                field.kind == core::FieldKind::note_reference ? document.findNote(*target) != nullptr :
                paragraph && (paragraph->caption() || (definition && definition->outline_level));
            if (!valid) { error = QStringLiteral("The replacement field target is not a supported source, note, heading or caption."); return false; }
            field.target_id = *target;
        }
        if (source.contains("locator")) {
            if (field.kind != core::FieldKind::citation) { error = QStringLiteral("Only citations have locators."); return false; }
            field.locator = inlineText(source.at("locator"));
        }
        if (source.contains("format")) {
            if (field.kind != core::FieldKind::cross_reference) { error = QStringLiteral("Only cross-references have reference formats."); return false; }
            field.reference_format = source.at("format") == "full_text" ? core::CrossReferenceFormat::full_text : core::CrossReferenceFormat::label_number;
        }
        std::string paragraphId;
        for (const auto& paragraph : document.paragraphs()) for (const auto& candidate : paragraph.fields())
            if (candidate.id == *fieldId) paragraphId = paragraph.id().toString();
        if (!append(document, operations, core::ReplaceField{*fieldId, std::move(field)}, error)) return false;
        affected.insert(paragraphId);
        return true;
    }
    if (kind == "set_caption_metadata") {
        const auto block = id(source, "blockId", error);
        const auto* paragraph = block ? document.findParagraph(*block) : nullptr;
        if (!paragraph || !paragraph->caption()) { error = QStringLiteral("The target is not an existing caption paragraph."); return false; }
        if (!source.contains("label") && !source.contains("objectId")) { error = QStringLiteral("Specify a caption label or object identity."); return false; }
        auto metadata = *paragraph->caption();
        if (source.contains("label")) {
            const auto label = source.at("label").get<std::string>();
            metadata.label = label == "Table" ? core::CaptionLabel::table : label == "Equation" ? core::CaptionLabel::equation : core::CaptionLabel::figure;
        }
        if (source.contains("objectId")) {
            const auto objectId = id(source, "objectId", error);
            if (!objectId) return false;
            if (!document.findTable(*objectId) && !document.findImage(*objectId) && !document.findEquation(*objectId)) { error = QStringLiteral("The caption object is unavailable."); return false; }
            metadata.target_id = *objectId;
        }
        if (!append(document, operations, core::SetCaption{*block, metadata}, error)) return false;
        affected.insert(block->toString()); return true;
    }
    if (kind == "set_bibliography_entry") {
        const auto block = id(source, "blockId", error);
        const auto* found = block ? document.findParagraph(*block) : nullptr;
        if (!found) { error = QStringLiteral("The bibliography paragraph is unavailable."); return false; }
        const auto paragraph = *found;
        const bool enabled = source.at("enabled").get<bool>();
        if (!append(document, operations, core::SetBibliographyEntry{*block, enabled}, error)) return false;
        std::vector<std::size_t> numbering;
        for (const auto& field : paragraph.fields()) if (field.kind == core::FieldKind::bibliography_number) numbering.push_back(field.utf16_offset);
        if (enabled && numbering.empty()) {
            const auto format = paragraph.characterFormatAt(0);
            if (!append(document, operations, core::InsertField{{*block, 0}, {core::NodeId::generate(), 0, core::FieldKind::bibliography_number, *block}, format}, error)) return false;
            if (!append(document, operations, core::InsertText{{*block, 1}, u" ", format}, error)) return false;
        } else if (!enabled) {
            std::sort(numbering.rbegin(), numbering.rend());
            for (const auto offset : numbering) {
                const auto length = offset + 1 < paragraph.text().size() && paragraph.text()[offset + 1] == u' ' ? 2U : 1U;
                if (!append(document, operations, core::DeleteRange{{{*block, offset}, {*block, offset + length}}}, error)) return false;
            }
        }
        affected.insert(block->toString()); return true;
    }
    error = QStringLiteral("Unsupported semantic operation.");
    return false;
}
}
bool parseEditorCharacterFormat(const codex::Json& input, core::CharacterFormatDelta& delta, QString& error) {
    error.clear();
    const auto definitions = codex::authoringSchemaDefinitions();
    if (!validate(input, definitions.at("semanticTextStyle"), definitions, error, QStringLiteral("style"))) return false;
    auto parsed = textStyle(input);
    const auto validation = parsed.validate();
    if (!validation) { error = QString::fromStdString(validation.error().message); return false; }
    delta = std::move(parsed);
    return true;
}
bool parseEditorParagraphFormat(const codex::Json& input, core::ParagraphFormatDelta& delta, QString& error) {
    error.clear();
    if (!input.is_object()) { error = QStringLiteral("Paragraph formatting must be an object."); return false; }
    auto style = input;
    style.erase("styleId");
    if (style.empty()) { delta = {}; return true; }
    const auto definitions = codex::authoringSchemaDefinitions();
    if (!validate(style, definitions.at("semanticParagraphFormat"), definitions, error, QStringLiteral("style"))) return false;
    core::ParagraphFormatDelta parsed;
    if (style.contains("alignment")) parsed.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(alignment(style.at("alignment").get<std::string>()));
    const auto property = [&](const char* name, core::PropertyDelta<std::int64_t>& destination) {
        if (style.contains(name)) destination = core::PropertyDelta<std::int64_t>::set(emu(style.at(name)));
    };
    property("leftIndentPoints", parsed.left_indent_emu); property("rightIndentPoints", parsed.right_indent_emu); property("firstLineIndentPoints", parsed.first_line_indent_emu);
    property("spaceBeforePoints", parsed.space_before_emu); property("spaceAfterPoints", parsed.space_after_emu);
    if (style.contains("lineSpacing") && style.contains("lineSpacingPoints")) { error = QStringLiteral("Use a line-spacing multiplier or a point value, not both."); return false; }
    if (style.contains("lineSpacing")) {
        parsed.line_spacing_emu = core::PropertyDelta<std::int64_t>::set(static_cast<std::int64_t>(std::llround(style.at("lineSpacing").get<double>() * 12.0 * 12700.0)));
        parsed.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(core::LineSpacingRule::automatic);
    }
    property("lineSpacingPoints", parsed.line_spacing_emu);
    if (style.contains("lineSpacingRule")) {
        const auto rule = style.at("lineSpacingRule").get<std::string>();
        parsed.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(rule == "exact" ? core::LineSpacingRule::exact : rule == "atLeast" ? core::LineSpacingRule::at_least : core::LineSpacingRule::automatic);
    }
    if (style.contains("keepWithNext")) parsed.keep_with_next = core::PropertyDelta<bool>::set(style.at("keepWithNext").get<bool>());
    if (style.contains("keepLines")) parsed.keep_lines = core::PropertyDelta<bool>::set(style.at("keepLines").get<bool>());
    if (style.contains("pageBreakBefore")) parsed.page_break_before = core::PropertyDelta<bool>::set(style.at("pageBreakBefore").get<bool>());
    const auto validation = parsed.validate();
    if (!validation) { error = QString::fromStdString(validation.error().message); return false; }
    delta = std::move(parsed);
    return true;
}
bool handlesSemanticOperation(std::string_view kind) {
    for (const auto& schema : codex::authoringOperationSchemas())
        if (schema.at("properties").at("kind").at("const").get<std::string>() == kind) return true;
    return false;
}
bool appendSemanticOperation(core::Document& working, const codex::Json& source,
    std::vector<core::Operation>& operations, std::set<std::string>& affected, QString& error) {
    error.clear();
    if (!source.is_object() || !source.contains("kind") || !source.at("kind").is_string()) {
        error = QStringLiteral("A semantic operation requires a kind."); return false;
    }
    for (const auto& schema : codex::authoringOperationSchemas()) {
        if (schema.at("properties").at("kind").at("const") != source.at("kind")) continue;
        if (!validate(source, schema, codex::authoringSchemaDefinitions(), error, QStringLiteral("operation"))) return false;
        auto candidate = working;
        std::vector<core::Operation> planned;
        auto changed = affected;
        if (!plan(candidate, source, planned, changed, error)) return false;
        working = std::move(candidate);
        affected = std::move(changed);
        operations.insert(operations.end(), std::make_move_iterator(planned.begin()), std::make_move_iterator(planned.end()));
        return true;
    }
    error = QStringLiteral("Unsupported semantic operation.");
    return false;
}
}  // namespace docxstudio::app
