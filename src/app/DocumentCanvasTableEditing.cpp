#include "docxstudio/app/DocumentCanvas.h"

#include <QColor>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace docxstudio::app {
namespace {
using Cell = std::pair<std::size_t, std::size_t>;
std::set<std::size_t> selectedIndices(const std::vector<Cell>& cells, bool columns) {
    std::set<std::size_t> result;
    for (const auto& cell : cells) result.insert(columns ? cell.second : cell.first);
    return result;
}
using CellPresentations = std::map<core::NodeId, const ImportedTableCellPresentation*>;
CellPresentations inheritedPresentations(const std::vector<ImportedTablePresentation>& imported, const core::Table& table) {
    const auto found = std::find_if(imported.begin(), imported.end(), [&table](const auto& value) { return value.tableId == table.id(); });
    if (found == imported.end()) return {};
    CellPresentations result;
    if (!found->cellIds.empty() && found->cellIds.size() == found->cells.size()) {
        for (std::size_t index = 0; index < found->cells.size(); ++index)
            if (!result.emplace(found->cellIds[index], &found->cells[index]).second) return {};
    } else if (found->cellIds.empty() && found->cells.size() == table.cells().size()) {
        for (std::size_t index = 0; index < found->cells.size(); ++index) result.emplace(table.cells()[index].id, &found->cells[index]);
    }
    return result;
}
std::pair<core::ParagraphAlignment, core::TableCellVerticalAlignment> effectiveCellAlignment(
    const core::TableCell& cell, const CellPresentations& presentations) {
    const auto found = presentations.find(cell.id);
    const auto* inherited = found == presentations.end() ? nullptr : found->second;
    const auto horizontal = cell.paragraph_format.alignment.value_or(inherited
        ? inherited->alignment.value_or(core::ParagraphAlignment::left) : core::ParagraphAlignment::left);
    const auto vertical = cell.layout.vertical_alignment.value_or(!inherited ? core::TableCellVerticalAlignment::top :
        inherited->verticalAlignment == ImportedCellVerticalAlignment::center ? core::TableCellVerticalAlignment::center :
        inherited->verticalAlignment == ImportedCellVerticalAlignment::bottom ? core::TableCellVerticalAlignment::bottom : core::TableCellVerticalAlignment::top);
    return {horizontal, vertical};
}
}

std::vector<core::NodeId> DocumentCanvas::selectedTableCellIds() const {
    std::vector<core::NodeId> result;
    if (headerFooterEditing_ || !selectedTable_) return result;
    const auto snapshot = visibleDocumentSnapshot();
    const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return result;
    for (const auto& [row, column] : selectedTableCells(*table))
        if (const auto* cell = table->cell(row, column)) result.push_back(cell->id);
    return result;
}

std::optional<TableCellRange> DocumentCanvas::selectedTableCellRange() const {
    if (headerFooterEditing_ || !tableCursor_ || tableCellSelection_) return std::nullopt;
    const auto snapshot = visibleDocumentSnapshot();
    const auto* table = snapshot.document.findTable(tableCursor_->tableId);
    const auto* cell = table ? table->cell(tableCursor_->row, tableCursor_->column) : nullptr;
    if (!cell) return std::nullopt;
    const auto focus = std::min(tableCursor_->utf16Offset, cell->text.size());
    const auto anchor = std::min(tableSelectionAnchor_.value_or(focus), cell->text.size());
    return TableCellRange{table->id(), cell->id, tableCursor_->row, tableCursor_->column, std::min(anchor, focus), std::max(anchor, focus)};
}

std::optional<std::pair<core::ParagraphAlignment, core::TableCellVerticalAlignment>>
DocumentCanvas::selectedTableCellAlignment() const {
    if (headerFooterEditing_ || previewId_ || !selectedTable_) return std::nullopt;
    const auto snapshot = session_->snapshot();
    const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return std::nullopt;
    const auto presentations = inheritedPresentations(importedTables_, *table);
    std::optional<std::pair<core::ParagraphAlignment, core::TableCellVerticalAlignment>> common;
    for (const auto& [row, column] : selectedTableCells(*table)) {
        const auto* cell = table->cell(row, column);
        const auto value = effectiveCellAlignment(*cell, presentations);
        if (common && *common != value) return std::nullopt;
        common = value;
    }
    return common;
}

bool DocumentCanvas::setSelectedTableCellAlignment(core::ParagraphAlignment horizontal, core::TableCellVerticalAlignment vertical) {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    core::ParagraphFormat probe; probe.alignment = horizontal;
    core::TableCellLayout layoutProbe; layoutProbe.vertical_alignment = vertical;
    if (!probe.validate() || !layoutProbe.validate()) return false;
    const auto snapshot = session_->snapshot();
    const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto presentations = inheritedPresentations(importedTables_, *table);
    std::vector<core::Operation> operations;
    for (const auto& [row, column] : selectedTableCells(*table)) {
        const auto* cell = table->cell(row, column);
        const auto current = effectiveCellAlignment(*cell, presentations);
        if (current.first != horizontal) {
            core::ParagraphFormatDelta delta;
            delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(horizontal);
            operations.emplace_back(core::SetTableCellParagraphFormat{table->id(), row, column, delta});
        }
        if (current.second != vertical) {
            auto layout = cell->layout; layout.vertical_alignment = vertical;
            operations.emplace_back(core::SetTableCellLayout{table->id(), row, column, std::move(layout)});
        }
    }
    return operations.empty() || apply(std::move(operations));
}

bool DocumentCanvas::setSelectedTableBorders(const QString& preset, double widthPoints, const QColor& color) {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const QStringList supported{QStringLiteral("all"), QStringLiteral("outside"), QStringLiteral("inside"), QStringLiteral("none"),
        QStringLiteral("top"), QStringLiteral("bottom"), QStringLiteral("left"), QStringLiteral("right")};
    if (!supported.contains(preset) || !std::isfinite(widthPoints) || widthPoints < 0.125 || widthPoints > 12.0 || !color.isValid() || color.alpha() != 255) return false;
    const auto snapshot = session_->snapshot();
    const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto cells = selectedTableCells(*table);
    if (cells.empty()) return false;
    const auto rows = selectedIndices(cells, false), columns = selectedIndices(cells, true);
    const core::TableBorder border{preset == QStringLiteral("none") ? core::TableBorderStyle::none : core::TableBorderStyle::single,
        static_cast<std::uint16_t>(preset == QStringLiteral("none") ? 0LL : std::llround(widthPoints * 8.0)), static_cast<std::uint32_t>(color.rgba())};
    enum class Side { top, right, bottom, left };
    std::map<Cell, core::TableCellLayout> layouts;
    const auto edge = [&](std::size_t row, std::size_t column, Side side) {
        auto [found, inserted] = layouts.try_emplace(Cell{row, column}, table->cell(row, column)->layout);
        static_cast<void>(inserted);
        auto& layout = found->second;
        switch (side) {
            case Side::top: layout.border_top = border; break;
            case Side::right: layout.border_right = border; break;
            case Side::bottom: layout.border_bottom = border; break;
            case Side::left: layout.border_left = border; break;
        }
    };
    const auto sharedEdge = [&](std::size_t row, std::size_t column, Side side) {
        edge(row, column, side);
        if (side == Side::top && row > 0) edge(row - 1, column, Side::bottom);
        if (side == Side::bottom && row + 1 < table->rowCount()) edge(row + 1, column, Side::top);
        if (side == Side::left && column > 0) edge(row, column - 1, Side::right);
        if (side == Side::right && column + 1 < table->columnCount()) edge(row, column + 1, Side::left);
    };
    for (const auto& [row, column] : cells) {
        const bool top = row == *rows.begin(), bottom = row == *rows.rbegin();
        const bool left = column == *columns.begin(), right = column == *columns.rbegin();
        const auto shouldSet = [&](const QString& name, bool outside) {
            return preset == QStringLiteral("all") || preset == QStringLiteral("none") ||
                (preset == QStringLiteral("outside") && outside) || (preset == QStringLiteral("inside") && !outside) ||
                (preset == name && outside);
        };
        if (shouldSet(QStringLiteral("top"), top)) sharedEdge(row, column, Side::top);
        if (shouldSet(QStringLiteral("bottom"), bottom)) sharedEdge(row, column, Side::bottom);
        if (shouldSet(QStringLiteral("left"), left)) sharedEdge(row, column, Side::left);
        if (shouldSet(QStringLiteral("right"), right)) sharedEdge(row, column, Side::right);
    }
    std::vector<core::Operation> operations;
    for (auto& [cell, layout] : layouts) if (layout != table->cell(cell.first, cell.second)->layout)
        operations.emplace_back(core::SetTableCellLayout{table->id(), cell.first, cell.second, std::move(layout)});
    return operations.empty() || apply(std::move(operations));
}

double DocumentCanvas::selectedTableColumnWidthPoints() const {
    if (headerFooterEditing_ || previewId_ || !selectedTable_) return -1;
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return -1;
    const auto columns = selectedIndices(selectedTableCells(*table), true);
    const auto geometry = resolvedTableGeometry(table->id());
    std::optional<std::uint32_t> common;
    for (const auto column : columns) {
        if (column >= geometry.column_widths_twips.size()) return -1;
        const auto width = geometry.column_widths_twips[column];
        if (common && *common != width) return -1;
        common = width;
    }
    return common ? static_cast<double>(*common) / 20.0 : -1;
}

double DocumentCanvas::selectedTableRowMinimumHeightPoints() const {
    if (headerFooterEditing_ || previewId_ || !selectedTable_) return -1;
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return -1;
    std::optional<std::uint32_t> common;
    for (const auto row : selectedIndices(selectedTableCells(*table), false)) {
        const auto height = table->geometry().row_minimum_heights_twips.empty() ? 0U : table->geometry().row_minimum_heights_twips[row];
        if (common && *common != height) return -1;
        common = height;
    }
    return common ? static_cast<double>(*common) / 20.0 : -1;
}

bool DocumentCanvas::setSelectedTableColumnWidth(double points) {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    if (!std::isfinite(points) || points < 18 || points > 1584) return false;
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto columns = selectedIndices(selectedTableCells(*table), true);
    if (columns.empty()) return false;
    const auto width = static_cast<std::uint32_t>(std::llround(points * 20.0));
    auto geometry = table->geometry();
    geometry.column_widths_twips = resolvedTableGeometry(table->id()).column_widths_twips;
    if (geometry.column_widths_twips.size() != table->columnCount()) return false;
    bool changed = false;
    for (const auto column : columns) { changed |= geometry.column_widths_twips[column] != width; geometry.column_widths_twips[column] = width; }
    if (!changed) return true;
    const auto total = std::accumulate(geometry.column_widths_twips.begin(), geometry.column_widths_twips.end(), std::uint64_t{0});
    if (static_cast<double>(total) > std::max(0.0, (pageWidthPoints_ - marginLeftPoints_ - marginRightPoints_) * 20.0) + 0.01 || !geometry.validate(table->rowCount(), table->columnCount())) return false;
    return apply({core::SetTableGeometry{table->id(), std::move(geometry)}});
}

bool DocumentCanvas::setSelectedTableRowMinimumHeight(double points) {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    ensureLayout();
    const auto capacity = std::min(1584.0, std::max(0.0, pageHeightPoints_ - bodyMarginTopPoints_ - bodyMarginBottomPoints_));
    if (!std::isfinite(points) || points < 0 || points > capacity) return false;
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto rows = selectedIndices(selectedTableCells(*table), false);
    if (rows.empty()) return false;
    const auto height = static_cast<std::uint32_t>(std::llround(points * 20.0));
    auto geometry = table->geometry();
    if (geometry.row_minimum_heights_twips.empty()) geometry.row_minimum_heights_twips.resize(table->rowCount(), 0);
    bool changed = false;
    for (const auto row : rows) { changed |= geometry.row_minimum_heights_twips[row] != height; geometry.row_minimum_heights_twips[row] = height; }
    if (!changed) return true;
    return apply({core::SetTableGeometry{table->id(), std::move(geometry)}});
}

bool DocumentCanvas::distributeSelectedTableColumns() {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto columns = selectedIndices(selectedTableCells(*table), true);
    if (columns.empty()) return false;
    if (columns.size() == 1) return true;
    auto geometry = table->geometry();
    geometry.column_widths_twips = resolvedTableGeometry(table->id()).column_widths_twips;
    if (geometry.column_widths_twips.size() != table->columnCount()) return false;
    std::uint32_t total = 0;
    for (const auto column : columns) total += geometry.column_widths_twips[column];
    const auto width = static_cast<std::uint32_t>(total / columns.size());
    auto remainder = total % columns.size();
    if (width < 360) return false;
    bool changed = false;
    for (const auto column : columns) { const auto next = width + (remainder > 0 ? 1U : 0U); if (remainder) --remainder;
        changed |= geometry.column_widths_twips[column] != next; geometry.column_widths_twips[column] = next; }
    return !changed || apply({core::SetTableGeometry{table->id(), std::move(geometry)}});
}

bool DocumentCanvas::distributeSelectedTableRows() {
    if (headerFooterEditing_ || !selectedTable_ || rejectLiveEditDuringPreview()) return false;
    ensureLayout();
    const auto snapshot = session_->snapshot(); const auto* table = snapshot.document.findTable(*selectedTable_);
    if (!table) return false;
    const auto rows = selectedIndices(selectedTableCells(*table), false);
    if (rows.empty()) return false;
    if (rows.size() == 1) return true;
    const auto measured = resolvedTableGeometry(table->id()).row_minimum_heights_twips;
    if (measured.size() != table->rowCount()) return false;
    std::uint32_t maximum = 0; bool equal = true;
    for (const auto row : rows) { if (maximum && maximum != measured[row]) equal = false; maximum = std::max(maximum, measured[row]); }
    if (equal) return true;
    if (static_cast<double>(maximum) > std::min(31680.0, std::max(0.0, pageHeightPoints_ - bodyMarginTopPoints_ - bodyMarginBottomPoints_) * 20.0)) return false;
    auto geometry = table->geometry();
    if (geometry.row_minimum_heights_twips.empty()) geometry.row_minimum_heights_twips.resize(table->rowCount(), 0);
    for (const auto row : rows) geometry.row_minimum_heights_twips[row] = maximum;
    return apply({core::SetTableGeometry{table->id(), std::move(geometry)}});
}
} // namespace docxstudio::app
