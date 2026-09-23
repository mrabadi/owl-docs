#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ExcalidrawFigure.h"

#include "docxstudio/app/MathLayout.h"
#include "docxstudio/app/RasterDecoder.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/math/latex_parser.h"

#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontMetricsF>
#include <QFocusEvent>
#include <QFrame>
#include <QFormLayout>
#include <QGlyphRun>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRawFont>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtPrintSupport/QPrinter>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iterator>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace docxstudio::app {
namespace {

constexpr double kScreenPointsScale = 96.0 / 72.0;
constexpr double kPageGapPixels = 24.0;
constexpr double kCanvasPaddingPixels = 28.0;
constexpr double kEmuPerPoint = 12700.0;
constexpr char kInlineImageClipboardLegacyMime[] =
    "application/x-owl-docs-inline-image-v1";
constexpr char kInlineImageClipboardMime[] =
    "application/x-owl-docs-inline-image-v2";
constexpr char kInlineImageClipboardLegacyMagic[] = "OWLDIMG1";
constexpr char kInlineImageClipboardMagic[] = "OWLDIMG2";
constexpr qsizetype kInlineImageClipboardLegacyHeaderBytes = 33;
constexpr qsizetype kInlineImageClipboardHeaderBytes = 67;

class StoryTextEdit final : public QPlainTextEdit {
public:
    using PasteImage = std::function<void(const QMimeData*)>;

    explicit StoryTextEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent) {}

    PasteImage pasteImage;
    std::function<void()> activated;

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return hasPicture(source) ||
            QPlainTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData* source) override {
        if (hasPicture(source) && pasteImage) {
            pasteImage(source);
            return;
        }
        QPlainTextEdit::insertFromMimeData(source);
    }

    void focusInEvent(QFocusEvent* event) override {
        QPlainTextEdit::focusInEvent(event);
        if (activated) activated();
    }

private:
    static bool hasPicture(const QMimeData* source) {
        return source &&
            (source->hasFormat(QString::fromLatin1(
                 kInlineImageClipboardMime)) ||
             source->hasFormat(QString::fromLatin1(
                 kInlineImageClipboardLegacyMime)) ||
             source->hasFormat(QStringLiteral("image/png")) ||
             source->hasFormat(QStringLiteral("image/jpeg")) ||
             source->hasImage());
    }
};

bool isPasteTextOnlyShortcut(const QKeyEvent& event) {
    constexpr auto relevantModifiers =
        Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier |
        Qt::MetaModifier;
    return event.key() == Qt::Key_V &&
           (event.modifiers() & relevantModifiers) ==
               (Qt::ControlModifier | Qt::ShiftModifier);
}

struct ClipboardInlineImage {
    core::ImageFormat format{core::ImageFormat::png};
    std::int64_t widthEmu{};
    std::int64_t heightEmu{};
    core::ImageLayout layout;
    QString accessibleName;
    std::vector<std::uint8_t> encodedBytes;
};

void appendLittleEndian(QByteArray& output, std::uint64_t value,
                        int byteCount) {
    for (int byte = 0; byte < byteCount; ++byte) {
        output.append(static_cast<char>((value >> (byte * 8)) & 0xffU));
    }
}

bool readLittleEndian(const QByteArray& input, qsizetype& cursor,
                      int byteCount, std::uint64_t& value) {
    if (cursor < 0 || byteCount < 0 ||
        cursor > input.size() - byteCount) {
        return false;
    }
    value = 0;
    for (int byte = 0; byte < byteCount; ++byte) {
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(input.at(cursor + byte)))
                 << (byte * 8);
    }
    cursor += byteCount;
    return true;
}

QByteArray encodeClipboardInlineImage(const core::ImageAtom& image,
                                      bool legacy = false) {
    const auto bytes = image.encoded_payload.bytes();
    if (bytes.empty() || bytes.size() > core::kMaximumEncodedImageBytes ||
        image.accessible_name.size() >
            core::kMaximumImageAccessibleNameBytes ||
        image.width_emu <= 0 || image.height_emu <= 0 ||
        image.width_emu > core::kMaximumInlineImageDimensionEmu ||
        image.height_emu > core::kMaximumInlineImageDimensionEmu ||
        !image.layout.validate()) {
        return {};
    }
    const QByteArray name(
        image.accessible_name.data(),
        static_cast<qsizetype>(image.accessible_name.size()));
    const qsizetype payloadSize = static_cast<qsizetype>(bytes.size());
    QByteArray output;
    output.reserve((legacy ? kInlineImageClipboardLegacyHeaderBytes
                           : kInlineImageClipboardHeaderBytes) + name.size() +
                   payloadSize);
    output.append(legacy ? kInlineImageClipboardLegacyMagic
                         : kInlineImageClipboardMagic, 8);
    output.append(image.format == core::ImageFormat::png ? '\x01' : '\x02');
    appendLittleEndian(output, static_cast<std::uint64_t>(image.width_emu), 8);
    appendLittleEndian(output, static_cast<std::uint64_t>(image.height_emu), 8);
    if (!legacy) {
        output.append(static_cast<char>(image.layout.placement));
        appendLittleEndian(
            output, static_cast<std::uint64_t>(
                        image.layout.distance_top_emu), 8);
        appendLittleEndian(
            output, static_cast<std::uint64_t>(
                        image.layout.distance_right_emu), 8);
        appendLittleEndian(
            output, static_cast<std::uint64_t>(
                        image.layout.distance_bottom_emu), 8);
        appendLittleEndian(
            output, static_cast<std::uint64_t>(
                        image.layout.distance_left_emu), 8);
        output.append(image.layout.move_with_text ? '\x01' : '\x00');
    }
    appendLittleEndian(output, static_cast<std::uint64_t>(name.size()), 4);
    appendLittleEndian(output, static_cast<std::uint64_t>(payloadSize), 4);
    output.append(name);
    output.append(reinterpret_cast<const char*>(bytes.data()), payloadSize);
    return output;
}

std::optional<ClipboardInlineImage> decodeClipboardInlineImage(
    const QByteArray& input) {
    const bool legacy = input.size() >= kInlineImageClipboardLegacyHeaderBytes &&
        input.first(8) == QByteArray(kInlineImageClipboardLegacyMagic, 8);
    const bool current = input.size() >= kInlineImageClipboardHeaderBytes &&
        input.first(8) == QByteArray(kInlineImageClipboardMagic, 8);
    if (!legacy && !current) {
        return std::nullopt;
    }
    qsizetype cursor = 8;
    const unsigned char formatByte =
        static_cast<unsigned char>(input.at(cursor++));
    if (formatByte != 1U && formatByte != 2U) return std::nullopt;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t nameLength = 0;
    std::uint64_t payloadLength = 0;
    if (!readLittleEndian(input, cursor, 8, width) ||
        !readLittleEndian(input, cursor, 8, height)) {
        return std::nullopt;
    }
    core::ImageLayout layout;
    if (current) {
        if (cursor >= input.size()) return std::nullopt;
        layout.placement = static_cast<core::ImagePlacement>(
            static_cast<unsigned char>(input.at(cursor++)));
        std::uint64_t top = 0;
        std::uint64_t right = 0;
        std::uint64_t bottom = 0;
        std::uint64_t left = 0;
        if (!readLittleEndian(input, cursor, 8, top) ||
            !readLittleEndian(input, cursor, 8, right) ||
            !readLittleEndian(input, cursor, 8, bottom) ||
            !readLittleEndian(input, cursor, 8, left) ||
            cursor >= input.size()) {
            return std::nullopt;
        }
        layout.distance_top_emu = static_cast<std::int64_t>(top);
        layout.distance_right_emu = static_cast<std::int64_t>(right);
        layout.distance_bottom_emu = static_cast<std::int64_t>(bottom);
        layout.distance_left_emu = static_cast<std::int64_t>(left);
        const unsigned char move =
            static_cast<unsigned char>(input.at(cursor++));
        if (move > 1U) return std::nullopt;
        layout.move_with_text = move == 1U;
        if (!layout.validate()) return std::nullopt;
    }
    if (!readLittleEndian(input, cursor, 4, nameLength) ||
        !readLittleEndian(input, cursor, 4, payloadLength) ||
        width == 0 || height == 0 ||
        width > static_cast<std::uint64_t>(
                    core::kMaximumInlineImageDimensionEmu) ||
        height > static_cast<std::uint64_t>(
                     core::kMaximumInlineImageDimensionEmu) ||
        nameLength > core::kMaximumImageAccessibleNameBytes ||
        payloadLength == 0 ||
        payloadLength > core::kMaximumEncodedImageBytes ||
        nameLength > static_cast<std::uint64_t>(input.size() - cursor)) {
        return std::nullopt;
    }
    cursor += static_cast<qsizetype>(nameLength);
    if (payloadLength !=
        static_cast<std::uint64_t>(input.size() - cursor)) {
        return std::nullopt;
    }
    const qsizetype nameOffset = cursor -
        static_cast<qsizetype>(nameLength);
    const QByteArray nameBytes = input.mid(
        nameOffset, static_cast<qsizetype>(nameLength));
    const QString name = QString::fromUtf8(nameBytes);
    if (name.toUtf8() != nameBytes) return std::nullopt;
    const auto expected = formatByte == 1U
        ? raster::Format::png
        : raster::Format::jpeg;
    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    const auto payload = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input.constData() + cursor),
        static_cast<std::size_t>(payloadLength));
    const auto inspection = raster::inspect(payload, expected, limits);
    if (!inspection.ok()) return std::nullopt;
    ClipboardInlineImage result;
    result.format = formatByte == 1U ? core::ImageFormat::png
                                     : core::ImageFormat::jpeg;
    result.widthEmu = static_cast<std::int64_t>(width);
    result.heightEmu = static_cast<std::int64_t>(height);
    result.layout = layout;
    result.accessibleName = name;
    result.encodedBytes.assign(payload.begin(), payload.end());
    return result;
}

QString fromUtf16(const std::u16string& text) {
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

std::u16string toUtf16(const QString& text) { return text.toStdU16String(); }

struct SearchMatchRange {
    std::size_t start{};
    std::size_t end{};
};

bool boundaryAt(QTextBoundaryFinder& finder, qsizetype position) {
    finder.setPosition(position);
    return finder.isAtBoundary();
}

bool wholeWordMatch(QTextBoundaryFinder& finder, qsizetype start,
                    qsizetype end) {
    finder.setPosition(start);
    if (!(finder.boundaryReasons() & QTextBoundaryFinder::StartOfItem)) {
        return false;
    }
    finder.setPosition(end);
    return finder.boundaryReasons() & QTextBoundaryFinder::EndOfItem;
}

std::vector<SearchMatchRange> searchMatchRanges(
    const QString& text, const QString& needle,
    const DocumentSearchOptions& options) {
    std::vector<SearchMatchRange> matches;
    if (needle.isEmpty() || needle.size() > text.size()) return matches;
    const Qt::CaseSensitivity sensitivity = options.caseSensitive
        ? Qt::CaseSensitive
        : Qt::CaseInsensitive;
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    qsizetype offset = 0;
    while (offset <= text.size() - needle.size()) {
        const qsizetype start = text.indexOf(needle, offset, sensitivity);
        if (start < 0) break;
        const qsizetype end = start + needle.size();
        offset = std::max(start + 1, end);
        if (!boundaryAt(graphemes, start) ||
            !boundaryAt(graphemes, end)) {
            continue;
        }
        if (options.wholeWord && !wholeWordMatch(words, start, end)) {
            continue;
        }
        matches.push_back({static_cast<std::size_t>(start),
                           static_cast<std::size_t>(end)});
    }
    return matches;
}

std::size_t searchHitStart(const DocumentSearchHit& hit) {
    return std::visit(
        [](const auto& target) { return target.startUtf16; }, hit.target);
}

std::size_t searchHitEnd(const DocumentSearchHit& hit) {
    return std::visit(
        [](const auto& target) { return target.endUtf16; }, hit.target);
}

bool sameSearchUnit(
    const std::variant<BodyParagraphSearchHit, TableCellSearchHit>& left,
    const std::variant<BodyParagraphSearchHit, TableCellSearchHit>& right) {
    if (left.index() != right.index()) return false;
    if (const auto* leftParagraph =
            std::get_if<BodyParagraphSearchHit>(&left)) {
        const auto& rightParagraph = std::get<BodyParagraphSearchHit>(right);
        return leftParagraph->paragraphId == rightParagraph.paragraphId;
    }
    const auto& leftCell = std::get<TableCellSearchHit>(left);
    const auto& rightCell = std::get<TableCellSearchHit>(right);
    return leftCell.tableId == rightCell.tableId &&
           leftCell.cellId == rightCell.cellId &&
           leftCell.row == rightCell.row &&
           leftCell.column == rightCell.column;
}

std::optional<std::size_t> searchUnitOrdinal(
    const core::Document& document,
    const std::variant<BodyParagraphSearchHit, TableCellSearchHit>& target) {
    std::size_t ordinal = 0;
    for (const auto& block : document.bodyBlocks()) {
        if (block.kind == core::BodyBlockKind::paragraph) {
            if (const auto* paragraph =
                    std::get_if<BodyParagraphSearchHit>(&target);
                paragraph && paragraph->paragraphId == block.id) {
                return ordinal;
            }
            ++ordinal;
            continue;
        }
        const auto* table = document.findTable(block.id);
        if (!table) continue;
        for (std::size_t row = 0; row < table->rowCount(); ++row) {
            for (std::size_t column = 0; column < table->columnCount();
                 ++column) {
                const auto* cell = table->cell(row, column);
                if (const auto* requested =
                        std::get_if<TableCellSearchHit>(&target);
                    requested && cell && requested->tableId == table->id() &&
                    requested->cellId == cell->id && requested->row == row &&
                    requested->column == column) {
                    return ordinal;
                }
                ++ordinal;
            }
        }
    }
    return std::nullopt;
}

std::size_t mapSearchOffset(
    std::size_t originalOffset,
    const std::vector<SearchMatchRange>& matches,
    std::size_t replacementLength) {
    std::int64_t shift = 0;
    for (const auto& match : matches) {
        if (originalOffset < match.start) break;
        const auto mappedStart = static_cast<std::int64_t>(match.start) + shift;
        if (originalOffset <= match.end) {
            return static_cast<std::size_t>(
                mappedStart + static_cast<std::int64_t>(replacementLength));
        }
        shift += static_cast<std::int64_t>(replacementLength) -
                 static_cast<std::int64_t>(match.end - match.start);
    }
    return static_cast<std::size_t>(
        static_cast<std::int64_t>(originalOffset) + shift);
}

struct SpellingWord {
    std::size_t start{};
    std::size_t end{};
    QString text;
};

void fillSelectionBackground(QPainter& painter, const QTextLine& line,
                             int start, int end) {
    if (start >= end) return;
    const QColor highlight(51, 132, 255, 70);
    bool painted = false;
    // One logical selection may occupy several disjoint visual runs in bidi
    // text.  QTextLine::glyphRuns() returns those shaped visual fragments;
    // painting each fragment avoids the incorrect single rectangle between
    // two logical cursor positions.
    for (const QGlyphRun& run : line.glyphRuns(start, end - start)) {
        const QRectF bounds = run.boundingRect();
        if (!bounds.isValid() || bounds.width() <= 0.0) continue;
        painter.fillRect(
            QRectF(bounds.left(), line.y(), bounds.width(), line.height()),
            highlight);
        painted = true;
    }
    // Whitespace-only ranges may not produce a glyph run.  Preserve their
    // visible selection using the two caret edges as a bounded fallback.
    if (!painted) {
        const qreal first = line.cursorToX(start);
        const qreal last = line.cursorToX(end);
        painter.fillRect(
            QRectF(std::min(first, last), line.y(), std::abs(last - first),
                   line.height()),
            highlight);
    }
}

std::vector<SpellingWord> spellingWords(const QString& text) {
    // Hunspell is currently configured with the local en-US dictionary. Keep
    // its candidates Latin, but recognize both straight and typographic
    // apostrophes so contractions are treated consistently by painting and
    // the context menu. Tokenize the complete paragraph/cell rather than each
    // visual line: a wrapped word must be checked once as one word.
    static const QRegularExpression expression(
        QStringLiteral("[A-Za-z]+(?:['\u2019][A-Za-z]+)*['\u2019]?"));
    std::vector<SpellingWord> result;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        // Preserve the prior behavior of leaving one-letter fragments alone.
        if (match.capturedLength() < 2) continue;
        result.push_back(SpellingWord{
            static_cast<std::size_t>(match.capturedStart()),
            static_cast<std::size_t>(match.capturedEnd()),
            match.captured()});
    }
    return result;
}

std::optional<SpellingWord> spellingWordAt(
    const QString& text, std::size_t utf16Offset) {
    utf16Offset = std::min(
        utf16Offset, static_cast<std::size_t>(text.size()));
    for (const auto& word : spellingWords(text)) {
        // A caret immediately after the last typed character is still on the
        // word. Delimiter input advances it beyond this inclusive interval.
        if (utf16Offset >= word.start && utf16Offset <= word.end) {
            return word;
        }
    }
    return std::nullopt;
}

bool canContinueSpellingWord(QChar character) {
    return (character >= QLatin1Char('A') &&
            character <= QLatin1Char('Z')) ||
           (character >= QLatin1Char('a') &&
            character <= QLatin1Char('z')) ||
           character == QLatin1Char('\'') || character == QChar(0x2019);
}

QColor fromArgb(std::uint32_t value) {
    return QColor::fromRgba(value);
}

std::uint32_t toArgb(const QColor& color) {
    return static_cast<std::uint32_t>(color.rgba());
}

constexpr std::uint32_t kDefaultTextArgb = 0xff000000U;

struct TableStylePalette {
    std::optional<std::uint32_t> headerFillArgb;
    std::optional<std::uint32_t> bandFillArgb;
    std::uint32_t borderArgb{0xffb7b7b7U};
    std::uint32_t headerTextArgb{kDefaultTextArgb};
    double borderWidthPoints{0.5};
    bool bandedRows{false};
};

TableStylePalette tableStylePalette(core::TableStyle style) {
    using enum core::TableStyle;
    switch (style) {
        case plain:
            return {{}, {}, 0xffb7b7b7U, kDefaultTextArgb, 0.35, false};
        case grid:
            return {{}, {}, 0xff444444U, kDefaultTextArgb, 0.65, false};
        case light_gray:
            return {0xffd9e1f2U, 0xfff2f2f2U, 0xffa6a6a6U,
                    kDefaultTextArgb, 0.5, true};
        case light_blue:
            return {0xff5b9bd5U, 0xffddebf7U, 0xff9eadbaU,
                    0xffffffffU, 0.55, true};
        case light_orange:
            return {0xffe95420U, 0xfffbe9e1U, 0xffc88a73U,
                    0xffffffffU, 0.55, true};
        case medium_blue:
            return {0xff2f75b5U, 0xffd9eaf7U, 0xff2f75b5U,
                    0xffffffffU, 0.7, true};
        case medium_green:
            return {0xff548235U, 0xffe2f0d9U, 0xff548235U,
                    0xffffffffU, 0.7, true};
        case medium_orange:
            return {0xffc65911U, 0xfffce4d6U, 0xffc65911U,
                    0xffffffffU, 0.7, true};
        case aubergine:
            return {0xff77216fU, 0xffefe3eeU, 0xff5e2750U,
                    0xffffffffU, 0.7, true};
        case orange_accent:
            return {0xffe95420U, 0xfffff2edU, 0xff77216fU,
                    0xffffffffU, 0.75, true};
        case banded_blue:
            return {0xff1f4e78U, 0xffd9eaf7U, 0xff5b9bd5U,
                    0xffffffffU, 0.65, true};
        case banded_aubergine:
            return {0xff5e2750U, 0xffeadde8U, 0xff77216fU,
                    0xffffffffU, 0.65, true};
        case dark_header:
            return {0xff262626U, 0xfff2f2f2U, 0xff7f7f7fU,
                    0xffffffffU, 0.65, true};
    }
    return {};
}

std::optional<std::uint32_t> tableStyleCellFill(
    const TableStylePalette& palette, bool headerRow, std::size_t row) {
    if (headerRow && row == 0) return palette.headerFillArgb;
    const std::size_t contentRow = row - (headerRow ? 1U : 0U);
    if (palette.bandedRows && contentRow % 2U == 1U) {
        return palette.bandFillArgb;
    }
    return std::nullopt;
}

math::ParseLimits editorEquationLimits() {
    math::ParseLimits limits;
    limits.max_input_bytes = 8U * 1024U;
    limits.max_depth = 32;
    limits.max_nodes = 2'048;
    limits.max_matrix_rows = 32;
    limits.max_matrix_columns = 32;
    return limits;
}

QString equationFallback(const core::EquationAtom& equation) {
    const QString source = QString::fromUtf8(
        equation.canonical_latex.data(),
        static_cast<qsizetype>(equation.canonical_latex.size()));
    return equation.display
        ? QStringLiteral("\\[%1\\]").arg(source)
        : QStringLiteral("\\(%1\\)").arg(source);
}

QString visibleParagraphText(const core::Paragraph& paragraph,
                             std::size_t start, std::size_t end) {
    start = std::min(start, paragraph.text().size());
    end = std::clamp(end, start, paragraph.text().size());
    QString result;
    std::size_t cursor = start;
    while (cursor < end) {
        if (const auto* equation = paragraph.equationAt(cursor)) {
            result += equationFallback(*equation);
            ++cursor;
            continue;
        }
        if (const auto* image = paragraph.imageAt(cursor)) {
            const QString name = QString::fromStdString(
                image->accessible_name).trimmed();
            result += name.isEmpty()
                ? QStringLiteral("[Picture]")
                : QStringLiteral("[Picture: %1]").arg(name);
            ++cursor;
            continue;
        }
        const auto next = std::find(
            paragraph.text().begin() + static_cast<std::ptrdiff_t>(cursor),
            paragraph.text().begin() + static_cast<std::ptrdiff_t>(end),
            core::kInlineObjectReplacementCharacter);
        const auto nextOffset = static_cast<std::size_t>(
            std::distance(paragraph.text().begin(), next));
        if (nextOffset == cursor) {
            result += QChar(core::kInlineObjectReplacementCharacter);
            ++cursor;
            continue;
        }
        result += fromUtf16(paragraph.text().substr(
            cursor, nextOffset - cursor));
        cursor = nextOffset;
    }
    return result;
}

QFont fontFrom(const core::CharacterFormat& format,
               const QString& defaultFamily = QStringLiteral("Helvetica"),
               double defaultPointSize = 11.0) {
    QFont font(format.font_family
                   ? QString::fromStdString(*format.font_family)
                   : defaultFamily);
    const double points = format.font_size_half_points
        ? *format.font_size_half_points / 2.0
        : defaultPointSize;
    const int pixelSize = std::max(
        1, static_cast<int>(std::lround(points)));
    font.setPixelSize(pixelSize);
    // QFont exposes fractional point sizes but only integral pixel sizes.
    // The layout vocabulary itself is in points (one logical unit per point),
    // so using point sizes here would apply the screen's DPI conversion a
    // second time. Preserve half-point advance metrics by compensating with a
    // horizontal stretch when the nearest integral pixel size was required.
    font.setStretch(std::clamp(
        static_cast<int>(std::lround(points * 100.0 / pixelSize)), 1, 4000));
    font.setBold(format.bold.value_or(false));
    font.setItalic(format.italic.value_or(false));
    font.setStrikeOut(format.strike.value_or(false));
    font.setUnderline(format.underline.value_or(core::UnderlineStyle::none) !=
                      core::UnderlineStyle::none);
    // Qt's PDF backend can omit the Unicode mapping for optional Latin
    // ligatures (notably "ft" in Carlito), making otherwise visible text
    // unsearchable. This flag suppresses cosmetic shaping where it is not
    // required while retaining the shaping required by complex scripts.
    font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        static_cast<int>(font.styleStrategy()) |
        static_cast<int>(QFont::PreferNoShaping)));
    return font;
}

double requestedPointSize(const core::CharacterFormat& format,
                          double defaultPointSize = 11.0) {
    return std::max(
        0.5, format.font_size_half_points
                 ? *format.font_size_half_points / 2.0
                 : defaultPointSize);
}

double unroundedLineAdvance(
    const core::CharacterFormat& format,
    const QString& defaultFamily = QStringLiteral("Helvetica"),
    double defaultPointSize = 11.0) {
    const double requestedPoints = requestedPointSize(format, defaultPointSize);
    const QFont font = fontFrom(format, defaultFamily, defaultPointSize);
    const QRawFont raw = QRawFont::fromFont(font);
    if (raw.isValid() && raw.pixelSize() > 0.0) {
        const double designAdvance =
            raw.ascent() + raw.descent() + raw.leading();
        if (std::isfinite(designAdvance) && designAdvance > 0.0) {
            // QRawFont exposes scaled, non-integer OpenType metrics. Rescale
            // from the realized whole-pixel QFont size to the exact OOXML
            // half-point request so 8 pt and 8.5 pt do not collapse to the
            // same (or adjacent, overly coarse) table line heights.
            return designAdvance * requestedPoints / raw.pixelSize();
        }
    }
    return std::max<double>(
        1.0, QFontMetricsF(font).lineSpacing() * requestedPoints /
                 std::max(1, font.pixelSize()));
}

QTextCharFormat qtFormat(const core::CharacterFormat& format,
                         const QString& defaultFamily = QStringLiteral("Helvetica"),
                         double defaultPointSize = 11.0) {
    QTextCharFormat result;
    result.setFont(fontFrom(format, defaultFamily, defaultPointSize));
    // A QTextLayout format without a foreground brush inherits the painter's
    // current pen. Decorations (page borders and spelling squiggles) change
    // that pen, so leaving this unspecified made text appear gray or red
    // depending on what had just been painted.
    result.setForeground(fromArgb(
        format.foreground_argb.value_or(kDefaultTextArgb)));
    if (format.highlight_argb) {
        result.setBackground(fromArgb(*format.highlight_argb));
    }
    if (format.baseline == core::BaselinePosition::superscript) {
        result.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    } else if (format.baseline == core::BaselinePosition::subscript) {
        result.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    }
    return result;
}

core::CharacterFormat resolvedCharacterFormat(
    core::CharacterFormat inherited,
    const core::CharacterFormat& specified) {
    if (specified.font_family) inherited.font_family = specified.font_family;
    if (specified.font_size_half_points) {
        inherited.font_size_half_points = specified.font_size_half_points;
    }
    if (specified.bold) inherited.bold = specified.bold;
    if (specified.italic) inherited.italic = specified.italic;
    if (specified.underline) inherited.underline = specified.underline;
    if (specified.strike) inherited.strike = specified.strike;
    if (specified.foreground_argb) {
        inherited.foreground_argb = specified.foreground_argb;
    }
    if (specified.highlight_argb) {
        inherited.highlight_argb = specified.highlight_argb;
    }
    if (specified.baseline) inherited.baseline = specified.baseline;
    if (specified.language) inherited.language = specified.language;
    return inherited;
}

Qt::Alignment paragraphAlignment(const core::ParagraphFormat& format) {
    switch (format.alignment.value_or(core::ParagraphAlignment::left)) {
        case core::ParagraphAlignment::center: return Qt::AlignHCenter;
        case core::ParagraphAlignment::right: return Qt::AlignRight;
        case core::ParagraphAlignment::justified:
        case core::ParagraphAlignment::distributed: return Qt::AlignJustify;
        case core::ParagraphAlignment::left: return Qt::AlignLeft;
    }
    return Qt::AlignLeft;
}

double emuToPoints(const std::optional<std::int64_t>& value) {
    return value ? static_cast<double>(*value) / kEmuPerPoint : 0.0;
}

QString errorText(const core::Error& error) {
    return QString::fromStdString(error.message);
}

core::CharacterFormat inheritedFormatAfterReplacement(
    const core::Document& source,
    const core::NormalizedRange& replacement) {
    auto context = source;
    if (!replacement.empty()) {
        const auto erased = context.deleteRange(
            {replacement.start, replacement.end});
        if (!erased) {
            return {};
        }
    }
    const auto* paragraph = context.findParagraph(replacement.start.paragraph_id);
    return paragraph
        ? paragraph->characterFormatAt(replacement.start.utf16_offset)
        : core::CharacterFormat{};
}

bool rangeUniformlyUsesFormat(const core::Document& document,
                              const core::NormalizedRange& range,
                              const core::CharacterFormat& format) {
    bool sawCharacter = false;
    for (std::size_t index = range.start_paragraph_index;
         index <= range.end_paragraph_index; ++index) {
        const auto& paragraph = document.paragraphs()[index];
        const std::size_t start = index == range.start_paragraph_index
            ? range.start.utf16_offset
            : 0;
        const std::size_t end = index == range.end_paragraph_index
            ? range.end.utf16_offset
            : paragraph.text().size();
        for (std::size_t offset = start; offset < end; ++offset) {
            sawCharacter = true;
            if (paragraph.characterFormatAt(offset + 1) != format) {
                return false;
            }
        }
    }
    return sawCharacter;
}

bool operationsHaveNonTextChanges(const core::Document& base,
                                  const std::vector<core::Operation>& operations) {
    auto working = base;
    for (const auto& operation : operations) {
        if (const auto* insert = std::get_if<core::InsertText>(&operation)) {
            const core::Range range{insert->position, insert->position};
            const auto normalized = working.normalizeRange(range);
            if (!normalized) return true;
            const auto inherited = inheritedFormatAfterReplacement(
                working, normalized.value());
            if (!insert->text.empty() && insert->format &&
                *insert->format != inherited) {
                return true;
            }
            if (!working.insertText(insert->position, insert->text, insert->format)) {
                return true;
            }
            continue;
        }
        if (const auto* replacement = std::get_if<core::ReplaceRange>(&operation)) {
            const auto normalized = working.normalizeRange(replacement->range);
            if (!normalized ||
                normalized.value().start_paragraph_index !=
                    normalized.value().end_paragraph_index) {
                return true;
            }
            const auto& paragraph = working.paragraphs()[
                normalized.value().start_paragraph_index];
            const auto paragraphId = paragraph.id();
            const auto markBefore = paragraph.paragraphMarkCharacterFormat();
            if (std::find(
                    paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                        normalized.value().start.utf16_offset),
                    paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                        normalized.value().end.utf16_offset),
                    core::kInlineObjectReplacementCharacter) !=
                paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                    normalized.value().end.utf16_offset)) {
                return true;
            }
            if (!replacement->text.empty()) {
                const auto desired = replacement->format.value_or(
                    inheritedFormatAfterReplacement(working, normalized.value()));
                if (normalized.value().empty()) {
                    const auto inherited = inheritedFormatAfterReplacement(
                        working, normalized.value());
                    if (replacement->format && desired != inherited) return true;
                } else if (!rangeUniformlyUsesFormat(
                               working, normalized.value(), desired)) {
                    return true;
                }
            }
            if (!working.replaceRange(replacement->range, replacement->text,
                                      replacement->format)) {
                return true;
            }
            const auto* changedParagraph = working.findParagraph(paragraphId);
            if (!changedParagraph ||
                changedParagraph->paragraphMarkCharacterFormat() != markBefore) {
                return true;
            }
            continue;
        }
        if (const auto* deletion = std::get_if<core::DeleteRange>(&operation)) {
            const auto normalized = working.normalizeRange(deletion->range);
            if (!normalized ||
                normalized.value().start_paragraph_index !=
                    normalized.value().end_paragraph_index) {
                return true;
            }
            const auto& paragraph = working.paragraphs()[
                normalized.value().start_paragraph_index];
            const auto paragraphId = paragraph.id();
            const auto markBefore = paragraph.paragraphMarkCharacterFormat();
            if (std::find(
                    paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                        normalized.value().start.utf16_offset),
                    paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                        normalized.value().end.utf16_offset),
                    core::kInlineObjectReplacementCharacter) !=
                paragraph.text().begin() + static_cast<std::ptrdiff_t>(
                    normalized.value().end.utf16_offset)) {
                return true;
            }
            if (!working.deleteRange(
                    deletion->range, deletion->empty_paragraph_format)) {
                return true;
            }
            const auto* changedParagraph = working.findParagraph(paragraphId);
            if (!changedParagraph ||
                changedParagraph->paragraphMarkCharacterFormat() != markBefore) {
                return true;
            }
            continue;
        }
        return true;
    }
    return false;
}

struct PlainTextListMarker {
    enum class Kind { bullet, numbered };

    Kind kind{Kind::bullet};
    QString indent;
    QString marker;
    QString separator;
    qsizetype prefixLength{};
    bool emptyItem{false};
};

std::optional<PlainTextListMarker> plainTextListMarker(const QString& text) {
    static const QRegularExpression bulletPattern(
        QStringLiteral(R"(^([ \t]*)([\x{2022}\x{25E6}\x{25AA}\x{2023}*-])([ \t]+))"));
    static const QRegularExpression numberedPattern(QStringLiteral(
        R"(^([ \t]*)([0-9]+|[A-Z]{1,3}|[a-z]{1,3}|[IVXLCDM]{4,15}|[ivxlcdm]{4,15})([.)])([ \t]+))"));

    auto match = bulletPattern.match(text);
    PlainTextListMarker result;
    if (match.hasMatch()) {
        result.kind = PlainTextListMarker::Kind::bullet;
        result.indent = match.captured(1);
        result.marker = match.captured(2);
        result.separator = match.captured(3);
    } else {
        match = numberedPattern.match(text);
        if (!match.hasMatch()) {
            return std::nullopt;
        }
        result.kind = PlainTextListMarker::Kind::numbered;
        result.indent = match.captured(1);
        result.marker = match.captured(2) + match.captured(3);
        result.separator = match.captured(4);
        if (match.captured(2).front().isLetter() && result.indent.isEmpty() &&
            !result.separator.contains(QLatin1Char('\t'))) {
            return std::nullopt;
        }
    }

    result.prefixLength = match.capturedLength(0);
    result.emptyItem = text.mid(result.prefixLength).trimmed().isEmpty();
    return result;
}

int indentationColumns(const QString& indent, int tabWidthSpaces = 4) {
    tabWidthSpaces = std::max(1, tabWidthSpaces);
    int columns = 0;
    for (const QChar character : indent) {
        if (character == QLatin1Char('\t')) {
            columns += tabWidthSpaces - (columns % tabWidthSpaces);
        } else {
            ++columns;
        }
    }
    return columns;
}

QString bulletForLevel(std::size_t level) {
    static const QStringList cycle{
        QStringLiteral("\u2022"), QStringLiteral("\u25e6"), QStringLiteral("\u25aa")};
    return cycle.at(static_cast<qsizetype>(level %
                                          static_cast<std::size_t>(cycle.size())));
}

enum class NumberingStyle {
    decimal,
    upperAlphabetic,
    upperRoman,
    lowerAlphabetic,
    lowerRoman,
};

NumberingStyle numberingStyleForLevel(std::size_t level) {
    switch (level % 5U) {
        case 0: return NumberingStyle::decimal;
        case 1: return NumberingStyle::upperAlphabetic;
        case 2: return NumberingStyle::upperRoman;
        case 3: return NumberingStyle::lowerAlphabetic;
        default: return NumberingStyle::lowerRoman;
    }
}

std::optional<qulonglong> decimalOrdinal(const QString& text) {
    bool valid = false;
    const qulonglong value = text.toULongLong(&valid);
    return valid && value > 0 ? std::optional<qulonglong>(value) : std::nullopt;
}

std::optional<qulonglong> alphabeticOrdinal(const QString& text,
                                            bool uppercase) {
    if (text.isEmpty()) return std::nullopt;
    qulonglong result = 0;
    constexpr qulonglong radix = 26;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        const ushort first = uppercase ? static_cast<ushort>('A')
                                       : static_cast<ushort>('a');
        const ushort last = uppercase ? static_cast<ushort>('Z')
                                      : static_cast<ushort>('z');
        if (code < first || code > last) return std::nullopt;
        const qulonglong digit = static_cast<qulonglong>(code - first) + 1U;
        if (result > (std::numeric_limits<qulonglong>::max() - digit) / radix) {
            return std::nullopt;
        }
        result = result * radix + digit;
    }
    return result;
}

QString alphabeticMarker(qulonglong ordinal, bool uppercase) {
    // Three letters already cover 18,278 items at a single level. Beyond
    // that, use decimal digits so a pathological list cannot create an
    // unbounded marker and the value remains lossless.
    if (ordinal == 0 || ordinal > 18'278U) return QString::number(ordinal);
    QString result;
    const ushort first = uppercase ? static_cast<ushort>('A')
                                   : static_cast<ushort>('a');
    while (ordinal > 0) {
        --ordinal;
        result.prepend(QChar(static_cast<ushort>(
            first + static_cast<ushort>(ordinal % 26U))));
        ordinal /= 26U;
    }
    return result;
}

QString romanMarker(qulonglong ordinal, bool uppercase) {
    // Conventional Roman numerals have no portable representation above
    // 3,999. Decimal is an explicit, parseable fallback at that boundary.
    if (ordinal == 0 || ordinal > 3'999U) return QString::number(ordinal);
    struct RomanPart {
        qulonglong value;
        const char* text;
    };
    static constexpr RomanPart parts[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
        {100, "C"},  {90, "XC"},  {50, "L"},  {40, "XL"},
        {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"},
        {1, "I"},
    };
    QString result;
    for (const auto& part : parts) {
        while (ordinal >= part.value) {
            result += QLatin1String(part.text);
            ordinal -= part.value;
        }
    }
    return uppercase ? result : result.toLower();
}

std::optional<qulonglong> romanOrdinal(const QString& text, bool uppercase) {
    if (text.isEmpty() || text.size() > 15) return std::nullopt;
    const QString expectedCase = uppercase ? text.toUpper() : text.toLower();
    if (text != expectedCase) return std::nullopt;
    const QString upper = text.toUpper();
    const auto valueOf = [](QChar character) -> unsigned {
        switch (character.unicode()) {
            case 'I': return 1;
            case 'V': return 5;
            case 'X': return 10;
            case 'L': return 50;
            case 'C': return 100;
            case 'D': return 500;
            case 'M': return 1000;
            default: return 0;
        }
    };
    qulonglong value = 0;
    for (qsizetype index = 0; index < upper.size(); ++index) {
        const unsigned current = valueOf(upper.at(index));
        if (current == 0) return std::nullopt;
        const unsigned next = index + 1 < upper.size()
            ? valueOf(upper.at(index + 1))
            : 0;
        if (current < next) {
            value += static_cast<qulonglong>(next - current);
            ++index;
        } else {
            value += current;
        }
    }
    if (value == 0 || value > 3'999U || romanMarker(value, true) != upper) {
        return std::nullopt;
    }
    return value;
}

std::optional<qulonglong> numberedOrdinal(const QString& marker,
                                          std::size_t level) {
    if (marker.size() < 2) return std::nullopt;
    const QChar delimiter = marker.back();
    if (delimiter != QLatin1Char('.') && delimiter != QLatin1Char(')')) {
        return std::nullopt;
    }
    const QString body = marker.first(marker.size() - 1);
    std::optional<qulonglong> result;
    switch (numberingStyleForLevel(level)) {
        case NumberingStyle::decimal:
            result = decimalOrdinal(body);
            break;
        case NumberingStyle::upperAlphabetic:
            result = alphabeticOrdinal(body, true);
            break;
        case NumberingStyle::upperRoman:
            result = romanOrdinal(body, true);
            break;
        case NumberingStyle::lowerAlphabetic:
            result = alphabeticOrdinal(body, false);
            break;
        case NumberingStyle::lowerRoman:
            result = romanOrdinal(body, false);
            break;
    }
    if (result) return result;

    // Gracefully migrate lists made by earlier Owl Docs builds, which used
    // decimal markers at every level. At level one, also recognize a manually
    // typed alphabetic/Roman marker. Do not reinterpret malformed Roman text
    // (for example "IIII") as a huge alphabetic ordinal.
    if (auto decimal = decimalOrdinal(body)) return decimal;
    if (numberingStyleForLevel(level) != NumberingStyle::decimal) {
        return std::nullopt;
    }
    if (auto roman = romanOrdinal(body, body == body.toUpper())) return roman;
    return alphabeticOrdinal(body, body == body.toUpper());
}

QString numberedMarker(qulonglong ordinal, std::size_t level,
                       QChar delimiter = QLatin1Char('.')) {
    QString body;
    switch (numberingStyleForLevel(level)) {
        case NumberingStyle::decimal:
            body = QString::number(ordinal);
            break;
        case NumberingStyle::upperAlphabetic:
            body = alphabeticMarker(ordinal, true);
            break;
        case NumberingStyle::upperRoman:
            body = romanMarker(ordinal, true);
            break;
        case NumberingStyle::lowerAlphabetic:
            body = alphabeticMarker(ordinal, false);
            break;
        case NumberingStyle::lowerRoman:
            body = romanMarker(ordinal, false);
            break;
    }
    return body + delimiter;
}

QString normalizedNumberedMarker(const QString& marker, std::size_t level) {
    const auto ordinal = numberedOrdinal(marker, level);
    return ordinal ? numberedMarker(*ordinal, level, marker.back()) : marker;
}

std::size_t listLevelForMarker(const PlainTextListMarker& marker,
                               int tabWidthSpaces) {
    return std::min<std::size_t>(
        core::kListLevelCount - 1,
        static_cast<std::size_t>(std::max(
            0, indentationColumns(marker.indent, tabWidthSpaces) /
                   std::max(1, tabWidthSpaces))));
}

QString listPrefix(PlainTextListMarker::Kind kind, const QString& marker,
                   std::size_t level, const core::ListLayout& layout) {
    const auto safeLevel = std::min(level, core::kListLevelCount - 1);
    const int bulletIndent = layout.levels[safeLevel].bullet_indent_spaces;
    const bool standardBullet = marker == QStringLiteral("\u2022") ||
                                marker == QStringLiteral("\u25e6") ||
                                marker == QStringLiteral("\u25aa");
    QString displayedMarker = marker;
    if (kind == PlainTextListMarker::Kind::bullet && standardBullet) {
        displayedMarker = bulletForLevel(safeLevel);
    } else if (kind == PlainTextListMarker::Kind::numbered) {
        displayedMarker = normalizedNumberedMarker(marker, safeLevel);
    }
    return QString(std::max(0, bulletIndent), QLatin1Char(' ')) +
           displayedMarker + QLatin1Char('\t');
}

core::ParagraphFormatDelta semanticListDelta(
    core::NodeId listId, std::size_t level,
    const core::ListLayout& layout = core::ListLayout{}) {
    core::ParagraphFormatDelta delta;
    delta.list_id = core::PropertyDelta<core::NodeId>::set(listId);
    delta.list_level = core::PropertyDelta<std::uint8_t>::set(
        static_cast<std::uint8_t>(std::min(level, core::kListLevelCount - 1)));
    delta.list_layout = core::PropertyDelta<core::ListLayout>::set(layout);
    return delta;
}

core::ParagraphFormatDelta clearSemanticListDelta() {
    core::ParagraphFormatDelta delta;
    delta.list_id = core::PropertyDelta<core::NodeId>::clear();
    delta.list_level = core::PropertyDelta<std::uint8_t>::clear();
    delta.list_layout = core::PropertyDelta<core::ListLayout>::clear();
    return delta;
}

void adoptPlainTextLists(core::Document& document, int tabWidthSpaces) {
    std::optional<PlainTextListMarker::Kind> previousKind;
    core::NodeId currentListId{};
    core::ListLayout currentLayout;
    for (const auto& paragraph : document.paragraphs()) {
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (!marker) {
            previousKind.reset();
            currentListId = {};
            continue;
        }

        const auto& existing = paragraph.format();
        if (existing.list_id && existing.list_level && existing.list_layout) {
            currentListId = *existing.list_id;
            currentLayout = *existing.list_layout;
            previousKind = marker->kind;
            continue;
        }
        if (!previousKind || *previousKind != marker->kind ||
            !currentListId.isValid()) {
            currentListId = core::NodeId::generate();
            currentLayout = core::ListLayout{};
        }
        const auto applied = document.applyParagraphFormat(
            {paragraph.id()}, semanticListDelta(
                                 currentListId,
                                 listLevelForMarker(*marker, tabWidthSpaces),
                                 currentLayout));
        if (!applied) {
            previousKind.reset();
            currentListId = {};
            continue;
        }
        previousKind = marker->kind;
    }
}

qsizetype previousWordStart(const QString& text, qsizetype position) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    finder.setPosition(std::clamp<qsizetype>(position, 0, text.size()));
    while (true) {
        const qsizetype boundary = finder.toPreviousBoundary();
        if (boundary < 0) {
            return 0;
        }
        if (finder.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem)) {
            return boundary;
        }
    }
}

qsizetype nextWordStart(const QString& text, qsizetype position) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    finder.setPosition(std::clamp<qsizetype>(position, 0, text.size()));
    while (true) {
        const qsizetype boundary = finder.toNextBoundary();
        if (boundary < 0) {
            return text.size();
        }
        if (finder.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem)) {
            return boundary;
        }
    }
}

bool containsPrintableKeyText(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }
    return std::none_of(text.cbegin(), text.cend(), [](QChar character) {
        const auto value = character.unicode();
        return value < 0x20U || value == 0x7fU;
    });
}

bool isAltGrTextInput(const QKeyEvent& event) {
    if (!containsPrintableKeyText(event.text()) ||
        (event.modifiers() & Qt::MetaModifier)) {
        return false;
    }
    if (event.modifiers() & Qt::GroupSwitchModifier) {
        return true;
    }
    if ((event.modifiers() & (Qt::ControlModifier | Qt::AltModifier)) !=
        (Qt::ControlModifier | Qt::AltModifier)) {
        return false;
    }

    // XKB configurations may expose AltGr as Ctrl+Alt. Preserve actual
    // Ctrl+Alt shortcuts when their text is simply the unmodified key, while
    // accepting the alternate symbol produced by a third-level layout.
    if (event.text().size() != 1 || event.key() < 0x20 || event.key() > 0x7e) {
        return true;
    }
    return event.text().compare(
               QString(QChar(static_cast<ushort>(event.key()))),
                                Qt::CaseInsensitive) != 0;
}

std::optional<QString> continuationMarker(const PlainTextListMarker& marker,
                                          std::size_t level) {
    if (marker.kind == PlainTextListMarker::Kind::bullet) {
        return marker.marker;
    }

    const auto ordinal = numberedOrdinal(marker.marker, level);
    if (!ordinal || *ordinal == std::numeric_limits<qulonglong>::max()) {
        return std::nullopt;
    }
    return numberedMarker(*ordinal + 1U, level, marker.marker.back());
}

std::optional<core::CharacterFormat> insertionFormatFor(
    const core::Document& source,
    const core::NormalizedRange& replacement,
    const core::CharacterFormat& desired) {
    // A null operation format deliberately means "inherit at the insertion
    // point". Prefer it whenever it produces the same semantic formatting:
    // imported text can then remain a safe text-only OOXML patch instead of
    // being mislabeled as a formatting rewrite merely because its inherited
    // run happens to be colored, bold, or otherwise directly formatted. An
    // explicitly empty desired format is still meaningful when it clears the
    // adjacent run's only property, so compare before deciding to inherit.
    return inheritedFormatAfterReplacement(source, replacement) == desired
        ? std::nullopt
        : std::optional<core::CharacterFormat>(desired);
}

template <typename T>
void setStyleTransition(core::PropertyDelta<T>& delta,
                        const std::optional<T>& current,
                        const std::optional<T>& previous,
                        const std::optional<T>& next,
                        bool replaceUnknownBaseline,
                        bool authoritativeSourceBaseline,
                        bool directOverride) {
    // Source provenance is authoritative when present: explicit overrides
    // survive even when their value happens to equal the inherited baseline.
    // Authored/legacy paragraphs without provenance retain the conservative
    // value-comparison fallback used by earlier recovery snapshots.
    if (directOverride) return;
    if (!authoritativeSourceBaseline && !replaceUnknownBaseline && current &&
        current != previous) {
        return;
    }
    if (!replaceUnknownBaseline && previous == next && current == next) {
        return;
    }
    if (next) {
        if (current != next) delta = core::PropertyDelta<T>::set(*next);
    } else if (current) {
        delta = core::PropertyDelta<T>::clear();
    }
}

template <typename T>
bool stylePropertyIsDirect(const std::optional<T>& current,
                           const std::optional<T>& previous,
                           bool replaceUnknownBaseline,
                           bool authoritativeSourceBaseline,
                           bool sourceDirectOverride) {
    if (authoritativeSourceBaseline) return sourceDirectOverride;
    return !replaceUnknownBaseline && current && current != previous;
}

core::CharacterFormatMask styleCharacterOverrides(
    const core::CharacterFormat& current,
    const core::CharacterFormat& previous,
    bool replaceUnknownBaseline,
    bool authoritativeSourceBaseline,
    const core::CharacterFormatMask& sourceOverrides) {
    return {
        stylePropertyIsDirect(
            current.font_family, previous.font_family,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.font_family),
        stylePropertyIsDirect(
            current.font_size_half_points, previous.font_size_half_points,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.font_size_half_points),
        stylePropertyIsDirect(
            current.bold, previous.bold, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.bold),
        stylePropertyIsDirect(
            current.italic, previous.italic, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.italic),
        stylePropertyIsDirect(
            current.underline, previous.underline, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.underline),
        stylePropertyIsDirect(
            current.strike, previous.strike, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.strike),
        stylePropertyIsDirect(
            current.foreground_argb, previous.foreground_argb,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.foreground_argb),
        stylePropertyIsDirect(
            current.highlight_argb, previous.highlight_argb,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.highlight_argb),
        stylePropertyIsDirect(
            current.baseline, previous.baseline, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.baseline),
        stylePropertyIsDirect(
            current.language, previous.language, replaceUnknownBaseline,
            authoritativeSourceBaseline, sourceOverrides.language),
    };
}

void mergeCharacterFormatMask(
    core::CharacterFormatMask& destination,
    const core::CharacterFormatMask& source) noexcept {
    destination.font_family =
        destination.font_family || source.font_family;
    destination.font_size_half_points =
        destination.font_size_half_points || source.font_size_half_points;
    destination.bold = destination.bold || source.bold;
    destination.italic = destination.italic || source.italic;
    destination.underline = destination.underline || source.underline;
    destination.strike = destination.strike || source.strike;
    destination.foreground_argb =
        destination.foreground_argb || source.foreground_argb;
    destination.highlight_argb =
        destination.highlight_argb || source.highlight_argb;
    destination.baseline = destination.baseline || source.baseline;
    destination.language = destination.language || source.language;
}

template <typename T>
void setMaskedProperty(core::PropertyDelta<T>& delta,
                       const std::optional<T>& value, bool masked) {
    if (!masked) return;
    delta = value ? core::PropertyDelta<T>::set(*value)
                  : core::PropertyDelta<T>::clear();
}

core::CharacterFormatDelta maskedCharacterFormatDelta(
    const core::CharacterFormat& format,
    const core::CharacterFormatMask& mask) {
    core::CharacterFormatDelta delta;
    setMaskedProperty(delta.font_family, format.font_family,
                      mask.font_family);
    setMaskedProperty(delta.font_size_half_points,
                      format.font_size_half_points,
                      mask.font_size_half_points);
    setMaskedProperty(delta.bold, format.bold, mask.bold);
    setMaskedProperty(delta.italic, format.italic, mask.italic);
    setMaskedProperty(delta.underline, format.underline, mask.underline);
    setMaskedProperty(delta.strike, format.strike, mask.strike);
    setMaskedProperty(delta.foreground_argb, format.foreground_argb,
                      mask.foreground_argb);
    setMaskedProperty(delta.highlight_argb, format.highlight_argb,
                      mask.highlight_argb);
    setMaskedProperty(delta.baseline, format.baseline, mask.baseline);
    setMaskedProperty(delta.language, format.language, mask.language);
    return delta;
}

core::ParagraphFormatMask styleParagraphOverrides(
    const core::ParagraphFormat& current,
    const core::ParagraphFormat& previous,
    bool replaceUnknownBaseline,
    bool authoritativeSourceBaseline,
    const core::ParagraphFormatMask& sourceOverrides) {
    return {
        stylePropertyIsDirect(
            current.alignment, previous.alignment,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.alignment),
        stylePropertyIsDirect(
            current.left_indent_emu, previous.left_indent_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.left_indent_emu),
        stylePropertyIsDirect(
            current.right_indent_emu, previous.right_indent_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.right_indent_emu),
        stylePropertyIsDirect(
            current.first_line_indent_emu, previous.first_line_indent_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.first_line_indent_emu),
        stylePropertyIsDirect(
            current.space_before_emu, previous.space_before_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.space_before_emu),
        stylePropertyIsDirect(
            current.space_after_emu, previous.space_after_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.space_after_emu),
        stylePropertyIsDirect(
            current.line_spacing_emu, previous.line_spacing_emu,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.line_spacing_emu),
        stylePropertyIsDirect(
            current.line_spacing_rule, previous.line_spacing_rule,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.line_spacing_rule),
        stylePropertyIsDirect(
            current.keep_with_next, previous.keep_with_next,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.keep_with_next),
        stylePropertyIsDirect(
            current.keep_lines, previous.keep_lines,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.keep_lines),
        stylePropertyIsDirect(
            current.page_break_before, previous.page_break_before,
            replaceUnknownBaseline, authoritativeSourceBaseline,
            sourceOverrides.page_break_before),
    };
}

core::CharacterFormat effectiveStyleCharacterFormat(
    const core::ParagraphStyleDefinition& style,
    const QString& defaultFontFamily, double defaultFontPointSize) {
    auto result = style.character_format;
    const bool followsEditorDefaults =
        style.id == "Normal" || style.id == "NoSpacing";
    if (followsEditorDefaults || !result.font_family) {
        result.font_family = defaultFontFamily.toStdString();
    }
    if (followsEditorDefaults || !result.font_size_half_points) {
        result.font_size_half_points = static_cast<std::int32_t>(
            std::lround(defaultFontPointSize * 2.0));
    }
    return result;
}

template <typename T>
bool propertyDeltaRequiresMutation(
    const core::PropertyDelta<T>& delta, const std::optional<T>& current,
    const std::optional<T>& inherited, bool alreadyDirect,
    bool directnessIsTrackable, bool directnessCanBeInferredByValue) {
    if (delta.action == core::DeltaAction::unchanged) return false;
    auto candidate = current;
    delta.applyTo(candidate);
    if (candidate != current) return true;
    if (!directnessIsTrackable || alreadyDirect) return false;
    // Legacy/authored paragraphs without provenance can safely infer a
    // present value that differs from the known built-in baseline as direct.
    // Materializing provenance for that case would turn a visibly and
    // semantically redundant toolbar choice into a dirty document.
    return !(directnessCanBeInferredByValue && current &&
             current != inherited);
}

bool characterDeltaRequiresMutation(
    const core::Paragraph& paragraph, std::size_t utf16Offset,
    bool paragraphMark, const core::CharacterFormatDelta& delta,
    const QString& defaultFontFamily, double defaultFontPointSize) {
    const auto& provenance = paragraph.styleProvenance();
    const auto* definition = core::findBuiltInParagraphStyle(
        paragraph.styleId().value_or("Normal"));
    const bool trackable = provenance.has_value() || definition != nullptr;
    const bool inferByValue = !provenance && definition != nullptr;
    const auto inherited = provenance
        ? (paragraphMark
               ? provenance->inherited_paragraph_mark_character_format
               : provenance->inherited_character_format)
        : definition
        ? effectiveStyleCharacterFormat(
              *definition, defaultFontFamily, defaultFontPointSize)
        : core::CharacterFormat{};
    const auto current = paragraphMark
        ? paragraph.paragraphMarkCharacterFormat()
        : paragraph.characterFormatAt(utf16Offset);
    const auto mask = paragraphMark && provenance
        ? provenance->paragraph_mark_overrides
        : paragraph.styleOverrideMaskAt(utf16Offset);

    return propertyDeltaRequiresMutation(
               delta.font_family, current.font_family,
               inherited.font_family, mask.font_family, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.font_size_half_points,
               current.font_size_half_points,
               inherited.font_size_half_points,
               mask.font_size_half_points, trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.bold, current.bold, inherited.bold, mask.bold,
               trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.italic, current.italic, inherited.italic, mask.italic,
               trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.underline, current.underline, inherited.underline,
               mask.underline, trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.strike, current.strike, inherited.strike, mask.strike,
               trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.foreground_argb, current.foreground_argb,
               inherited.foreground_argb, mask.foreground_argb, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.highlight_argb, current.highlight_argb,
               inherited.highlight_argb, mask.highlight_argb, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.baseline, current.baseline, inherited.baseline,
               mask.baseline, trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.language, current.language, inherited.language,
               mask.language, trackable, inferByValue);
}

bool paragraphDeltaRequiresMutation(
    const core::Paragraph& paragraph,
    const core::ParagraphFormatDelta& delta) {
    const auto& provenance = paragraph.styleProvenance();
    const auto* definition = core::findBuiltInParagraphStyle(
        paragraph.styleId().value_or("Normal"));
    const bool trackable = provenance.has_value() || definition != nullptr;
    const bool inferByValue = !provenance && definition != nullptr;
    const auto inherited = provenance
        ? provenance->inherited_paragraph_format
        : definition ? definition->paragraph_format
                     : core::ParagraphFormat{};
    const auto& current = paragraph.format();
    const auto mask = provenance
        ? provenance->paragraph_overrides
        : core::ParagraphFormatMask{};

    return propertyDeltaRequiresMutation(
               delta.alignment, current.alignment, inherited.alignment,
               mask.alignment, trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.left_indent_emu, current.left_indent_emu,
               inherited.left_indent_emu, mask.left_indent_emu, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.right_indent_emu, current.right_indent_emu,
               inherited.right_indent_emu, mask.right_indent_emu, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.first_line_indent_emu,
               current.first_line_indent_emu,
               inherited.first_line_indent_emu,
               mask.first_line_indent_emu, trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.space_before_emu, current.space_before_emu,
               inherited.space_before_emu, mask.space_before_emu, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.space_after_emu, current.space_after_emu,
               inherited.space_after_emu, mask.space_after_emu, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.line_spacing_emu, current.line_spacing_emu,
               inherited.line_spacing_emu, mask.line_spacing_emu, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.line_spacing_rule, current.line_spacing_rule,
               inherited.line_spacing_rule, mask.line_spacing_rule,
               trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.keep_with_next, current.keep_with_next,
               inherited.keep_with_next, mask.keep_with_next, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.keep_lines, current.keep_lines,
               inherited.keep_lines, mask.keep_lines, trackable,
               inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.page_break_before, current.page_break_before,
               inherited.page_break_before, mask.page_break_before,
               trackable, inferByValue) ||
           propertyDeltaRequiresMutation(
               delta.list_id, current.list_id, std::optional<core::NodeId>{},
               false, false, false) ||
           propertyDeltaRequiresMutation(
               delta.list_level, current.list_level,
               std::optional<std::uint8_t>{}, false, false, false) ||
           propertyDeltaRequiresMutation(
               delta.list_layout, current.list_layout,
               std::optional<core::ListLayout>{}, false, false, false);
}

core::CharacterFormatDelta styleCharacterTransition(
    const core::CharacterFormat& current,
    const core::CharacterFormat& previous,
    const core::CharacterFormat& next,
    bool replaceUnknownBaseline,
    bool authoritativeSourceBaseline = false,
    const core::CharacterFormatMask& directOverrides = {}) {
    core::CharacterFormatDelta delta;
    setStyleTransition(delta.font_family, current.font_family,
                       previous.font_family, next.font_family,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.font_family);
    setStyleTransition(delta.font_size_half_points,
                       current.font_size_half_points,
                       previous.font_size_half_points,
                       next.font_size_half_points,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.font_size_half_points);
    setStyleTransition(delta.bold, current.bold, previous.bold, next.bold,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.bold);
    setStyleTransition(delta.italic, current.italic, previous.italic,
                       next.italic, replaceUnknownBaseline,
                       authoritativeSourceBaseline, directOverrides.italic);
    setStyleTransition(delta.underline, current.underline,
                       previous.underline, next.underline,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.underline);
    setStyleTransition(delta.strike, current.strike, previous.strike,
                       next.strike, replaceUnknownBaseline,
                       authoritativeSourceBaseline, directOverrides.strike);
    setStyleTransition(delta.foreground_argb, current.foreground_argb,
                       previous.foreground_argb, next.foreground_argb,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.foreground_argb);
    setStyleTransition(delta.highlight_argb, current.highlight_argb,
                       previous.highlight_argb, next.highlight_argb,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.highlight_argb);
    setStyleTransition(delta.baseline, current.baseline,
                       previous.baseline, next.baseline,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.baseline);
    setStyleTransition(delta.language, current.language, previous.language,
                       next.language, replaceUnknownBaseline,
                       authoritativeSourceBaseline, directOverrides.language);
    return delta;
}

core::ParagraphFormatDelta styleParagraphTransition(
    const core::ParagraphFormat& current,
    const core::ParagraphFormat& previous,
    const core::ParagraphFormat& next,
    bool replaceUnknownBaseline,
    bool authoritativeSourceBaseline = false,
    const core::ParagraphFormatMask& directOverrides = {}) {
    core::ParagraphFormatDelta delta;
    setStyleTransition(delta.alignment, current.alignment,
                       previous.alignment, next.alignment,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.alignment);
    setStyleTransition(delta.left_indent_emu, current.left_indent_emu,
                       previous.left_indent_emu, next.left_indent_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.left_indent_emu);
    setStyleTransition(delta.right_indent_emu, current.right_indent_emu,
                       previous.right_indent_emu, next.right_indent_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.right_indent_emu);
    setStyleTransition(delta.first_line_indent_emu,
                       current.first_line_indent_emu,
                       previous.first_line_indent_emu,
                       next.first_line_indent_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.first_line_indent_emu);
    setStyleTransition(delta.space_before_emu, current.space_before_emu,
                       previous.space_before_emu, next.space_before_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.space_before_emu);
    setStyleTransition(delta.space_after_emu, current.space_after_emu,
                       previous.space_after_emu, next.space_after_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.space_after_emu);
    setStyleTransition(delta.line_spacing_emu, current.line_spacing_emu,
                       previous.line_spacing_emu, next.line_spacing_emu,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.line_spacing_emu);
    setStyleTransition(delta.line_spacing_rule, current.line_spacing_rule,
                       previous.line_spacing_rule, next.line_spacing_rule,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.line_spacing_rule);
    setStyleTransition(delta.keep_with_next, current.keep_with_next,
                       previous.keep_with_next, next.keep_with_next,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.keep_with_next);
    setStyleTransition(delta.keep_lines, current.keep_lines,
                       previous.keep_lines, next.keep_lines,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.keep_lines);
    setStyleTransition(delta.page_break_before, current.page_break_before,
                       previous.page_break_before, next.page_break_before,
                       replaceUnknownBaseline, authoritativeSourceBaseline,
                       directOverrides.page_break_before);
    // List identity and geometry are intentionally outside paragraph styles.
    return delta;
}

struct ParagraphStyleApplicationPlan {
    std::vector<core::Operation> operations;
    std::optional<core::CharacterFormat> resultingTypingFormat;
    std::optional<core::CharacterFormatMask> resultingTypingOverrideMask;
};

ParagraphStyleApplicationPlan planParagraphStyleApplication(
    const core::Document& document,
    std::span<const core::NodeId> paragraphIds,
    const core::ParagraphStyleDefinition& target,
    const QString& defaultFontFamily, double defaultFontPointSize,
    const core::CharacterFormat& typingFormat,
    std::optional<core::Position> typingPosition,
    bool ensureTargetProvenance = false,
    const core::CharacterFormatMask& typingAdditionalOverrides = {}) {
    ParagraphStyleApplicationPlan plan;
    std::vector<core::NodeId> changedIds;
    const std::string targetId(target.id);
    const auto targetCharacter = effectiveStyleCharacterFormat(
        target, defaultFontFamily, defaultFontPointSize);

    for (const auto id : paragraphIds) {
        const auto* paragraph = document.findParagraph(id);
        if (!paragraph) continue;
        const std::string previousId =
            paragraph->styleId().value_or("Normal");
        const auto* previous = core::findBuiltInParagraphStyle(previousId);
        const auto& provenance = paragraph->styleProvenance();
        if (previousId == targetId &&
            (!ensureTargetProvenance || provenance)) {
            continue;
        }
        const bool authoritativeSourceBaseline = provenance.has_value();
        const bool replaceUnknownBaseline =
            previous == nullptr && !authoritativeSourceBaseline;
        const auto previousCharacter = provenance
            ? provenance->inherited_character_format
            : previous
            ? effectiveStyleCharacterFormat(
                  *previous, defaultFontFamily, defaultFontPointSize)
            : core::CharacterFormat{};
        const auto previousParagraph = provenance
            ? provenance->inherited_paragraph_format
            : previous
            ? previous->paragraph_format
            : core::ParagraphFormat{};
        const auto previousParagraphMarkCharacter = provenance
            ? provenance->inherited_paragraph_mark_character_format
            : previousCharacter;
        // The current built-in catalog has no distinct style pPr/rPr
        // contribution, so its paragraph-mark baseline is the resolved style
        // character baseline.  Keep it distinct in provenance for imported
        // styles where those two baselines can differ.
        const auto targetParagraphMarkCharacter = targetCharacter;
        if (paragraph->styleId() != targetId) {
            changedIds.push_back(id);
        }
        core::ParagraphStyleProvenance targetProvenance;
        targetProvenance.inherited_character_format = targetCharacter;
        targetProvenance.inherited_paragraph_mark_character_format =
            targetParagraphMarkCharacter;
        targetProvenance.inherited_paragraph_format =
            target.paragraph_format;

        std::vector<std::size_t> boundaries{0, paragraph->text().size()};
        for (const auto& run : paragraph->characterFormats()) {
            boundaries.push_back(run.start);
            boundaries.push_back(run.end);
        }
        if (provenance) {
            for (const auto& run : provenance->character_overrides) {
                boundaries.push_back(run.start);
                boundaries.push_back(run.end);
            }
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                         boundaries.end());
        for (std::size_t boundary = 1; boundary < boundaries.size();
             ++boundary) {
            const std::size_t start = boundaries[boundary - 1];
            const std::size_t end = boundaries[boundary];
            if (start >= end) continue;
            const auto current = paragraph->characterFormatAt(start + 1);
            const auto sourceOverrides = paragraph->styleOverrideMaskAt(
                start + 1);
            const auto targetOverrides = styleCharacterOverrides(
                current, previousCharacter, replaceUnknownBaseline,
                authoritativeSourceBaseline, sourceOverrides);
            if (!targetOverrides.empty()) {
                auto& runs = targetProvenance.character_overrides;
                if (!runs.empty() && runs.back().end == start &&
                    runs.back().properties == targetOverrides) {
                    runs.back().end = end;
                } else {
                    runs.push_back({start, end, targetOverrides});
                }
            }
            const auto delta = styleCharacterTransition(
                current, previousCharacter, targetCharacter,
                replaceUnknownBaseline, authoritativeSourceBaseline,
                sourceOverrides);
            if (!delta.empty()) {
                plan.operations.emplace_back(core::SetCharacterFormat{
                    {{id, start}, {id, end}}, delta});
            }
        }
        const auto markDelta = styleCharacterTransition(
            paragraph->paragraphMarkCharacterFormat(),
            previousParagraphMarkCharacter,
            targetParagraphMarkCharacter, replaceUnknownBaseline,
            authoritativeSourceBaseline,
            provenance ? provenance->paragraph_mark_overrides
                       : core::CharacterFormatMask{});
        targetProvenance.paragraph_mark_overrides =
            styleCharacterOverrides(
                paragraph->paragraphMarkCharacterFormat(),
                previousParagraphMarkCharacter, replaceUnknownBaseline,
                authoritativeSourceBaseline,
                provenance ? provenance->paragraph_mark_overrides
                           : core::CharacterFormatMask{});
        if (!markDelta.empty()) {
            plan.operations.emplace_back(
                core::SetParagraphMarkCharacterFormat{id, markDelta});
        }
        const auto paragraphDelta = styleParagraphTransition(
            paragraph->format(), previousParagraph,
            target.paragraph_format, replaceUnknownBaseline,
            authoritativeSourceBaseline,
            provenance ? provenance->paragraph_overrides
                       : core::ParagraphFormatMask{});
        targetProvenance.paragraph_overrides = styleParagraphOverrides(
            paragraph->format(), previousParagraph, replaceUnknownBaseline,
            authoritativeSourceBaseline,
            provenance ? provenance->paragraph_overrides
                       : core::ParagraphFormatMask{});
        if (!paragraphDelta.empty()) {
            plan.operations.emplace_back(
                core::SetParagraphFormat{{id}, paragraphDelta});
        }

        if (typingPosition && id == typingPosition->paragraph_id) {
            auto resulting = typingFormat;
            auto typingOverrides =
                paragraph->styleOverrideMaskAt(typingPosition->utf16_offset);
            mergeCharacterFormatMask(
                typingOverrides, typingAdditionalOverrides);
            const auto& previousTypingBaseline = paragraph->text().empty()
                ? previousParagraphMarkCharacter
                : previousCharacter;
            const auto& targetTypingBaseline = paragraph->text().empty()
                ? targetParagraphMarkCharacter
                : targetCharacter;
            const auto typingDelta = styleCharacterTransition(
                resulting, previousTypingBaseline, targetTypingBaseline,
                replaceUnknownBaseline, authoritativeSourceBaseline,
                typingOverrides);
            auto resultingOverrides = styleCharacterOverrides(
                resulting, previousTypingBaseline, replaceUnknownBaseline,
                authoritativeSourceBaseline, typingOverrides);
            // Without source provenance, equality alone cannot distinguish
            // an inherited value from a transient explicit choice at the
            // caret. The editor state carries that missing intent.
            mergeCharacterFormatMask(
                resultingOverrides, typingAdditionalOverrides);
            typingDelta.applyTo(resulting);
            plan.resultingTypingFormat = std::move(resulting);
            plan.resultingTypingOverrideMask =
                std::move(resultingOverrides);
        }
        plan.operations.emplace_back(core::SetParagraphStyleProvenance{
            id, std::move(targetProvenance)});
    }
    if (!changedIds.empty()) {
        // Identity precedes effective-format changes so every observer of the
        // completed batch sees semantic intent and appearance together.
        plan.operations.insert(
            plan.operations.begin(),
            core::SetParagraphStyle{std::move(changedIds), targetId});
    }
    return plan;
}

ParagraphStyleApplicationPlan planBuiltInStyleProvenanceInitialization(
    const core::Document& document,
    std::span<const core::NodeId> paragraphIds,
    const QString& defaultFontFamily, double defaultFontPointSize,
    const core::CharacterFormat& typingFormat,
    std::optional<core::Position> typingPosition,
    const core::CharacterFormatMask& typingAdditionalOverrides = {}) {
    ParagraphStyleApplicationPlan combined;
    std::vector<core::NodeId> visited;
    visited.reserve(paragraphIds.size());
    for (const auto id : paragraphIds) {
        if (std::find(visited.begin(), visited.end(), id) != visited.end()) {
            continue;
        }
        visited.push_back(id);
        const auto* paragraph = document.findParagraph(id);
        if (!paragraph || paragraph->styleProvenance()) continue;
        const auto* target = core::findBuiltInParagraphStyle(
            paragraph->styleId().value_or("Normal"));
        if (!target) continue;
        const bool ownsTypingPosition =
            typingPosition && typingPosition->paragraph_id == id;
        auto local = planParagraphStyleApplication(
            document, std::span<const core::NodeId>(&id, 1), *target,
            defaultFontFamily, defaultFontPointSize, typingFormat,
            ownsTypingPosition ? typingPosition : std::nullopt, true,
            ownsTypingPosition ? typingAdditionalOverrides
                               : core::CharacterFormatMask{});
        combined.operations.insert(
            combined.operations.end(),
            std::make_move_iterator(local.operations.begin()),
            std::make_move_iterator(local.operations.end()));
        if (local.resultingTypingFormat) {
            combined.resultingTypingFormat =
                std::move(local.resultingTypingFormat);
        }
        if (local.resultingTypingOverrideMask) {
            combined.resultingTypingOverrideMask =
                std::move(local.resultingTypingOverrideMask);
        }
    }
    return combined;
}

struct ReplacementFormattingPlan {
    std::vector<core::Operation> operations;
    core::CharacterFormat effective_typing_format;
    core::CharacterFormatMask effective_typing_override_mask;
    std::optional<core::CharacterFormat> insertion_format;
    std::optional<core::CharacterFormat> resulting_typing_format;
    std::optional<core::CharacterFormatMask>
        resulting_typing_override_mask;
};

ReplacementFormattingPlan planReplacementFormatting(
    const core::Document& document, const core::NormalizedRange& normalized,
    const core::CharacterFormat& typingFormat,
    const core::CharacterFormatMask& typingOverrideMask,
    const QString& defaultFontFamily, double defaultFontPointSize) {
    std::vector<core::NodeId> provenanceParagraphIds;
    if (!typingOverrideMask.empty()) {
        provenanceParagraphIds.reserve(
            normalized.end_paragraph_index -
            normalized.start_paragraph_index + 1);
        for (std::size_t index = normalized.start_paragraph_index;
             index <= normalized.end_paragraph_index; ++index) {
            provenanceParagraphIds.push_back(
                document.paragraphs()[index].id());
        }
    }
    auto initialization = planBuiltInStyleProvenanceInitialization(
        document, provenanceParagraphIds, defaultFontFamily,
        defaultFontPointSize, typingFormat, normalized.start,
        typingOverrideMask);

    ReplacementFormattingPlan plan;
    plan.operations = std::move(initialization.operations);
    plan.effective_typing_format =
        initialization.resultingTypingFormat.value_or(typingFormat);
    plan.effective_typing_override_mask =
        initialization.resultingTypingOverrideMask.value_or(
            typingOverrideMask);
    plan.insertion_format = insertionFormatFor(
        document, normalized, plan.effective_typing_format);
    plan.resulting_typing_format =
        std::move(initialization.resultingTypingFormat);
    plan.resulting_typing_override_mask =
        std::move(initialization.resultingTypingOverrideMask);
    return plan;
}

void appendInitialReplacementOperations(
    std::vector<core::Operation>& operations,
    const core::NormalizedRange& normalized,
    const core::Range& effectiveSelection, const std::u16string& text,
    const std::optional<core::CharacterFormat>& insertionFormat,
    const core::CharacterFormatDelta& directTypingDelta,
    core::Position& cursor,
    std::vector<core::NodeId>& emptyTypingCandidates) {
    const core::Position insertedStart = cursor;
    if (!normalized.empty()) {
        operations.emplace_back(core::ReplaceRange{
            effectiveSelection, text, insertionFormat});
    } else if (!text.empty()) {
        operations.emplace_back(core::InsertText{
            cursor, text, insertionFormat});
    }
    cursor.utf16_offset += text.size();
    if (!text.empty() && !directTypingDelta.empty()) {
        operations.emplace_back(core::SetCharacterFormat{
            {insertedStart, cursor}, directTypingDelta});
    } else if (text.empty() && !directTypingDelta.empty()) {
        emptyTypingCandidates.push_back(cursor.paragraph_id);
    }
}

bool appendEmptyTypingOverrideOperations(
    const core::Document& source,
    std::vector<core::Operation>& operations,
    std::vector<core::NodeId> emptyTypingCandidates,
    const core::CharacterFormatDelta& directTypingDelta,
    QString& error) {
    if (emptyTypingCandidates.empty()) return true;

    // A masked equal-to-baseline typing choice cannot be reconstructed from
    // CharacterFormat alone. Project the complete edit and stamp the exact
    // intent only onto result paragraphs that are genuinely empty. Both live
    // replacement and chat preview use this path, so preview acceptance cannot
    // silently drop explicit black/false/clear formatting.
    core::DocumentSession projectedSession(source);
    const auto projectedBase = projectedSession.snapshot();
    const auto projected = projectedSession.applyBatch(
        projectedBase.revision, operations);
    if (!projected) {
        error = errorText(projected.error());
        return false;
    }
    const auto projectedSnapshot = projectedSession.snapshot();
    std::sort(emptyTypingCandidates.begin(), emptyTypingCandidates.end());
    emptyTypingCandidates.erase(
        std::unique(emptyTypingCandidates.begin(),
                    emptyTypingCandidates.end()),
        emptyTypingCandidates.end());
    for (const auto id : emptyTypingCandidates) {
        const auto* paragraph = projectedSnapshot.document.findParagraph(id);
        if (paragraph && paragraph->text().empty()) {
            operations.emplace_back(core::SetParagraphMarkCharacterFormat{
                id, directTypingDelta});
        }
    }
    return true;
}

}  // namespace

struct DocumentCanvas::VisualLine {
    QTextLine line;
    int pageIndex{};
    std::size_t paragraphIndex{};
};

struct DocumentCanvas::EquationVisual {
    std::size_t utf16Offset{};
    std::unique_ptr<MathLayout> layout;
    MathLayoutMetrics metrics;
    QColor color{Qt::black};
};

struct ParagraphImageVisual {
    core::NodeId id;
    QImage image;
    std::size_t coreUtf16Offset{};
    std::size_t layoutUtf16Offset{};
    QRectF rect;
    int pageIndex{};
    core::ImageLayout layout;
};

struct DocumentCanvas::ParagraphVisual {
    core::NodeId id;
    QString text;
    std::unique_ptr<QTextLayout> layout;
    std::vector<VisualLine> lines;
    std::vector<EquationVisual> equations;
    std::vector<ParagraphImageVisual> images;

    [[nodiscard]] int layoutOffsetForCore(std::size_t offset) const {
        offset = std::min(offset, static_cast<std::size_t>(text.size()));
        return static_cast<int>(std::min<std::size_t>(
            offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    [[nodiscard]] std::size_t coreOffsetForLayout(int offset) const {
        const auto layoutSize = static_cast<std::size_t>(
            layout ? layout->text().size() : text.size());
        const int maximum = static_cast<int>(std::min<std::size_t>(
            layoutSize,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const std::size_t bounded = static_cast<std::size_t>(
            std::clamp(offset, 0, maximum));
        return std::min(
            static_cast<std::size_t>(text.size()), bounded);
    }
};

struct DocumentCanvas::TableCellVisual {
    std::size_t row{};
    std::size_t column{};
    QString text;
    std::unique_ptr<QTextLayout> layout;
    std::vector<QTextLine> lines;
    QRectF rect;
    int pageIndex{};
    double contentHeight{0.0};
    double paddingTop{4.0};
    double paddingRight{5.0};
    double paddingBottom{4.0};
    double paddingLeft{5.0};
    bool hasImportedPresentation{false};
    ImportedCellVerticalAlignment verticalAlignment{
        ImportedCellVerticalAlignment::top};
    std::optional<std::uint32_t> fillArgb;
    std::optional<ImportedCellBorderPresentation> borderTop;
    std::optional<ImportedCellBorderPresentation> borderRight;
    std::optional<ImportedCellBorderPresentation> borderBottom;
    std::optional<ImportedCellBorderPresentation> borderLeft;
};

struct DocumentCanvas::TableRowVisual {
    QRectF rect;
    int pageIndex{};
};

struct DocumentCanvas::TableVisual {
    core::NodeId id;
    std::size_t rows{};
    std::size_t columns{};
    bool headerRow{false};
    bool hasImportedPresentation{false};
    bool hasSemanticStyle{false};
    std::vector<TableCellVisual> cells;
    std::vector<TableRowVisual> rowVisuals;
    QRectF handleRect;
    int handlePageIndex{};
};

struct DocumentCanvas::BlockPlacement {
    core::NodeId id;
    core::BodyBlockKind kind{core::BodyBlockKind::paragraph};
    int firstPage{};
    int lastPage{};
    double top{};
    double bottom{};
};

struct DocumentCanvas::Hit {
    core::Position position;
    bool valid{false};
    int lineStart{};
    int lineEnd{};
    int layoutLineStart{};
    std::optional<TableCursor> tableCursor;
    std::optional<core::NodeId> tableHandle;
    std::optional<core::NodeId> image;
    std::optional<ImageResizeHandle> imageResizeHandle;
    QPointF pagePoint;
    int pageIndex{};
};

DocumentCanvas::DocumentCanvas(SpellChecker& spelling, QWidget* parent)
    : DocumentCanvas(spelling, core::DocumentSessionLimits{}, parent) {}

DocumentCanvas::DocumentCanvas(SpellChecker& spelling,
                               core::DocumentSessionLimits limits,
                               QWidget* parent)
    : QAbstractScrollArea(parent), spelling_(spelling),
      session_(std::make_unique<core::DocumentSession>(core::Document{},
                                                       limits)) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMouseTracking(true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAutoFillBackground(false);
    horizontalScrollBar()->setSingleStep(24);
    verticalScrollBar()->setSingleStep(32);

    const auto initial = session_->snapshot();
    const auto& paragraph = initial.document.paragraphs().front();
    selection_ = {{paragraph.id(), 0}, {paragraph.id(), 0}};
    typingFormat_ = currentCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();

    auto* blink = new QTimer(this);
    blink->setInterval(530);
    connect(blink, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
    blink->start();

    typingGroupTimer_ = new QTimer(this);
    typingGroupTimer_->setSingleShot(true);
    typingGroupTimer_->setInterval(1000);
    connect(typingGroupTimer_, &QTimer::timeout, this,
            &DocumentCanvas::endTypingGroup);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateStoryEditorGeometry(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateStoryEditorGeometry(); });
}

DocumentCanvas::~DocumentCanvas() = default;

void DocumentCanvas::setDocument(core::Document document) {
    endHeaderFooterEditing();
    endTypingGroup();
    endColorAdjustment();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    decodedImages_.clear();
    decodedImageBytes_ = 0;
    importedTables_.clear();
    // Recognize contiguous literal-marker runs without changing their text.
    // This gives imported and pasted lists stable per-list identity while a
    // no-op save can still preserve the original DOCX bytes exactly.
    adoptPlainTextLists(document, tabWidthSpaces_);
    session_ = std::make_unique<core::DocumentSession>(std::move(document));
    const auto snap = session_->snapshot();
    const auto& first = snap.document.paragraphs().front();
    const auto firstMarker = plainTextListMarker(fromUtf16(first.text()));
    const std::size_t firstOffset = firstMarker
        ? static_cast<std::size_t>(firstMarker->prefixLength)
        : 0;
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_.reset();
    selection_ = {{first.id(), firstOffset}, {first.id(), firstOffset}};
    typingFormat_ = currentCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    draggingTable_ = false;
    tableDropTargetValid_ = false;
    tableDropBefore_.reset();
    imageResizeDrag_.reset();
    modified_ = false;
    nonTextModified_ = false;
    pageLayoutModified_ = false;
    previewId_.reset();
    previewRevision_ = {};
    previewCursor_.reset();
    previewOperations_.clear();
    previewHasNonTextChanges_ = false;
    undoCursorHistory_.clear();
    redoCursorHistory_.clear();
    currentStateId_ = 0;
    savedStateId_ = 0;
    nextStateId_ = 1;
    currentNonTextStateId_ = 0;
    savedNonTextStateId_ = 0;
    nextNonTextStateId_ = 1;
    currentPageLayoutStateId_ = 0;
    savedPageLayoutStateId_ = 0;
    nextPageLayoutStateId_ = 1;
    invalidateLayout();
    updateStatus();
    viewport()->update();
}

void DocumentCanvas::setEditorDefaults(const QString& fontFamily,
                                       double fontPointSize,
                                       int tabWidthSpaces) {
    const QString trimmedFamily = fontFamily.trimmed();
    if (trimmedFamily.isEmpty() || !std::isfinite(fontPointSize) ||
        fontPointSize < 1.0 || fontPointSize > 400.0 ||
        tabWidthSpaces < 1 || tabWidthSpaces > 32) {
        emit operationFailed(tr("The editor defaults are invalid."));
        return;
    }
    defaultFontFamily_ = trimmedFamily;
    defaultFontPointSize_ = std::round(fontPointSize * 2.0) / 2.0;
    tabWidthSpaces_ = tabWidthSpaces;
    invalidateLayout();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
}

void DocumentCanvas::setDefaultListLayout(const core::ListLayout& layout) {
    for (const auto& level : layout.levels) {
        if (level.bullet_indent_spaces < 0 ||
            level.bullet_indent_spaces > core::kMaximumListIndentSpaces ||
            level.text_indent_spaces < 0 ||
            level.text_indent_spaces > core::kMaximumListTextIndentSpaces) {
            emit operationFailed(tr("The list defaults are invalid."));
            return;
        }
    }
    defaultListLayout_ = layout;
}

core::DocumentSnapshot DocumentCanvas::snapshot() const { return session_->snapshot(); }
core::Range DocumentCanvas::selection() const { return selection_; }

QString DocumentCanvas::selectedText() const {
    const auto snap = session_->snapshot();
    if (selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        if (!table) return {};
        if (tableCursor_) {
            const auto* cell = table->cell(tableCursor_->row,
                                           tableCursor_->column);
            if (!cell) return {};
            const auto focus = std::min(tableCursor_->utf16Offset,
                                        cell->text.size());
            if (tableSelectionAnchor_ && *tableSelectionAnchor_ != focus) {
                const auto anchor = std::min(*tableSelectionAnchor_,
                                             cell->text.size());
                const auto start = std::min(anchor, focus);
                const auto end = std::max(anchor, focus);
                return fromUtf16(cell->text.substr(start, end - start));
            }
            return {};
        }
        if (tableCellSelection_) {
            QString result;
            const auto cells = selectedTableCells(*table);
            const auto firstRow = std::min(tableCellSelection_->anchorRow,
                                           tableCellSelection_->focusRow);
            std::size_t previousRow = firstRow;
            bool first = true;
            for (const auto& [row, column] : cells) {
                if (!first) {
                    result += row == previousRow ? QLatin1Char('\t')
                                                 : QLatin1Char('\n');
                }
                if (const auto* cell = table->cell(row, column)) {
                    result += fromUtf16(cell->text);
                }
                first = false;
                previousRow = row;
            }
            return result;
        }
        QString result;
        for (std::size_t row = 0; row < table->rowCount(); ++row) {
            for (std::size_t column = 0; column < table->columnCount(); ++column) {
                if (column != 0) result += QLatin1Char('\t');
                if (const auto* cell = table->cell(row, column)) {
                    result += fromUtf16(cell->text);
                }
            }
            if (row + 1 < table->rowCount()) result += QLatin1Char('\n');
        }
        return result;
    }
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized || normalized.value().empty()) {
        return {};
    }
    const auto& range = normalized.value();
    QString result;
    for (std::size_t index = range.start_paragraph_index; index <= range.end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const std::size_t start = index == range.start_paragraph_index ? range.start.utf16_offset : 0;
        const std::size_t end = index == range.end_paragraph_index
                                    ? range.end.utf16_offset
                                    : paragraph.text().size();
        result += visibleParagraphText(paragraph, start, end);
        if (index != range.end_paragraph_index) {
            result += QLatin1Char('\n');
        }
    }
    return result;
}

bool DocumentCanvas::hasClipboardSelection() const noexcept {
    if (tableCursor_) {
        return tableSelectionAnchor_ &&
               *tableSelectionAnchor_ != tableCursor_->utf16Offset;
    }
    if (tableCellSelection_) return true;
    if (selectedTable_) return true;
    return selection_.anchor != selection_.focus;
}

QString DocumentCanvas::outlineText(std::size_t maxCharacters) const {
    const auto snap = session_->snapshot();
    QString result;
    std::size_t index = 0;
    for (const auto& block : snap.document.bodyBlocks()) {
        QString text;
        if (block.kind == core::BodyBlockKind::paragraph) {
            const auto* paragraph = snap.document.findParagraph(block.id);
            if (paragraph) {
                text = visibleParagraphText(
                           *paragraph, 0, paragraph->text().size())
                           .trimmed();
            }
        } else if (const auto* table = snap.document.findTable(block.id)) {
            QStringList cells;
            for (const auto& cell : table->cells()) {
                const auto cellText = fromUtf16(cell.text).trimmed();
                if (!cellText.isEmpty()) cells.push_back(cellText);
            }
            text = tr("Table %1x%2: %3")
                       .arg(table->rowCount())
                       .arg(table->columnCount())
                       .arg(cells.join(QStringLiteral(" | ")));
        }
        if (text.isEmpty()) continue;
        result += QStringLiteral("%1. %2\n").arg(++index).arg(text.left(240));
        if (static_cast<std::size_t>(result.size()) >= maxCharacters) {
            result.truncate(static_cast<qsizetype>(maxCharacters));
            result += QStringLiteral("\n[…]");
            break;
        }
    }
    return result;
}

void DocumentCanvas::setZoomPercent(int percent) {
    const int bounded = std::clamp(
        percent, kMinimumZoomPercent, kMaximumZoomPercent);
    if (zoomPercent_ == bounded) {
        return;
    }
    zoomPercent_ = bounded;
    // Pagination and line layout use points, not screen pixels. A view-only
    // zoom must therefore leave the document revision, editing state, and
    // cached pagination untouched; only the scrollable pixel extent changes.
    updateScrollBars();
    updateStoryEditorGeometry();
    viewport()->update();
    emit zoomChanged(zoomPercent_);
}

int DocumentCanvas::pageCount() const {
    ensureLayout();
    return pageCount_;
}

std::uint64_t DocumentCanvas::layoutGeneration() const {
    ensureLayout();
    return layoutGeneration_;
}

int DocumentCanvas::currentPageNumber() const {
    ensureLayout();
    int page = 0;
    if (const auto* caretLine = visualLineForCaret()) {
        page = caretLine->pageIndex;
    }
    if (selectedTable_) {
        const auto found = std::find_if(
            tableVisuals_.begin(), tableVisuals_.end(),
            [this](const auto& table) { return table->id == *selectedTable_; });
        if (found != tableVisuals_.end()) {
            page = (*found)->handlePageIndex;
        }
    }
    if (tableCursor_) {
        for (const auto& tableVisual : tableVisuals_) {
            if (tableVisual->id != tableCursor_->tableId) continue;
            const auto found = std::find_if(
                tableVisual->cells.begin(), tableVisual->cells.end(),
                [this](const TableCellVisual& cell) {
                    return cell.row == tableCursor_->row &&
                           cell.column == tableCursor_->column;
                });
            if (found != tableVisual->cells.end()) {
                page = found->pageIndex;
            }
            break;
        }
    }
    if (const auto selectedImage = selectedInlineImageId()) {
        for (const auto& paragraph : visuals_) {
            const auto found = std::find_if(
                paragraph->images.begin(), paragraph->images.end(),
                [selectedImage](const ParagraphImageVisual& image) {
                    return image.id == *selectedImage;
                });
            if (found != paragraph->images.end()) {
                page = found->pageIndex;
                break;
            }
        }
    }
    return std::clamp(page, 0, std::max(0, pageCount_ - 1)) + 1;
}

void DocumentCanvas::markSaved() noexcept {
    endTypingGroup();
    endColorAdjustment();
    savedStateId_ = currentStateId_;
    savedNonTextStateId_ = currentNonTextStateId_;
    savedPageLayoutStateId_ = currentPageLayoutStateId_;
    updateDirtyFlags();
}

void DocumentCanvas::markRecovered() {
    endTypingGroup();
    endColorAdjustment();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    currentStateId_ = nextStateId_++;
    currentNonTextStateId_ = nextNonTextStateId_++;
    currentPageLayoutStateId_ = nextPageLayoutStateId_++;
    updateDirtyFlags();
    emit documentChanged(session_->snapshot().revision.value());
}

DocumentCanvas::CursorState DocumentCanvas::captureEditorState() const {
    return CursorState{
        selection_, typingFormat_, typingOverrideMask_, tableCursor_,
        tableSelectionAnchor_,
        tableCellSelection_, selectedTable_,
        lineAffinity_, preferredVerticalX_,
        pageWidthPoints_, pageHeightPoints_,
        marginTopPoints_, marginRightPoints_, marginBottomPoints_,
        marginLeftPoints_, currentStateId_, currentNonTextStateId_,
        currentPageLayoutStateId_};
}

void DocumentCanvas::restoreEditorState(const CursorState& state) {
    selection_ = state.selection;
    typingFormat_ = state.typingFormat;
    typingOverrideMask_ = state.typingOverrideMask;
    tableCursor_ = state.tableCursor;
    tableSelectionAnchor_ = state.tableSelectionAnchor;
    tableCellSelection_ = state.tableCellSelection;
    tableMouseSelectionAnchor_.reset();
    selectedTable_ = state.selectedTable;
    lineAffinity_ = state.lineAffinity;
    preferredVerticalX_ = state.preferredVerticalX;
    pageWidthPoints_ = state.pageWidthPoints;
    pageHeightPoints_ = state.pageHeightPoints;
    marginTopPoints_ = state.marginTopPoints;
    marginRightPoints_ = state.marginRightPoints;
    marginBottomPoints_ = state.marginBottomPoints;
    marginLeftPoints_ = state.marginLeftPoints;
    currentStateId_ = state.stateId;
    currentNonTextStateId_ = state.nonTextStateId;
    currentPageLayoutStateId_ = state.pageLayoutStateId;
    updateDirtyFlags();
}

void DocumentCanvas::updateDirtyFlags() {
    modified_ = currentStateId_ != savedStateId_;
    nonTextModified_ = currentNonTextStateId_ != savedNonTextStateId_;
    pageLayoutModified_ =
        currentPageLayoutStateId_ != savedPageLayoutStateId_;
}

void DocumentCanvas::synchronizeCursorHistory() {
    const core::DocumentHistoryDepths retained = session_->historyDepths();
    const auto trimEvictedPrefix = [](std::vector<CursorHistoryEntry>& stack,
                                      const std::size_t retainedTransactions) {
        const std::size_t recordedTransactions = static_cast<std::size_t>(
            std::count_if(stack.begin(), stack.end(),
                          [](const CursorHistoryEntry& entry) {
                              return entry.documentTransaction;
                          }));
        if (recordedTransactions <= retainedTransactions) return;

        std::size_t transactionsToRemove =
            recordedTransactions - retainedTransactions;
        auto retainedBegin = stack.begin();
        while (retainedBegin != stack.end() && transactionsToRemove > 0) {
            if (retainedBegin->documentTransaction) {
                --transactionsToRemove;
            }
            ++retainedBegin;
        }
        // A local layout-only entry older than an evicted document edit may
        // contain selections into document nodes that no longer exist at the
        // retained baseline. Discard the complete prefix through the last
        // evicted document transaction so the combined history stays
        // contiguous and every restored cursor belongs to its document.
        stack.erase(stack.begin(), retainedBegin);
    };
    trimEvictedPrefix(undoCursorHistory_, retained.undo);
    trimEvictedPrefix(redoCursorHistory_, retained.redo);
}

void DocumentCanvas::recordLayoutChange(const CursorState& before) {
    endTypingGroup();
    resetVerticalNavigation();
    currentStateId_ = nextStateId_++;
    currentNonTextStateId_ = nextNonTextStateId_++;
    currentPageLayoutStateId_ = nextPageLayoutStateId_++;
    updateDirtyFlags();
    undoCursorHistory_.push_back(
        CursorHistoryEntry{before, captureEditorState(), false});
    redoCursorHistory_.clear();
    invalidateLayout();
    viewport()->update();
    emit documentChanged(session_->snapshot().revision.value());
    updateStatus();
}

void DocumentCanvas::setImportedPresentation(
    std::vector<ImportedInlineImagePresentation> images,
    std::vector<ImportedTablePresentation> tables) {
    decodedImages_.clear();
    decodedImageBytes_ = 0;
    const auto snapshot = session_->snapshot();
    for (auto& image : images) {
        if (!image.id.isValid() || image.image.isNull() ||
            !snapshot.document.findImage(image.id) ||
            image.image.sizeInBytes() <= 0 ||
            image.image.sizeInBytes() >
                kMaximumAggregateDecodedRasterBytes - decodedImageBytes_) {
            continue;
        }
        const auto [found, inserted] = decodedImages_.emplace(
            image.id, std::move(image.image));
        if (inserted) {
            decodedImageBytes_ += found->second.sizeInBytes();
        }
    }
    importedTables_ = std::move(tables);
    invalidateLayout();
    viewport()->update();
    updateStatus();
}

std::optional<std::pair<core::Position, core::ImageAtom>>
DocumentCanvas::selectedInlineImage() const {
    if (selectedTable_ || tableCursor_ || tableCellSelection_) {
        return std::nullopt;
    }
    const auto snapshot = session_->snapshot();
    const auto normalized = snapshot.document.normalizeRange(selection_);
    if (!normalized ||
        normalized.value().start_paragraph_index !=
            normalized.value().end_paragraph_index ||
        normalized.value().end.utf16_offset !=
            normalized.value().start.utf16_offset + 1U) {
        return std::nullopt;
    }
    const auto* paragraph = snapshot.document.findParagraph(
        normalized.value().start.paragraph_id);
    const auto* image = paragraph
        ? paragraph->imageAt(normalized.value().start.utf16_offset)
        : nullptr;
    if (!image) return std::nullopt;
    return std::pair{normalized.value().start, *image};
}

std::optional<core::NodeId> DocumentCanvas::selectedInlineImageId() const {
    const auto selected = selectedInlineImage();
    return selected
        ? std::optional<core::NodeId>(selected->second.id)
        : std::nullopt;
}

std::optional<core::ImageLayout> DocumentCanvas::selectedImageLayout() const {
    const auto selected = selectedInlineImage();
    return selected ? std::optional<core::ImageLayout>(selected->second.layout)
                    : std::nullopt;
}

QString DocumentCanvas::selectedImageAccessibleName() const {
    const auto selected = selectedInlineImage();
    return selected
        ? QString::fromUtf8(selected->second.accessible_name.data(),
                            static_cast<qsizetype>(
                                selected->second.accessible_name.size()))
        : QString();
}

const QImage* DocumentCanvas::decodedInlineImage(
    const core::ImageAtom& image) const {
    const auto cached = decodedImages_.find(image.id);
    if (cached != decodedImages_.end()) return &cached->second;

    if (decodedImageBytes_ >= kMaximumAggregateDecodedRasterBytes) {
        return nullptr;
    }
    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    limits.maximum_decoded_bytes = std::min(
        limits.maximum_decoded_bytes,
        kMaximumAggregateDecodedRasterBytes - decodedImageBytes_);
    auto decoded = decodeRasterImage(image.encoded_payload.bytes(), limits);
    const auto expected = image.format == core::ImageFormat::png
        ? raster::Format::png
        : raster::Format::jpeg;
    if (!decoded.ok() || decoded.format != expected ||
        decoded.image.sizeInBytes() <= 0 ||
        decoded.image.sizeInBytes() >
            kMaximumAggregateDecodedRasterBytes - decodedImageBytes_) {
        return nullptr;
    }
    const auto [inserted, wasInserted] = decodedImages_.emplace(
        image.id, std::move(decoded.image));
    if (!wasInserted) return &inserted->second;
    decodedImageBytes_ += inserted->second.sizeInBytes();
    return &inserted->second;
}

void DocumentCanvas::reconcileDecodedImageCache() {
    const auto snapshot = session_->snapshot();
    for (auto cached = decodedImages_.begin(); cached != decodedImages_.end();) {
        if (snapshot.document.findImage(cached->first)) {
            ++cached;
            continue;
        }
        decodedImageBytes_ -= cached->second.sizeInBytes();
        cached = decodedImages_.erase(cached);
    }
    decodedImageBytes_ = std::max<std::int64_t>(0, decodedImageBytes_);
}

bool DocumentCanvas::rejectLiveEditDuringPreview() {
    if (!previewId_) {
        return false;
    }
    emit operationFailed(
        tr("A Codex preview is open. Accept or discard it before editing the document."));
    return true;
}

void DocumentCanvas::endTypingGroup() noexcept {
    typingGroupActive_ = false;
    if (typingGroupTimer_) {
        typingGroupTimer_->stop();
    }
}

void DocumentCanvas::clearPendingSpellingWord() noexcept {
    const bool changed = pendingSpellingWord_.has_value();
    pendingSpellingWord_.reset();
    if (changed) viewport()->update();
}

void DocumentCanvas::setPendingSpellingWordFromTypedText(
    const QString& insertedText) {
    // A delimiter commits the word immediately. In particular, do not let a
    // space inserted before existing text transfer the pending state to the
    // following word merely because the resulting caret touches its start.
    if (insertedText.isEmpty() ||
        !canContinueSpellingWord(insertedText.back())) {
        clearPendingSpellingWord();
        viewport()->update();
        return;
    }

    const auto snap = session_->snapshot();
    if (tableCursor_ &&
        tableSelectionAnchor_.value_or(tableCursor_->utf16Offset) ==
            tableCursor_->utf16Offset) {
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column) : nullptr;
        const auto word = cell
            ? spellingWordAt(fromUtf16(cell->text),
                             tableCursor_->utf16Offset)
            : std::nullopt;
        if (word) {
            pendingSpellingWord_ = PendingSpellingWord{
                PendingSpellingWord::Container::tableCell,
                tableCursor_->tableId, tableCursor_->row,
                tableCursor_->column, word->start, word->end};
        } else {
            clearPendingSpellingWord();
        }
        viewport()->update();
        return;
    }

    if (!selectedTable_ && selection_.anchor == selection_.focus) {
        const auto* paragraph = snap.document.findParagraph(
            selection_.focus.paragraph_id);
        const auto word = paragraph
            ? spellingWordAt(fromUtf16(paragraph->text()),
                             selection_.focus.utf16_offset)
            : std::nullopt;
        if (word) {
            pendingSpellingWord_ = PendingSpellingWord{
                PendingSpellingWord::Container::bodyParagraph,
                selection_.focus.paragraph_id, 0, 0,
                word->start, word->end};
        } else {
            clearPendingSpellingWord();
        }
    } else {
        clearPendingSpellingWord();
    }
    viewport()->update();
}

void DocumentCanvas::refreshPendingSpellingWordAfterEdit() {
    if (!pendingSpellingWord_) return;
    const auto pending = *pendingSpellingWord_;
    const auto snap = session_->snapshot();
    std::optional<SpellingWord> word;
    if (pending.container == PendingSpellingWord::Container::tableCell &&
        tableCursor_ && tableCursor_->tableId == pending.ownerId &&
        tableCursor_->row == pending.row &&
        tableCursor_->column == pending.column &&
        tableSelectionAnchor_.value_or(tableCursor_->utf16Offset) ==
            tableCursor_->utf16Offset) {
        const auto* table = snap.document.findTable(pending.ownerId);
        const auto* cell = table
            ? table->cell(pending.row, pending.column) : nullptr;
        if (cell) {
            word = spellingWordAt(fromUtf16(cell->text),
                                  tableCursor_->utf16Offset);
        }
    } else if (
        pending.container == PendingSpellingWord::Container::bodyParagraph &&
        !selectedTable_ && selection_.anchor == selection_.focus &&
        selection_.focus.paragraph_id == pending.ownerId) {
        const auto* paragraph = snap.document.findParagraph(pending.ownerId);
        if (paragraph) {
            word = spellingWordAt(fromUtf16(paragraph->text()),
                                  selection_.focus.utf16_offset);
        }
    }

    if (word) {
        pendingSpellingWord_->start = word->start;
        pendingSpellingWord_->end = word->end;
    } else {
        clearPendingSpellingWord();
    }
    viewport()->update();
}

void DocumentCanvas::commitPendingSpellingWordIfCaretLeft() {
    if (!pendingSpellingWord_) return;
    const auto& pending = *pendingSpellingWord_;
    bool remainsOnPendingWord = false;
    if (pending.container == PendingSpellingWord::Container::tableCell &&
        tableCursor_ && tableCursor_->tableId == pending.ownerId &&
        tableCursor_->row == pending.row &&
        tableCursor_->column == pending.column) {
        const auto anchor = tableSelectionAnchor_.value_or(
            tableCursor_->utf16Offset);
        remainsOnPendingWord = anchor >= pending.start &&
            anchor <= pending.end &&
            tableCursor_->utf16Offset >= pending.start &&
            tableCursor_->utf16Offset <= pending.end;
    } else if (
        pending.container == PendingSpellingWord::Container::bodyParagraph &&
        !selectedTable_ &&
        selection_.anchor.paragraph_id == pending.ownerId &&
        selection_.focus.paragraph_id == pending.ownerId) {
        remainsOnPendingWord =
            selection_.anchor.utf16_offset >= pending.start &&
            selection_.anchor.utf16_offset <= pending.end &&
            selection_.focus.utf16_offset >= pending.start &&
            selection_.focus.utf16_offset <= pending.end;
    }
    if (!remainsOnPendingWord) {
        clearPendingSpellingWord();
        viewport()->update();
    }
}

bool DocumentCanvas::suppressSpellingWord(
    PendingSpellingWord::Container container, core::NodeId ownerId,
    std::size_t row, std::size_t column, std::size_t start,
    std::size_t end) const noexcept {
    if (!pendingSpellingWord_) return false;
    const auto& pending = *pendingSpellingWord_;
    return pending.container == container && pending.ownerId == ownerId &&
           pending.row == row && pending.column == column &&
           pending.start == start && pending.end == end;
}

void DocumentCanvas::resetVerticalNavigation() noexcept {
    lineAffinity_.reset();
    preferredVerticalX_.reset();
}

void DocumentCanvas::invalidateLayout() {
    layoutValid_ = false;
    visuals_.clear();
    tableVisuals_.clear();
    blockPlacements_.clear();
    updateScrollBars();
}

void DocumentCanvas::ensureLayout() const {
    const bool preview = previewId_.has_value();
    core::Revision current = session_->snapshot().revision;
    if (preview) {
        const auto snapshot = session_->previewSnapshot(*previewId_);
        if (snapshot) {
            current = snapshot.value().revision;
        }
    }
    if (!layoutValid_ || current != layoutRevision_ || preview != layoutIsPreview_) {
        rebuildLayout();
    }
}

void DocumentCanvas::rebuildLayout() const {
    const auto liveSnapshot = session_->snapshot();
    auto snap = liveSnapshot;
    bool preview = false;
    if (previewId_) {
        const auto previewSnapshot = session_->previewSnapshot(*previewId_);
        if (previewSnapshot) {
            snap = {previewSnapshot.value().revision, previewSnapshot.value().document};
            preview = true;
        }
    }
    visuals_.clear();
    tableVisuals_.clear();
    blockPlacements_.clear();
    visuals_.reserve(snap.document.paragraphs().size());
    tableVisuals_.reserve(snap.document.tables().size());
    blockPlacements_.reserve(snap.document.bodyBlocks().size());
    int page = 0;
    double y = marginTopPoints_;
    const double pageBottom = pageHeightPoints_ - marginBottomPoints_;

    struct ListVisualMetrics {
        qreal maximumMarkerWidth{0.0};
        qreal spaceAdvance{0.0};
    };
    using ListVisualKey = std::pair<core::NodeId, std::uint8_t>;
    std::map<ListVisualKey, ListVisualMetrics> listMetrics;
    for (const auto& paragraph : snap.document.paragraphs()) {
        const auto& format = paragraph.format();
        if (!format.list_id || !format.list_level || !format.list_layout) {
            continue;
        }
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (!marker) continue;
        auto& metrics = listMetrics[{*format.list_id, *format.list_level}];
        const auto markerOffset = static_cast<std::size_t>(marker->indent.size());
        const auto contentOffset = std::min(
            paragraph.text().size(),
            static_cast<std::size_t>(marker->prefixLength));
        const QFont markerFont = fontFrom(
            paragraph.characterFormatAt(markerOffset + 1),
            defaultFontFamily_, defaultFontPointSize_);
        const QFont contentFont = fontFrom(
            paragraph.characterFormatAt(contentOffset),
            defaultFontFamily_, defaultFontPointSize_);
        metrics.maximumMarkerWidth = std::max(
            metrics.maximumMarkerWidth,
            QFontMetricsF(markerFont).horizontalAdvance(marker->marker));
        if (metrics.spaceAdvance <= 0.0) {
            metrics.spaceAdvance = std::max<qreal>(
                0.5, QFontMetricsF(contentFont).horizontalAdvance(
                         QLatin1Char(' ')));
        }
    }

    const auto layoutParagraph = [&](const core::Paragraph& paragraph,
                                     std::size_t paragraphIndexValue) {
        const auto& paragraphFormat = paragraph.format();
        y += emuToPoints(paragraphFormat.space_before_emu);
        if (paragraphFormat.page_break_before.value_or(false) && y > marginTopPoints_) {
            ++page;
            y = marginTopPoints_;
        }
        const int initialPage = page;
        const double initialY = y;

        auto visual = std::make_unique<ParagraphVisual>();
        visual->id = paragraph.id();
        visual->text = fromUtf16(paragraph.text());
        core::CharacterFormat defaultFormat;
        defaultFormat.font_family = defaultFontFamily_.toStdString();
        defaultFormat.font_size_half_points = static_cast<std::int32_t>(
            std::lround(defaultFontPointSize_ * 2.0));

        const double leftIndent = emuToPoints(paragraphFormat.left_indent_emu);
        const double rightIndent = emuToPoints(paragraphFormat.right_indent_emu);
        const double firstIndent = emuToPoints(
            paragraphFormat.first_line_indent_emu);
        const double baseWidth = std::max(
            36.0, pageWidthPoints_ - marginLeftPoints_ -
                      marginRightPoints_ - leftIndent - rightIndent);
        const double pageContentHeight = std::max(
            18.0, pageHeightPoints_ - marginTopPoints_ -
                      marginBottomPoints_);

        QString layoutText = visual->text;
        visual->images.reserve(paragraph.images().size());
        for (const auto& image : paragraph.images()) {
            if (image.utf16_offset >=
                    static_cast<std::size_t>(visual->text.size()) ||
                paragraph.text()[image.utf16_offset] !=
                    core::kInlineObjectReplacementCharacter) {
                continue;
            }
            const QImage* decodedImage = decodedInlineImage(image);
            if (!decodedImage || decodedImage->isNull()) continue;
            const auto coreOffset = image.utf16_offset;
            const double widthPoints =
                static_cast<double>(image.width_emu) / kEmuPerPoint;
            const double heightPoints =
                static_cast<double>(image.height_emu) / kEmuPerPoint;
            const bool anchored = image.layout.placement !=
                core::ImagePlacement::inline_with_text;
            const double fitScale = std::min(
                {1.0, baseWidth / widthPoints,
                 pageContentHeight / heightPoints});
            const double width = widthPoints * fitScale;
            const double height = heightPoints * fitScale;
            const auto layoutOffset = coreOffset;
            layoutText[static_cast<qsizetype>(layoutOffset)] = anchored
                ? QChar(0x2060)
                : QChar(0x00a0);
            visual->images.push_back(
                {image.id, *decodedImage,
                 coreOffset, layoutOffset,
                 QRectF(0.0, 0.0, width, height), 0, image.layout});
        }

        QList<QTextLayout::FormatRange> ranges;
        for (const auto& run : paragraph.characterFormats()) {
            QTextLayout::FormatRange range;
            range.start = visual->layoutOffsetForCore(run.start);
            range.length = std::max(
                0, visual->layoutOffsetForCore(run.end) - range.start);
            range.format = qtFormat(run.format, defaultFontFamily_,
                                    defaultFontPointSize_);
            if (range.length > 0) ranges.push_back(range);
        }
        for (const auto& image : visual->images) {
            if (image.layout.placement !=
                core::ImagePlacement::inline_with_text) {
                continue;
            }
            const auto characterFormat = paragraph.characterFormatAt(
                std::min(paragraph.text().size(),
                         image.coreUtf16Offset + 1U));
            QFont spacerFont = fontFrom(
                characterFormat, defaultFontFamily_, defaultFontPointSize_);
            const qreal naturalWidth = std::max<qreal>(
                0.1, QFontMetricsF(spacerFont).horizontalAdvance(
                         QChar(0x00a0)));
            spacerFont.setWordSpacing(std::max<qreal>(
                0.0, image.rect.width() - naturalWidth));

            QTextLayout::FormatRange range;
            range.start = static_cast<int>(image.layoutUtf16Offset);
            range.length = 1;
            range.format = qtFormat(characterFormat, defaultFontFamily_,
                                    defaultFontPointSize_);
            range.format.setFont(spacerFont);
            ranges.push_back(range);
        }

        visual->equations.reserve(paragraph.equations().size());
        for (const auto& equation : paragraph.equations()) {
            if (equation.utf16_offset >=
                    static_cast<std::size_t>(visual->text.size()) ||
                paragraph.text()[equation.utf16_offset] !=
                    core::kInlineObjectReplacementCharacter) {
                continue;
            }
            const auto parsed = math::parseLatex(
                equation.canonical_latex, editorEquationLimits());
            if (!parsed) continue;

            const auto characterFormat = paragraph.characterFormatAt(
                equation.utf16_offset + 1);
            const QFont equationFont = fontFrom(
                characterFormat, defaultFontFamily_, defaultFontPointSize_);
            auto equationLayout = std::make_unique<MathLayout>(
                parsed.value(), equationFont);
            const auto metrics = equationLayout->metrics();
            if (!equationLayout->valid() || metrics.width <= 0.0 ||
                metrics.height() <= 0.0) {
                continue;
            }

            // QTextLayout has no inline-object callback. Keep the semantic
            // equation at one UTF-16 position and give that position a
            // non-breaking space with equation-sized word spacing. Unlike a
            // stretched glyph run, this preserves the following text run in
            // Qt's PDF backend. The vector equation is painted over the blank
            // slot below. Pictures use the same semantic one-character slot,
            // so editor, layout, PDF, and DOCX offsets stay identical.
            const auto layoutOffset = static_cast<std::size_t>(
                visual->layoutOffsetForCore(equation.utf16_offset));
            layoutText[static_cast<qsizetype>(layoutOffset)] =
                QChar(0x00a0);
            QFont spacerFont = equationFont;
            const QFontMetricsF scaledMetrics(spacerFont);
            const qreal naturalWidth = std::max<qreal>(
                0.1, scaledMetrics.horizontalAdvance(QChar(0x00a0)));
            spacerFont.setWordSpacing(std::max<qreal>(
                0.0, metrics.width - naturalWidth));

            QTextLayout::FormatRange range;
            range.start = static_cast<int>(layoutOffset);
            range.length = 1;
            range.format = qtFormat(characterFormat, defaultFontFamily_,
                                    defaultFontPointSize_);
            range.format.setFont(spacerFont);
            ranges.push_back(range);
            visual->equations.push_back(
                {layoutOffset, std::move(equationLayout), metrics,
                 fromArgb(characterFormat.foreground_argb.value_or(
                     kDefaultTextArgb))});
        }

        bool semanticList = false;
        double listBulletOffset = 0.0;
        double listTextOffset = 0.0;
        QList<QTextOption::Tab> listTabs;
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (marker && paragraphFormat.list_id && paragraphFormat.list_level &&
            paragraphFormat.list_layout) {
            const auto key = ListVisualKey{*paragraphFormat.list_id,
                                           *paragraphFormat.list_level};
            const auto found = listMetrics.find(key);
            if (found != listMetrics.end()) {
                const auto level = static_cast<std::size_t>(
                    *paragraphFormat.list_level);
                const auto& levelLayout =
                    paragraphFormat.list_layout->levels[level];
                const double spaceAdvance = found->second.spaceAdvance;
                listBulletOffset =
                    levelLayout.bullet_indent_spaces * spaceAdvance;
                listTextOffset = listBulletOffset +
                    found->second.maximumMarkerWidth +
                    levelLayout.text_indent_spaces * spaceAdvance;
                QTextOption::Tab textTab;
                textTab.position = listTextOffset - listBulletOffset;
                textTab.type = QTextOption::LeftTab;
                listTabs.push_back(textTab);
                semanticList = true;

                // Prefix characters retain their semantic offsets for
                // selection, undo, and DOCX output. Layout positions the
                // marker itself, so suppress stored indentation and make the
                // separator a real tab stop in the display-only string.
                for (qsizetype index = 0;
                     index < marker->indent.size(); ++index) {
                    layoutText[visual->layoutOffsetForCore(
                        static_cast<std::size_t>(index))] = QChar(0x2060);
                }
                const qsizetype separatorStart =
                    marker->indent.size() + marker->marker.size();
                for (qsizetype index = 0;
                     index < marker->separator.size(); ++index) {
                    layoutText[visual->layoutOffsetForCore(
                        static_cast<std::size_t>(separatorStart + index))] =
                        index == 0 ? QChar(QLatin1Char('\t'))
                                   : QChar(0x2060);
                }
            }
        }

        // Keep a real, invisible layout line for every truly empty paragraph
        // so its caret, hit target, IME rectangle, and tab origin all exist.
        // An image-only paragraph already contains display-only inline slots.
        const QString finalLayoutText = layoutText.isEmpty()
            ? QString(QChar(0x2060))
            : layoutText;
        visual->layout = std::make_unique<QTextLayout>(
            finalLayoutText, fontFrom(defaultFormat, defaultFontFamily_,
                                      defaultFontPointSize_));
        visual->layout->setFormats(ranges);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setAlignment(paragraphAlignment(paragraphFormat));
        option.setUseDesignMetrics(true);
        const std::size_t tabReferenceOffset = marker
            ? std::min(paragraph.text().size(),
                       static_cast<std::size_t>(marker->prefixLength))
            : 0;
        const QFont tabReferenceFont = fontFrom(
            paragraph.characterFormatAt(tabReferenceOffset),
            defaultFontFamily_, defaultFontPointSize_);
        option.setTabStopDistance(std::max<qreal>(
            1.0, QFontMetricsF(tabReferenceFont)
                     .horizontalAdvance(QLatin1Char(' ')) *
                     static_cast<qreal>(tabWidthSpaces_)));
        if (!listTabs.isEmpty()) option.setTabs(listTabs);
        visual->layout->setTextOption(option);

        bool firstLine = true;

        // Probe character positions without wrap exclusions. The canonical
        // authored-DOCX subset writes moving pictures relative to character /
        // paragraph with zero offsets, so the picture origin must use the
        // U+FFFC character x and the paragraph y. Fixed pictures use page /
        // page with zero offsets. Wrap distances enlarge only the text
        // exclusion; they never translate or resize the picture itself.
        std::unordered_map<core::NodeId, double, core::NodeIdHash>
            movingAnchorXs;
        if (std::any_of(
                visual->images.begin(), visual->images.end(),
                [](const ParagraphImageVisual& image) {
                    return image.layout.placement !=
                               core::ImagePlacement::inline_with_text &&
                           image.layout.move_with_text;
                })) {
            QTextLayout probe(
                finalLayoutText,
                fontFrom(defaultFormat, defaultFontFamily_,
                         defaultFontPointSize_));
            probe.setFormats(ranges);
            probe.setTextOption(option);
            bool probeFirstLine = true;
            probe.beginLayout();
            while (true) {
                QTextLine line = probe.createLine();
                if (!line.isValid()) break;
                const double extra = semanticList
                    ? (probeFirstLine ? listBulletOffset : listTextOffset)
                    : (probeFirstLine ? firstIndent : 0.0);
                line.setLineWidth(std::max(18.0, baseWidth - extra));
                line.setPosition(QPointF(
                    marginLeftPoints_ + leftIndent + extra, 0.0));
                const auto lineStart = static_cast<std::size_t>(
                    line.textStart());
                const auto lineEnd = lineStart +
                    static_cast<std::size_t>(line.textLength());
                for (const auto& image : visual->images) {
                    if (image.layout.placement ==
                            core::ImagePlacement::inline_with_text ||
                        !image.layout.move_with_text ||
                        image.layoutUtf16Offset < lineStart ||
                        image.layoutUtf16Offset >= lineEnd) {
                        continue;
                    }
                    movingAnchorXs.try_emplace(
                        image.id,
                        line.cursorToX(static_cast<int>(
                            image.layoutUtf16Offset)));
                }
                probeFirstLine = false;
            }
            probe.endLayout();
        }

        // `allowOverlap="0"` is emitted for this subset. Resolve every
        // supported anchor through one vertical cursor so mixed square and
        // top/bottom pictures cannot occupy the same rectangle merely because
        // their wrap kinds differ.
        int nonOverlapPage = -1;
        double nextAnchorY = 0.0;
        for (auto& image : visual->images) {
            if (image.layout.placement ==
                core::ImagePlacement::inline_with_text) {
                continue;
            }
            int anchorPage = page;
            double anchorY = image.layout.move_with_text ? y : 0.0;
            if (nonOverlapPage == anchorPage) {
                anchorY = std::max(anchorY, nextAnchorY);
            }
            if (image.layout.move_with_text &&
                anchorY + image.rect.height() > pageBottom &&
                anchorY > marginTopPoints_) {
                ++page;
                y = marginTopPoints_;
                anchorPage = page;
                anchorY = y;
                if (nonOverlapPage == anchorPage) {
                    anchorY = std::max(anchorY, nextAnchorY);
                }
            }
            image.pageIndex = anchorPage;
            const auto movingX = movingAnchorXs.find(image.id);
            const double anchorX = image.layout.move_with_text
                ? (movingX != movingAnchorXs.end()
                       ? movingX->second
                       : marginLeftPoints_ + leftIndent)
                : 0.0;
            image.rect.moveTo(
                anchorX, anchorY);
            if (nonOverlapPage != anchorPage) {
                nonOverlapPage = anchorPage;
                nextAnchorY = image.rect.bottom();
            } else {
                nextAnchorY = std::max(nextAnchorY, image.rect.bottom());
            }
        }

        const auto exclusionRect = [](const ParagraphImageVisual& image) {
            const double top = static_cast<double>(
                image.layout.distance_top_emu) / kEmuPerPoint;
            const double right = static_cast<double>(
                image.layout.distance_right_emu) / kEmuPerPoint;
            const double bottom = static_cast<double>(
                image.layout.distance_bottom_emu) / kEmuPerPoint;
            const double left = static_cast<double>(
                image.layout.distance_left_emu) / kEmuPerPoint;
            return image.rect.adjusted(-left, -top, right, bottom);
        };

        const auto advancePastTopAndBottom = [&] {
            while (true) {
                double exclusionBottom = y;
                for (const auto& image : visual->images) {
                    if (image.layout.placement !=
                            core::ImagePlacement::top_and_bottom ||
                        image.pageIndex != page) {
                        continue;
                    }
                    const QRectF exclusion = exclusionRect(image);
                    if (exclusion.bottom() > y) {
                        exclusionBottom = std::max(
                            exclusionBottom, exclusion.bottom());
                    }
                }
                y = exclusionBottom;
                if (y < pageBottom || y <= marginTopPoints_) break;
                ++page;
                y = marginTopPoints_;
            }
        };

        if (!finalLayoutText.isEmpty()) {
            visual->layout->beginLayout();
            while (true) {
                QTextLine line = visual->layout->createLine();
                if (!line.isValid()) {
                    break;
                }
                const double extra = semanticList
                    ? (firstLine ? listBulletOffset : listTextOffset)
                    : (firstLine ? firstIndent : 0.0);
                advancePastTopAndBottom();
                const auto squareWrapOffset = [&]() {
                    double rightEdge = marginLeftPoints_ + leftIndent;
                    double wrapBottom = y;
                    for (const auto& image : visual->images) {
                        if (image.layout.placement !=
                                core::ImagePlacement::square ||
                            image.pageIndex != page) {
                            continue;
                        }
                        const QRectF exclusion = exclusionRect(image);
                        if (y < exclusion.top() ||
                            y >= exclusion.bottom()) continue;
                        rightEdge = std::max(
                            rightEdge, exclusion.right());
                        wrapBottom = std::max(
                            wrapBottom, exclusion.bottom());
                    }
                    return std::pair{
                        std::max(0.0, rightEdge -
                                          (marginLeftPoints_ + leftIndent)),
                        wrapBottom};
                };
                auto [wrapOffset, wrapBottom] = squareWrapOffset();
                if (baseWidth - extra - wrapOffset < 18.0 &&
                    wrapBottom > y) {
                    y = wrapBottom;
                    if (y >= pageBottom) {
                        ++page;
                        y = marginTopPoints_;
                    }
                    advancePastTopAndBottom();
                    std::tie(wrapOffset, wrapBottom) = squareWrapOffset();
                }
                line.setLineWidth(
                    std::max(18.0, baseWidth - extra - wrapOffset));
                const auto measureLine = [&]() {
                    const auto lineStart = static_cast<std::size_t>(
                        line.textStart());
                    const auto lineEnd = lineStart +
                        static_cast<std::size_t>(line.textLength());
                    qreal equationAscent = 0.0;
                    qreal equationDescent = 0.0;
                    for (const auto& equation : visual->equations) {
                        if (equation.utf16Offset >= lineStart &&
                            equation.utf16Offset < lineEnd) {
                            equationAscent = std::max(
                                equationAscent, equation.metrics.ascent);
                            equationDescent = std::max(
                                equationDescent, equation.metrics.descent);
                        }
                    }
                    qreal imageAscent = 0.0;
                    for (const auto& image : visual->images) {
                        if (image.layout.placement ==
                                core::ImagePlacement::inline_with_text &&
                            image.layoutUtf16Offset >= lineStart &&
                            image.layoutUtf16Offset < lineEnd) {
                            imageAscent = std::max<qreal>(
                                imageAscent, image.rect.height());
                        }
                    }
                    const qreal contentAscent = std::max(
                        equationAscent, imageAscent);
                    const double topPadding = std::max<double>(
                        0.0, contentAscent - line.ascent());
                    const double naturalAdvance = std::max<double>(
                        line.height(),
                        std::max<qreal>(line.ascent(), contentAscent) +
                            std::max<qreal>(line.descent(), equationDescent) +
                            line.leading());
                    double advance = naturalAdvance;
                    if (paragraphFormat.line_spacing_emu) {
                        const double requested = emuToPoints(
                            paragraphFormat.line_spacing_emu);
                        switch (paragraphFormat.line_spacing_rule.value_or(
                            core::LineSpacingRule::automatic)) {
                            case core::LineSpacingRule::exact:
                                advance = requested;
                                break;
                            case core::LineSpacingRule::at_least:
                                advance = std::max(
                                    naturalAdvance, requested);
                                break;
                            case core::LineSpacingRule::automatic:
                                advance = std::max(
                                    1.0,
                                    naturalAdvance * requested / 12.0);
                                break;
                        }
                    }
                    return std::tuple{
                        lineStart, lineEnd, topPadding, advance};
                };
                auto [lineStart, lineEnd, topPadding, advance] =
                    measureLine();
                if (y + advance > pageBottom && y > marginTopPoints_) {
                    ++page;
                    y = marginTopPoints_;
                    advancePastTopAndBottom();
                    std::tie(wrapOffset, wrapBottom) = squareWrapOffset();
                    if (baseWidth - extra - wrapOffset < 18.0 &&
                        wrapBottom > y) {
                        y = wrapBottom;
                        if (y >= pageBottom) {
                            ++page;
                            y = marginTopPoints_;
                        }
                        advancePastTopAndBottom();
                        std::tie(wrapOffset, wrapBottom) =
                            squareWrapOffset();
                    }
                    line.setLineWidth(
                        std::max(18.0, baseWidth - extra - wrapOffset));
                    std::tie(lineStart, lineEnd, topPadding, advance) =
                        measureLine();
                }
                line.setPosition(QPointF(
                    marginLeftPoints_ + leftIndent + extra + wrapOffset,
                    y + topPadding));
                for (auto& image : visual->images) {
                    if (image.layout.placement !=
                            core::ImagePlacement::inline_with_text ||
                        image.layoutUtf16Offset < lineStart ||
                        image.layoutUtf16Offset >= lineEnd) {
                        continue;
                    }
                    const qreal x = line.cursorToX(
                        static_cast<int>(image.layoutUtf16Offset));
                    image.rect.moveTo(
                        x, line.y() + line.ascent() - image.rect.height());
                    image.pageIndex = page;
                }
                visual->lines.push_back({line, page, paragraphIndexValue});
                y += advance;
                firstLine = false;
            }
            visual->layout->endLayout();
        }
        if (visual->lines.empty() && visual->images.empty()) {
            y += 14.0;
        }
        double contentBottom = y;
        for (const auto& image : visual->images) {
            if (image.pageIndex != page) continue;
            contentBottom = std::max(
                contentBottom,
                exclusionRect(image).bottom());
        }
        y = contentBottom;
        // No implicit paragraph gap: an unspecified format is true single
        // spacing. Imported or explicitly selected paragraph spacing remains
        // represented by space_after_emu above.
        y += emuToPoints(paragraphFormat.space_after_emu);
        int firstPage = initialPage;
        double top = initialY;
        if (!visual->lines.empty()) {
            firstPage = visual->lines.front().pageIndex;
            top = visual->lines.front().line.y();
        }
        for (const auto& image : visual->images) {
            if (image.pageIndex < firstPage) {
                firstPage = image.pageIndex;
                top = image.rect.top();
            } else if (image.pageIndex == firstPage) {
                top = std::min(top, image.rect.top());
            }
        }
        int lastPage = page;
        if (!visual->lines.empty()) lastPage = visual->lines.back().pageIndex;
        for (const auto& image : visual->images) {
            lastPage = std::max(lastPage, image.pageIndex);
        }
        const double blockBottom = contentBottom;
        blockPlacements_.push_back({paragraph.id(), core::BodyBlockKind::paragraph,
                                    firstPage, lastPage, top, blockBottom});
        visuals_.push_back(std::move(visual));
    };

    const auto layoutTable = [&](const core::Table& table) {
        constexpr double kCellHorizontalPadding = 5.0;
        constexpr double kCellVerticalPadding = 4.0;
        constexpr double kMinimumRowHeight = 22.0;
        const double availableWidth = std::max(
            36.0, pageWidthPoints_ - marginLeftPoints_ - marginRightPoints_);
        const auto imported = std::find_if(
            importedTables_.begin(), importedTables_.end(),
            [&table](const ImportedTablePresentation& presentation) {
                return presentation.tableId == table.id();
            });
        std::vector<const ImportedTableCellPresentation*> cellPresentations(
            table.cells().size(), nullptr);
        if (imported != importedTables_.end() &&
            !imported->cellIds.empty() &&
            imported->cellIds.size() == imported->cells.size()) {
            std::unordered_map<
                core::NodeId, const ImportedTableCellPresentation*,
                core::NodeIdHash>
                presentationsById;
            presentationsById.reserve(imported->cellIds.size());
            bool identitiesAreUnique = true;
            for (std::size_t index = 0;
                 index < imported->cellIds.size(); ++index) {
                if (!presentationsById.emplace(
                         imported->cellIds[index], &imported->cells[index])
                         .second) {
                    identitiesAreUnique = false;
                    break;
                }
            }
            if (identitiesAreUnique) {
                for (std::size_t index = 0; index < table.cells().size();
                     ++index) {
                    const auto found = presentationsById.find(
                        table.cells()[index].id);
                    if (found != presentationsById.end()) {
                        cellPresentations[index] = found->second;
                    }
                }
            }
        } else if (imported != importedTables_.end() &&
                   imported->cellIds.empty() &&
                   imported->cells.size() == table.cells().size()) {
            // Legacy presentation records without cell identities remain safe
            // only while their ordinal shape is unchanged.
            for (std::size_t index = 0; index < table.cells().size(); ++index) {
                cellPresentations[index] = &imported->cells[index];
            }
        }
        const bool hasImportedPresentation = std::any_of(
            cellPresentations.begin(), cellPresentations.end(),
            [](const ImportedTableCellPresentation* cell) {
                return cell != nullptr;
            });
        TableStylePalette semanticPalette;
        const bool hasSemanticPalette = table.style().has_value();
        if (hasSemanticPalette) {
            semanticPalette = tableStylePalette(*table.style());
        }
        std::vector<double> columnWidths(table.columnCount());
        if (imported != importedTables_.end() &&
            imported->columnWidthsPoints.size() == table.columnCount() &&
            std::all_of(imported->columnWidthsPoints.begin(),
                        imported->columnWidthsPoints.end(),
                        [](double width) { return width > 0.0; })) {
            const double requested = std::accumulate(
                imported->columnWidthsPoints.begin(),
                imported->columnWidthsPoints.end(), 0.0);
            const double widthScale = requested > availableWidth
                ? availableWidth / requested
                : 1.0;
            std::transform(
                imported->columnWidthsPoints.begin(),
                imported->columnWidthsPoints.end(), columnWidths.begin(),
                [widthScale](double width) { return width * widthScale; });
        } else {
            std::fill(
                columnWidths.begin(), columnWidths.end(),
                availableWidth / static_cast<double>(table.columnCount()));
        }
        const double tableWidth = std::accumulate(
            columnWidths.begin(), columnWidths.end(), 0.0);
        double tableLeft = marginLeftPoints_;
        if (imported != importedTables_.end() && imported->alignment) {
            if (*imported->alignment == core::ParagraphAlignment::center) {
                tableLeft += (availableWidth - tableWidth) / 2.0;
            } else if (*imported->alignment == core::ParagraphAlignment::right) {
                tableLeft += availableWidth - tableWidth;
            }
        }
        std::vector<double> columnLefts(table.columnCount(), tableLeft);
        for (std::size_t column = 1; column < table.columnCount(); ++column) {
            columnLefts[column] = columnLefts[column - 1] +
                                  columnWidths[column - 1];
        }
        auto visual = std::make_unique<TableVisual>();
        visual->id = table.id();
        visual->rows = table.rowCount();
        visual->columns = table.columnCount();
        visual->headerRow = table.hasHeaderRow();
        visual->hasImportedPresentation = hasImportedPresentation;
        visual->hasSemanticStyle = hasSemanticPalette;
        visual->cells.reserve(table.cells().size());
        visual->rowVisuals.reserve(table.rowCount());
        int firstPage = page;
        int lastPage = page;
        double firstTop = y;
        double lastBottom = y;

        for (std::size_t row = 0; row < table.rowCount(); ++row) {
            std::vector<TableCellVisual> rowCells;
            rowCells.reserve(table.columnCount());
            const auto rowPresentationStart = row * table.columnCount();
            const bool rowHasCompleteImportedPresentation = std::all_of(
                cellPresentations.begin() + static_cast<std::ptrdiff_t>(
                    rowPresentationStart),
                cellPresentations.begin() + static_cast<std::ptrdiff_t>(
                    rowPresentationStart + table.columnCount()),
                [](const ImportedTableCellPresentation* cell) {
                    return cell != nullptr;
                });
            double rowHeight = rowHasCompleteImportedPresentation
                ? 1.0
                : kMinimumRowHeight;
            for (std::size_t column = 0; column < table.columnCount(); ++column) {
                const auto* sourceCell = table.cell(row, column);
                const auto presentationIndex = row * table.columnCount() + column;
                const ImportedTableCellPresentation* presentation =
                    cellPresentations[presentationIndex];
                TableCellVisual cell;
                cell.row = row;
                cell.column = column;
                cell.text = sourceCell ? fromUtf16(sourceCell->text) : QString();
                core::CharacterFormat format;
                format.font_family = defaultFontFamily_.toStdString();
                format.font_size_half_points = static_cast<std::int32_t>(
                    std::lround(defaultFontPointSize_ * 2.0));
                const bool styledHeader = hasSemanticPalette &&
                    table.hasHeaderRow() && row == 0;
                format.bold = styledHeader ||
                    (!hasSemanticPalette && !presentation &&
                     table.hasHeaderRow() && row == 0);
                if (styledHeader) {
                    format.foreground_argb =
                        semanticPalette.headerTextArgb;
                }
                if (sourceCell) {
                    format = resolvedCharacterFormat(
                        std::move(format),
                        sourceCell->default_character_format);
                }
                const QString cellLayoutText = cell.text.isEmpty()
                    ? QString(QChar(0x2060))
                    : cell.text;
                cell.layout = std::make_unique<QTextLayout>(
                    cellLayoutText, fontFrom(format, defaultFontFamily_,
                                              defaultFontPointSize_));
                const bool usesSemanticRuns = sourceCell &&
                    !sourceCell->character_formats.empty();
                const bool usesImportedRuns = !usesSemanticRuns &&
                    presentation && presentation->sourceText == cell.text;
                const std::vector<core::FormatRun>* layoutRuns =
                    usesSemanticRuns
                    ? &sourceCell->character_formats
                    : (usesImportedRuns ? &presentation->formats : nullptr);
                {
                    QList<QTextLayout::FormatRange> ranges;
                    QTextLayout::FormatRange baseRange;
                    baseRange.start = 0;
                    baseRange.length = static_cast<int>(std::min<qsizetype>(
                        cellLayoutText.size(),
                        std::numeric_limits<int>::max()));
                    baseRange.format = qtFormat(
                        format, defaultFontFamily_, defaultFontPointSize_);
                    ranges.push_back(baseRange);
                    if (layoutRuns) {
                    for (const auto& run : *layoutRuns) {
                        const auto start = std::min(
                            run.start, static_cast<std::size_t>(cell.text.size()));
                        const auto end = std::min(
                            run.end, static_cast<std::size_t>(cell.text.size()));
                        if (end <= start) continue;
                        QTextLayout::FormatRange range;
                        range.start = static_cast<int>(start);
                        range.length = static_cast<int>(end - start);
                        range.format = qtFormat(
                            resolvedCharacterFormat(format, run.format),
                            defaultFontFamily_,
                            defaultFontPointSize_);
                        ranges.push_back(range);
                    }
                    }
                    cell.layout->setFormats(ranges);
                }
                cell.paddingTop = kCellVerticalPadding;
                cell.paddingRight = kCellHorizontalPadding;
                cell.paddingBottom = kCellVerticalPadding;
                cell.paddingLeft = kCellHorizontalPadding;
                if (hasSemanticPalette) {
                    cell.fillArgb = tableStyleCellFill(
                        semanticPalette, table.hasHeaderRow(), row);
                    const ImportedCellBorderPresentation border{
                        semanticPalette.borderArgb,
                        semanticPalette.borderWidthPoints};
                    cell.borderTop = border;
                    cell.borderRight = border;
                    cell.borderBottom = border;
                    cell.borderLeft = border;
                }
                if (presentation) {
                    cell.hasImportedPresentation = true;
                    cell.paddingTop = presentation->paddingTopPoints;
                    cell.paddingRight = presentation->paddingRightPoints;
                    cell.paddingBottom = presentation->paddingBottomPoints;
                    cell.paddingLeft = presentation->paddingLeftPoints;
                    cell.verticalAlignment = presentation->verticalAlignment;
                    // A table style is the base. Direct imported cell
                    // properties have the higher OOXML precedence and replace
                    // only the sides/properties that were explicitly present.
                    if (presentation->fillArgb) {
                        cell.fillArgb = presentation->fillArgb;
                    }
                    if (presentation->borderTop) {
                        cell.borderTop = presentation->borderTop;
                    }
                    if (presentation->borderRight) {
                        cell.borderRight = presentation->borderRight;
                    }
                    if (presentation->borderBottom) {
                        cell.borderBottom = presentation->borderBottom;
                    }
                    if (presentation->borderLeft) {
                        cell.borderLeft = presentation->borderLeft;
                    }
                }
                QTextOption option;
                option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                option.setUseDesignMetrics(true);
                core::ParagraphFormat cellParagraphFormat = sourceCell
                    ? sourceCell->paragraph_format
                    : core::ParagraphFormat{};
                if (!cellParagraphFormat.alignment && presentation) {
                    cellParagraphFormat.alignment = presentation->alignment;
                }
                option.setAlignment(paragraphAlignment(cellParagraphFormat));
                option.setTabStopDistance(std::max<qreal>(
                    1.0, QFontMetricsF(fontFrom(
                             format, defaultFontFamily_, defaultFontPointSize_))
                             .horizontalAdvance(QLatin1Char(' ')) *
                             static_cast<qreal>(tabWidthSpaces_)));
                cell.layout->setTextOption(option);
                const auto lineAdvance = [&](const QTextLine& line) {
                    double natural = 0.0;
                    bool foundIntersectingRun = false;
                    std::map<int, double> requestedPointsByPixelSize;
                    const auto includeFormat = [&](
                                                   const core::CharacterFormat&
                                                       sourceFormat) {
                        const QFont realized = fontFrom(
                            sourceFormat, defaultFontFamily_,
                            defaultFontPointSize_);
                        const QRawFont raw = QRawFont::fromFont(realized);
                        const double realizedPixels = raw.isValid()
                            ? raw.pixelSize()
                            : static_cast<double>(realized.pixelSize());
                        if (std::isfinite(realizedPixels) &&
                            realizedPixels > 0.0) {
                            const int key = static_cast<int>(
                                std::lround(realizedPixels));
                            requestedPointsByPixelSize[key] = std::max(
                                requestedPointsByPixelSize[key],
                                requestedPointSize(
                                    sourceFormat, defaultFontPointSize_));
                        }
                    };
                    if (layoutRuns) {
                        const std::size_t lineStart = static_cast<std::size_t>(
                            std::max(0, line.textStart()));
                        const std::size_t lineEnd = lineStart +
                            static_cast<std::size_t>(
                                std::max(0, line.textLength()));
                        for (const auto& run : *layoutRuns) {
                            if (run.end <= lineStart || run.start >= lineEnd) {
                                continue;
                            }
                            foundIntersectingRun = true;
                            const auto resolved = resolvedCharacterFormat(
                                format, run.format);
                            includeFormat(resolved);
                            natural = std::max(
                                natural, unroundedLineAdvance(
                                             resolved, defaultFontFamily_,
                                             defaultFontPointSize_));
                        }
                    }
                    if (!foundIntersectingRun) {
                        includeFormat(format);
                        natural = unroundedLineAdvance(
                            format, defaultFontFamily_, defaultFontPointSize_);
                    }

                    // QTextLayout performs font fallback while shaping. A
                    // CJK/Indic/RTL fallback can have taller OpenType metrics
                    // than the requested Latin family, so primary-font-only
                    // row measurement can clip otherwise valid glyphs. Use
                    // every font that actually contributed glyphs to this
                    // line, while normalizing the realized whole-pixel Qt
                    // font back to the exact OOXML half-point request.
                    for (const QGlyphRun& glyphRun : line.glyphRuns()) {
                        const QRawFont raw = glyphRun.rawFont();
                        if (!raw.isValid() || raw.pixelSize() <= 0.0) continue;
                        const double designAdvance =
                            raw.ascent() + raw.descent() + raw.leading();
                        if (!std::isfinite(designAdvance) ||
                            designAdvance <= 0.0) {
                            continue;
                        }
                        const int key = static_cast<int>(
                            std::lround(raw.pixelSize()));
                        const auto requested =
                            requestedPointsByPixelSize.find(key);
                        const double requestedPoints =
                            requested != requestedPointsByPixelSize.end()
                            ? requested->second
                            : raw.pixelSize();
                        natural = std::max(
                            natural,
                            designAdvance * requestedPoints /
                                raw.pixelSize());
                    }
                    if (cellParagraphFormat.line_spacing_emu) {
                        const double requested = emuToPoints(
                            cellParagraphFormat.line_spacing_emu);
                        switch (cellParagraphFormat.line_spacing_rule.value_or(
                            core::LineSpacingRule::automatic)) {
                            case core::LineSpacingRule::automatic:
                                return std::max(
                                    1.0, natural * requested / 12.0);
                            case core::LineSpacingRule::at_least:
                                return std::max(natural, requested);
                            case core::LineSpacingRule::exact:
                                return std::max(1.0, requested);
                        }
                    }
                    if (!presentation || !presentation->lineSpacing) {
                        return natural;
                    }
                    switch (presentation->lineSpacingRule.value_or(
                        core::LineSpacingRule::automatic)) {
                        case core::LineSpacingRule::automatic:
                            return std::max(
                                1.0, natural *
                                         static_cast<double>(
                                             *presentation->lineSpacing) /
                                         240.0);
                        case core::LineSpacingRule::at_least:
                            return std::max(
                                natural,
                                static_cast<double>(
                                    *presentation->lineSpacing) /
                                    20.0);
                        case core::LineSpacingRule::exact:
                            return std::max(
                                1.0,
                                static_cast<double>(
                                    *presentation->lineSpacing) /
                                    20.0);
                    }
                    return natural;
                };
                double relativeY = cellParagraphFormat.space_before_emu
                    ? emuToPoints(cellParagraphFormat.space_before_emu)
                    : (presentation ? presentation->spaceBeforePoints : 0.0);
                cell.layout->beginLayout();
                while (true) {
                    QTextLine line = cell.layout->createLine();
                    if (!line.isValid()) break;
                    line.setLineWidth(std::max(
                        8.0, columnWidths[column] - cell.paddingLeft -
                                 cell.paddingRight));
                    line.setPosition(QPointF(0.0, relativeY));
                    relativeY += lineAdvance(line);
                    cell.lines.push_back(line);
                }
                cell.layout->endLayout();
                if (cellParagraphFormat.space_after_emu) {
                    relativeY += emuToPoints(
                        cellParagraphFormat.space_after_emu);
                } else if (presentation) {
                    relativeY += presentation->spaceAfterPoints;
                }
                cell.contentHeight = relativeY;
                const double topBorderExtent = cell.borderTop
                    ? cell.borderTop->widthPoints / 2.0
                    : 0.0;
                const double bottomBorderExtent = cell.borderBottom
                    ? cell.borderBottom->widthPoints / 2.0
                    : 0.0;
                rowHeight = std::max(
                    rowHeight, relativeY + cell.paddingTop +
                                   cell.paddingBottom + topBorderExtent +
                                   bottomBorderExtent);
                rowCells.push_back(std::move(cell));
            }

            if (y + rowHeight > pageBottom && y > marginTopPoints_) {
                ++page;
                y = marginTopPoints_;
            }
            if (row == 0) {
                firstPage = page;
                firstTop = y;
                visual->handlePageIndex = page;
                visual->handleRect = QRectF(
                    std::max(2.0, tableLeft - 13.0), y, 11.0, 11.0);
            }
            visual->rowVisuals.push_back(
                {QRectF(tableLeft, y, tableWidth, rowHeight), page});
            for (std::size_t column = 0; column < rowCells.size(); ++column) {
                auto& cell = rowCells[column];
                const double cellLeft = columnLefts[column];
                cell.rect = QRectF(
                    cellLeft, y, columnWidths[column], rowHeight);
                cell.pageIndex = page;
                const double topBorderExtent = cell.borderTop
                    ? cell.borderTop->widthPoints / 2.0
                    : 0.0;
                const double bottomBorderExtent = cell.borderBottom
                    ? cell.borderBottom->widthPoints / 2.0
                    : 0.0;
                const double spareHeight = std::max(
                    0.0, rowHeight - cell.paddingTop -
                             cell.paddingBottom - topBorderExtent -
                             bottomBorderExtent - cell.contentHeight);
                double verticalOffset = 0.0;
                if (cell.verticalAlignment ==
                    ImportedCellVerticalAlignment::center) {
                    verticalOffset = spareHeight / 2.0;
                } else if (cell.verticalAlignment ==
                           ImportedCellVerticalAlignment::bottom) {
                    verticalOffset = spareHeight;
                }
                for (auto& line : cell.lines) {
                    line.setPosition(QPointF(
                        cellLeft + cell.paddingLeft,
                        y + topBorderExtent + cell.paddingTop +
                            verticalOffset + line.y()));
                }
                visual->cells.push_back(std::move(cell));
            }
            y += rowHeight;
            lastPage = page;
            lastBottom = y;
        }
        blockPlacements_.push_back({table.id(), core::BodyBlockKind::table,
                                    firstPage, lastPage, firstTop, lastBottom});
        tableVisuals_.push_back(std::move(visual));
    };

    for (const auto& block : snap.document.bodyBlocks()) {
        if (block.kind == core::BodyBlockKind::paragraph) {
            const auto paragraphIndex = snap.document.paragraphIndex(block.id);
            const auto* paragraph = snap.document.findParagraph(block.id);
            if (paragraph && paragraphIndex) {
                layoutParagraph(*paragraph, *paragraphIndex);
            }
        } else if (const auto* table = snap.document.findTable(block.id)) {
            layoutTable(*table);
        }
    }

    pageCount_ = std::max(1, page + 1);
    layoutRevision_ = snap.revision;
    layoutIsPreview_ = preview;
    layoutValid_ = true;
    ++layoutGeneration_;
    updateScrollBars();
}

core::DocumentSnapshot DocumentCanvas::visibleDocumentSnapshot() const {
    if (previewId_) {
        const auto preview = session_->previewSnapshot(*previewId_);
        if (preview) {
            return {preview.value().revision, preview.value().document};
        }
    }
    return session_->snapshot();
}

void DocumentCanvas::updateScrollBars() const {
    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    const int contentHeight = static_cast<int>(std::ceil(
        kCanvasPaddingPixels * 2 + pageCount_ * pageHeightPoints_ * scale +
        std::max(0, pageCount_ - 1) * kPageGapPixels));
    const int contentWidth = static_cast<int>(std::ceil(
        kCanvasPaddingPixels * 2 + pageWidthPoints_ * scale));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setRange(0, std::max(0, contentHeight - viewport()->height()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setRange(0, std::max(0, contentWidth - viewport()->width()));
}

void DocumentCanvas::paintEvent(QPaintEvent*) {
    ensureLayout();
    updateStoryEditorGeometry();
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor(QStringLiteral("#e8eaed")));
    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    const double pagePixelWidth = pageWidthPoints_ * scale;
    const double documentWidth = std::max(pagePixelWidth + 2 * kCanvasPaddingPixels,
                                          static_cast<double>(viewport()->width()));
    const double left = (documentWidth - pagePixelWidth) / 2.0 - horizontalScrollBar()->value();

    for (int page = 0; page < pageCount_; ++page) {
        const double top = kCanvasPaddingPixels + page * (pageHeightPoints_ * scale + kPageGapPixels) -
                           verticalScrollBar()->value();
        QRectF pageRect(left, top, pagePixelWidth, pageHeightPoints_ * scale);
        if (!pageRect.intersects(viewport()->rect())) {
            continue;
        }
        painter.fillRect(pageRect.translated(3, 4), QColor(0, 0, 0, 38));
        painter.fillRect(pageRect, Qt::white);
        painter.setPen(previewId_ ? QColor(QStringLiteral("#2f80ed"))
                                  : QColor(QStringLiteral("#c9cdd2")));
        painter.drawRect(pageRect);
        renderPage(painter, page, pageRect.topLeft(), scale, true);
        if (previewId_ && page == 0) {
            const QRectF badge(pageRect.left() + 10.0, pageRect.top() + 10.0,
                               160.0, 24.0);
            painter.fillRect(badge, QColor(QStringLiteral("#eaf4ff")));
            painter.setPen(QColor(QStringLiteral("#165d9c")));
            painter.drawText(badge, Qt::AlignCenter, tr("CODEX PREVIEW — NOT APPLIED"));
        }
    }
}

void DocumentCanvas::renderPage(QPainter& painter, int pageIndexValue,
                                const QPointF& origin, double scale, bool decorations) const {
    const auto snap = visibleDocumentSnapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    const auto selectedImage = selectedInlineImageId();
    painter.save();
    painter.translate(origin);
    painter.scale(scale, scale);
    painter.setClipRect(QRectF(0, 0, pageWidthPoints_, pageHeightPoints_));
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    // Always paint from the cached full-resolution decode. Qt otherwise uses
    // its fast raster transform for interactive page scaling, which makes
    // photographs and screenshots visibly degrade while they are resized or
    // viewed at non-integral zoom levels. This does not rewrite source bytes.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QFont storyFont(defaultFontFamily_);
    storyFont.setPointSizeF(defaultFontPointSize_);
    painter.setFont(storyFont);
    painter.setPen(Qt::black);
    const double storyLeft = marginLeftPoints_;
    const double storyWidth = std::max(
        1.0, pageWidthPoints_ - marginLeftPoints_ - marginRightPoints_);
    const auto drawStory = [&](const std::u16string& source,
                               const std::vector<core::ImageAtom>& images,
                               bool footer) {
        const auto sections = splitStorySections(fromUtf16(source));
        const double top = footer
            ? pageHeightPoints_ - marginBottomPoints_ + 9.0 : 18.0;
        const double height = footer
            ? std::max(12.0, marginBottomPoints_ - 18.0)
            : std::max(12.0, marginTopPoints_ - 27.0);
        for (int index = 0; index < 3; ++index) {
            const QRectF rect(
                storyLeft + storyWidth * static_cast<double>(index) / 3.0,
                top, storyWidth / 3.0, height);
            QString text = sections[static_cast<std::size_t>(index)];
            text.replace(QStringLiteral("{PAGE}"),
                         QString::number(pageIndexValue + 1),
                         Qt::CaseInsensitive);
            text.replace(QStringLiteral("{PAGES}"),
                         QString::number(pageCount_), Qt::CaseInsensitive);
            std::vector<const core::ImageAtom*> regionImages;
            for (const auto& image : images) {
                int imageRegion = 0;
                for (std::size_t offset = 0;
                     offset < image.utf16_offset && offset < source.size();
                     ++offset) {
                    if (source[offset] == u'\t') {
                        imageRegion = std::min(2, imageRegion + 1);
                    }
                }
                if (imageRegion == index) regionImages.push_back(&image);
            }
            const QStringList parts = text.split(
                QChar(core::kInlineObjectReplacementCharacter),
                Qt::KeepEmptyParts);
            const QFontMetricsF metrics(painter.font());
            std::vector<double> imageWidths;
            std::vector<double> imageHeights;
            double totalWidth = 0.0;
            for (int part = 0; part < parts.size(); ++part) {
                totalWidth += metrics.horizontalAdvance(parts[part]);
                if (part >= static_cast<int>(regionImages.size())) continue;
                const auto* image = regionImages[static_cast<std::size_t>(part)];
                const double naturalWidth =
                    static_cast<double>(image->width_emu) / kEmuPerPoint;
                const double naturalHeight =
                    static_cast<double>(image->height_emu) / kEmuPerPoint;
                const double fit = std::min(
                    {1.0, rect.height() / std::max(1.0, naturalHeight),
                     rect.width() / std::max(1.0, naturalWidth)});
                imageWidths.push_back(naturalWidth * fit);
                imageHeights.push_back(naturalHeight * fit);
                totalWidth += imageWidths.back();
            }
            double x = index == 0 ? rect.left()
                : index == 1 ? rect.center().x() - totalWidth / 2.0
                             : rect.right() - totalWidth;
            x = std::max(rect.left(), x);
            std::size_t imageIndex = 0;
            for (int part = 0; part < parts.size(); ++part) {
                const QString& fragment = parts[part];
                const double textWidth = metrics.horizontalAdvance(fragment);
                const double textY = footer
                    ? rect.bottom() - metrics.descent()
                    : rect.top() + metrics.ascent();
                painter.drawText(QPointF(x, textY), fragment);
                x += textWidth;
                if (part >= static_cast<int>(regionImages.size())) continue;
                const auto* image = regionImages[imageIndex];
                const QImage* decoded = decodedInlineImage(*image);
                const double imageWidth = imageWidths[imageIndex];
                const double imageHeight = imageHeights[imageIndex];
                const double imageY = footer
                    ? rect.bottom() - imageHeight : rect.top();
                if (decoded) {
                    painter.drawImage(
                        QRectF(x, imageY, imageWidth, imageHeight), *decoded);
                }
                x += imageWidth;
                ++imageIndex;
            }
        }
    };
    if (!snap.document.headerText().empty() ||
        !snap.document.headerImages().empty()) {
        drawStory(snap.document.headerText(),
                  snap.document.headerImages(), false);
    }
    if (!snap.document.footerText().empty() ||
        !snap.document.footerImages().empty()) {
        drawStory(snap.document.footerText(),
                  snap.document.footerImages(), true);
    }
    if (decorations && headerFooterEditing_) {
        painter.save();
        painter.setPen(QPen(QColor(QStringLiteral("#7b8794")), 0.8,
                            Qt::DashLine));
        for (const bool footer : {false, true}) {
            const double top = footer
                ? pageHeightPoints_ - marginBottomPoints_ + 7.0 : 14.0;
            const double height = footer
                ? std::max(18.0, marginBottomPoints_ - 14.0)
                : std::max(18.0, marginTopPoints_ - 21.0);
            for (int index = 0; index < 3; ++index) {
                painter.drawRect(QRectF(
                    storyLeft + storyWidth * static_cast<double>(index) / 3.0,
                    top, storyWidth / 3.0, height));
            }
        }
        painter.restore();
    }

    for (const auto& tableVisual : tableVisuals_) {
        const auto& table = *tableVisual;
        const bool tableObjectSelected =
            decorations && selectedTable_ && *selectedTable_ == table.id &&
            !tableCursor_ && !tableCellSelection_;
        for (const auto& cell : table.cells) {
            if (cell.pageIndex != pageIndexValue) continue;
            if (cell.fillArgb) {
                painter.fillRect(cell.rect, fromArgb(*cell.fillArgb));
            }
            const bool selectedCell = decorations &&
                tableCellIsSelected(table.id, cell.row, cell.column);
            if (tableObjectSelected || selectedCell) {
                painter.fillRect(cell.rect, QColor(51, 132, 255, 28));
            }
            std::optional<std::pair<std::size_t, std::size_t>>
                activeCellTextSelection;
            if (decorations && tableCursor_ &&
                tableCursor_->tableId == table.id &&
                tableCursor_->row == cell.row &&
                tableCursor_->column == cell.column) {
                const auto focus = std::min<std::size_t>(
                    tableCursor_->utf16Offset,
                    static_cast<std::size_t>(cell.text.size()));
                const auto anchor = std::min<std::size_t>(
                    tableSelectionAnchor_.value_or(focus),
                    static_cast<std::size_t>(cell.text.size()));
                const auto selectionStart = std::min(anchor, focus);
                const auto selectionEnd = std::max(anchor, focus);
                if (selectionStart != selectionEnd) {
                    activeCellTextSelection =
                        std::pair{selectionStart, selectionEnd};
                }
            }
            const bool hasCellBorders = cell.borderTop || cell.borderRight ||
                                        cell.borderBottom || cell.borderLeft;
            if (!hasCellBorders && !cell.hasImportedPresentation) {
                painter.setPen(QPen(
                    QColor(QStringLiteral("#777777")), 0.6));
                painter.drawRect(cell.rect);
            } else {
                const auto drawBorder = [&painter](
                    const std::optional<ImportedCellBorderPresentation>& border,
                    const QLineF& line) {
                    if (!border) return;
                    painter.setPen(QPen(
                        fromArgb(border->argb), border->widthPoints));
                    painter.drawLine(line);
                };
                drawBorder(cell.borderTop,
                           QLineF(cell.rect.topLeft(), cell.rect.topRight()));
                drawBorder(cell.borderRight,
                           QLineF(cell.rect.topRight(), cell.rect.bottomRight()));
                drawBorder(cell.borderBottom,
                           QLineF(cell.rect.bottomLeft(), cell.rect.bottomRight()));
                drawBorder(cell.borderLeft,
                           QLineF(cell.rect.topLeft(), cell.rect.bottomLeft()));
            }
            if (tableObjectSelected || selectedCell) {
                painter.setPen(QPen(QColor(QStringLiteral("#2f80ed")), 1.2));
                painter.drawRect(cell.rect);
            }
            const auto cellSpellingWords =
                decorations && spelling_.available()
                    ? spellingWords(cell.text)
                    : std::vector<SpellingWord>{};
            for (const auto& line : cell.lines) {
                // Spellcheck decorations change the painter pen. Reset it for
                // every line so unformatted glyphs keep their normal color.
                painter.setPen(Qt::black);
                if (activeCellTextSelection) {
                    const auto lineStart = static_cast<std::size_t>(
                        std::max(0, line.textStart()));
                    const auto lineEnd = lineStart +
                        static_cast<std::size_t>(
                            std::max(0, line.textLength()));
                    const auto start = std::max(
                        activeCellTextSelection->first, lineStart);
                    const auto end = std::min(
                        activeCellTextSelection->second, lineEnd);
                    if (start < end) {
                        fillSelectionBackground(
                            painter, line, static_cast<int>(start),
                            static_cast<int>(end));
                    }
                }
                line.draw(&painter, QPointF());
                if (!cellSpellingWords.empty()) {
                    const auto spellingLineStart = static_cast<std::size_t>(
                        std::max(0, line.textStart()));
                    const auto spellingLineEnd = spellingLineStart +
                        static_cast<std::size_t>(
                            std::max(0, line.textLength()));
                    QPen misspelling(
                        QColor(QStringLiteral("#d92d20")));
                    misspelling.setWidthF(0.8);
                    painter.setPen(misspelling);
                    for (const auto& word : cellSpellingWords) {
                        if (word.end <= spellingLineStart ||
                            word.start >= spellingLineEnd ||
                            spelling_.isCorrect(word.text) ||
                            suppressSpellingWord(
                                PendingSpellingWord::Container::tableCell,
                                table.id, cell.row, cell.column,
                                word.start, word.end)) {
                            continue;
                        }
                        const auto start = std::max(
                            word.start, spellingLineStart);
                        const auto end = std::min(
                            word.end, spellingLineEnd);
                        const qreal x1 = line.cursorToX(
                            static_cast<int>(start));
                        const qreal x2 = line.cursorToX(
                            static_cast<int>(end));
                        const qreal underlineY =
                            line.y() + line.height() - 1.0;
                        painter.drawLine(
                            QPointF(std::min(x1, x2), underlineY),
                            QPointF(std::max(x1, x2), underlineY));
                    }
                }
            }
        }

        if (decorations && table.handlePageIndex == pageIndexValue) {
            painter.fillRect(table.handleRect,
                             tableObjectSelected
                                 ? QColor(QStringLiteral("#2f80ed"))
                                 : QColor(QStringLiteral("#5e2750")));
            painter.setPen(QPen(Qt::white, 0.8));
            const QPointF center = table.handleRect.center();
            painter.drawLine(QPointF(table.handleRect.left() + 2.0, center.y()),
                             QPointF(table.handleRect.right() - 2.0, center.y()));
            painter.drawLine(QPointF(center.x(), table.handleRect.top() + 2.0),
                             QPointF(center.x(), table.handleRect.bottom() - 2.0));
        }
    }

    for (const auto& paragraphVisual : visuals_) {
        const auto& paragraph = *paragraphVisual;
        const auto paragraphSpellingWords =
            decorations && spelling_.available()
                ? spellingWords(paragraph.text)
                : std::vector<SpellingWord>{};
        for (const auto& image : paragraph.images) {
            if (image.pageIndex == pageIndexValue && !image.image.isNull()) {
                const QRectF renderedRect = imageResizeDrag_ &&
                        imageResizeDrag_->imageId == image.id &&
                        imageResizeDrag_->pageIndex == pageIndexValue
                    ? imageResizeDrag_->previewRect
                    : image.rect;
                painter.drawImage(renderedRect, image.image);
                if (decorations && selectedImage &&
                    image.id == *selectedImage) {
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(QPen(
                        QColor(QStringLiteral("#2f80ed")), 1.2));
                    painter.drawRect(renderedRect);
                    constexpr double kHandleSize = 5.0;
                    painter.setBrush(QColor(QStringLiteral("#2f80ed")));
                    const auto drawHandle =
                        [&painter, kHandleSize](const QPointF& center) {
                        painter.drawRect(QRectF(
                            center.x() - kHandleSize / 2.0,
                            center.y() - kHandleSize / 2.0,
                            kHandleSize, kHandleSize));
                    };
                    // Picture Layout v1 stores dimensions but not a movable
                    // image origin. Expose only handles whose preview can be
                    // committed without the image jumping back on release.
                    drawHandle(QPointF(renderedRect.right(),
                                       renderedRect.center().y()));
                    drawHandle(renderedRect.bottomRight());
                    drawHandle(QPointF(renderedRect.center().x(),
                                       renderedRect.bottom()));
                    painter.setBrush(Qt::NoBrush);
                }
            }
        }
        for (const auto& visualLine : paragraph.lines) {
            if (visualLine.pageIndex != pageIndexValue) {
                continue;
            }
            if (decorations && normalized && !normalized.value().empty()) {
                const auto& range = normalized.value();
                if (visualLine.paragraphIndex >= range.start_paragraph_index &&
                    visualLine.paragraphIndex <= range.end_paragraph_index) {
                    int start = visualLine.line.textStart();
                    int end = start + visualLine.line.textLength();
                    if (visualLine.paragraphIndex == range.start_paragraph_index) {
                        start = std::max(
                            start, paragraph.layoutOffsetForCore(
                                       range.start.utf16_offset));
                    }
                    if (visualLine.paragraphIndex == range.end_paragraph_index) {
                        end = std::min(
                            end, paragraph.layoutOffsetForCore(
                                     range.end.utf16_offset));
                    }
                    if (end > start) {
                        fillSelectionBackground(
                            painter, visualLine.line, start, end);
                    }
                }
            }
            // Unformatted QTextLayout glyphs use the current painter pen.
            // Reset it for every line because spellcheck decoration below can
            // leave the pen red before the next line is drawn.
            painter.setPen(Qt::black);
            visualLine.line.draw(&painter, QPointF());

            const auto lineStart = static_cast<std::size_t>(
                visualLine.line.textStart());
            const auto lineEnd = lineStart + static_cast<std::size_t>(
                visualLine.line.textLength());
            for (const auto& equation : paragraph.equations) {
                if (equation.utf16Offset < lineStart ||
                    equation.utf16Offset >= lineEnd || !equation.layout) {
                    continue;
                }
                const qreal x = visualLine.line.cursorToX(
                    static_cast<int>(equation.utf16Offset));
                equation.layout->draw(
                    painter,
                    QPointF(x, visualLine.line.y() +
                                   visualLine.line.ascent()),
                    equation.color);
            }

            if (!paragraphSpellingWords.empty()) {
                const auto spellingLineStart = static_cast<std::size_t>(
                    std::max(0, visualLine.line.textStart()));
                const auto spellingLineEnd = spellingLineStart +
                    static_cast<std::size_t>(
                        std::max(0, visualLine.line.textLength()));
                QPen misspelling(QColor(QStringLiteral("#d92d20")));
                misspelling.setWidthF(0.8);
                painter.setPen(misspelling);
                for (const auto& word : paragraphSpellingWords) {
                    const auto wordStart = static_cast<std::size_t>(
                        paragraph.layoutOffsetForCore(word.start));
                    const auto wordEnd = static_cast<std::size_t>(
                        paragraph.layoutOffsetForCore(word.end));
                    if (wordEnd <= spellingLineStart ||
                        wordStart >= spellingLineEnd ||
                        spelling_.isCorrect(word.text) ||
                        suppressSpellingWord(
                            PendingSpellingWord::Container::bodyParagraph,
                            paragraph.id, 0, 0, word.start, word.end)) {
                        continue;
                    }
                    const auto start = std::max(
                        wordStart, spellingLineStart);
                    const auto end = std::min(wordEnd, spellingLineEnd);
                    const qreal x1 = visualLine.line.cursorToX(
                        static_cast<int>(start));
                    const qreal x2 = visualLine.line.cursorToX(
                        static_cast<int>(end));
                    const qreal y = visualLine.line.y() + visualLine.line.height() - 1.0;
                    painter.drawLine(QPointF(std::min(x1, x2), y), QPointF(std::max(x1, x2), y));
                }
            }
        }
    }

    if (decorations && draggingTable_ && tableDropTargetValid_) {
        int targetPage = 0;
        double targetY = marginTopPoints_;
        if (tableDropBefore_) {
            const auto found = std::find_if(
                blockPlacements_.begin(), blockPlacements_.end(),
                [this](const BlockPlacement& block) {
                    return block.id == *tableDropBefore_;
                });
            if (found != blockPlacements_.end()) {
                targetPage = found->firstPage;
                targetY = found->top;
            }
        } else if (!blockPlacements_.empty()) {
            targetPage = blockPlacements_.back().lastPage;
            targetY = blockPlacements_.back().bottom;
        }
        if (targetPage == pageIndexValue) {
            painter.setPen(QPen(QColor(QStringLiteral("#2f80ed")), 2.0));
            painter.drawLine(QPointF(marginLeftPoints_, targetY),
                             QPointF(pageWidthPoints_ - marginRightPoints_, targetY));
        }
    }

    if (decorations && hasFocus() && tableCursor_ &&
        tableSelectionAnchor_.value_or(tableCursor_->utf16Offset) ==
            tableCursor_->utf16Offset) {
        for (const auto& tableVisual : tableVisuals_) {
            if (tableVisual->id != tableCursor_->tableId) continue;
            const auto found = std::find_if(
                tableVisual->cells.begin(), tableVisual->cells.end(),
                [this](const TableCellVisual& cell) {
                    return cell.row == tableCursor_->row &&
                           cell.column == tableCursor_->column;
                });
            if (found == tableVisual->cells.end() ||
                found->pageIndex != pageIndexValue) break;
            qreal x = found->rect.left() + 5.0;
            qreal top = found->rect.top() + 4.0;
            qreal height = 14.0;
            const QTextLine* caretLine = nullptr;
            for (const auto& line : found->lines) {
                const auto start = static_cast<std::size_t>(line.textStart());
                const auto end = start + static_cast<std::size_t>(line.textLength());
                if (tableCursor_->utf16Offset >= start &&
                    tableCursor_->utf16Offset <= end) {
                    caretLine = &line;
                    break;
                }
            }
            if (!caretLine && !found->lines.empty()) {
                caretLine = &found->lines.back();
            }
            if (caretLine) {
                x = caretLine->cursorToX(static_cast<int>(
                    std::min<std::size_t>(tableCursor_->utf16Offset,
                                          found->text.size())));
                top = caretLine->y();
                height = caretLine->height();
            }
            painter.setPen(QPen(Qt::black, 0.9));
            painter.drawLine(QPointF(x, top), QPointF(x, top + height));
            break;
        }
    } else if (decorations && hasFocus() &&
               selection_.anchor == selection_.focus) {
        const auto* caretLine = visualLineForCaret();
        if (caretLine && caretLine->pageIndex == pageIndexValue) {
            int layoutOffset = static_cast<int>(
                selection_.focus.utf16_offset);
            const auto owner = std::find_if(
                visuals_.begin(), visuals_.end(), [this](const auto& item) {
                    return item->id == selection_.focus.paragraph_id;
                });
            if (owner != visuals_.end()) {
                layoutOffset = (*owner)->layoutOffsetForCore(
                    selection_.focus.utf16_offset);
            }
            const qreal x = caretLine->line.cursorToX(
                layoutOffset);
            painter.setPen(QPen(Qt::black, 0.9));
            painter.drawLine(QPointF(x, caretLine->line.y()),
                             QPointF(x, caretLine->line.y() +
                                                caretLine->line.height()));
        }
    }
    painter.restore();
}

void DocumentCanvas::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void DocumentCanvas::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        constexpr int kAngleUnitsPerStep = 120;
        constexpr int kPixelsPerStep = 40;
        constexpr int kZoomStepPercent = 10;
        int steps = 0;
        if (event->angleDelta().y() != 0) {
            zoomWheelPixelRemainder_ = 0;
            zoomWheelAngleRemainder_ += event->angleDelta().y();
            steps = zoomWheelAngleRemainder_ / kAngleUnitsPerStep;
            zoomWheelAngleRemainder_ %= kAngleUnitsPerStep;
        } else if (event->pixelDelta().y() != 0) {
            zoomWheelAngleRemainder_ = 0;
            zoomWheelPixelRemainder_ += event->pixelDelta().y();
            steps = zoomWheelPixelRemainder_ / kPixelsPerStep;
            zoomWheelPixelRemainder_ %= kPixelsPerStep;
        }
        if (steps != 0) {
            setZoomPercent(zoomPercent_ + steps * kZoomStepPercent);
        }
        const bool pushingPastMaximum =
            zoomPercent_ == kMaximumZoomPercent &&
            (event->angleDelta().y() > 0 || event->pixelDelta().y() > 0);
        const bool pushingPastMinimum =
            zoomPercent_ == kMinimumZoomPercent &&
            (event->angleDelta().y() < 0 || event->pixelDelta().y() < 0);
        if (pushingPastMaximum || pushingPastMinimum ||
            event->phase() == Qt::ScrollEnd) {
            zoomWheelAngleRemainder_ = 0;
            zoomWheelPixelRemainder_ = 0;
        }
        // Ctrl+wheel belongs exclusively to document zoom. Consume even a
        // zero-delta phase event or an event at a zoom bound so it never
        // scrolls the document underneath the pointer.
        event->accept();
        return;
    }
    zoomWheelAngleRemainder_ = 0;
    zoomWheelPixelRemainder_ = 0;
    QAbstractScrollArea::wheelEvent(event);
}

bool DocumentCanvas::apply(std::vector<core::Operation> operations,
                           std::optional<core::Position> resultingCursor,
                           std::optional<core::CharacterFormat> resultingTypingFormat,
                           bool coalesceTyping,
                           std::optional<core::Range> resultingSelection,
                           bool coalesceWithPrevious,
                           bool updateTableSelection,
                           std::optional<TableCursor> resultingTableCursor,
                           std::optional<core::NodeId> resultingSelectedTable,
                           std::optional<LineAffinity> resultingLineAffinity,
                           std::optional<core::CharacterFormatMask>
                               resultingTypingOverrideMask) {
    if (rejectLiveEditDuringPreview()) {
        return false;
    }
    const CursorState before = captureEditorState();
    const bool mergeWithPrevious =
        ((coalesceTyping && typingGroupActive_) || coalesceWithPrevious) &&
        !undoCursorHistory_.empty() &&
        undoCursorHistory_.back().documentTransaction;
    if (!coalesceTyping) {
        endTypingGroup();
    }
    resetVerticalNavigation();
    const auto snap = session_->snapshot();
    const bool hasNonTextChanges = operationsHaveNonTextChanges(
        snap.document, operations);
    const auto result = session_->applyBatch(
        snap.revision, operations,
        mergeWithPrevious
            ? core::UndoGrouping::coalesce_with_previous
            : core::UndoGrouping::separate);
    if (!result) {
        endTypingGroup();
        emit operationFailed(errorText(result.error()));
        return false;
    }
    if (resultingSelection) {
        selection_ = *resultingSelection;
    } else if (resultingCursor) {
        selection_ = {*resultingCursor, *resultingCursor};
    }
    if (resultingTypingFormat) {
        typingFormat_ = *resultingTypingFormat;
    }
    if (resultingTypingOverrideMask) {
        typingOverrideMask_ = *resultingTypingOverrideMask;
    }
    if (updateTableSelection) {
        tableCursor_ = resultingTableCursor;
        tableSelectionAnchor_ = resultingTableCursor
            ? std::optional<std::size_t>(resultingTableCursor->utf16Offset)
            : std::nullopt;
        tableCellSelection_.reset();
        tableMouseSelectionAnchor_.reset();
        selectedTable_ = resultingSelectedTable;
    }
    if (resultingLineAffinity) {
        // A hard visual break has two valid caret locations at the same
        // UTF-16 offset: the end of the preceding QTextLine and the start of
        // the continuation. Remember which side owns the caret so painting,
        // IME placement, vertical navigation, and redo all agree.
        lineAffinity_ = *resultingLineAffinity;
        preferredVerticalX_.reset();
    }
    if (result.value().changed) {
        reconcileDecodedImageCache();
        currentStateId_ = nextStateId_++;
        if (hasNonTextChanges) {
            currentNonTextStateId_ = nextNonTextStateId_++;
        }
        updateDirtyFlags();
        if (mergeWithPrevious) {
            undoCursorHistory_.back().after = captureEditorState();
        } else {
            undoCursorHistory_.push_back(
                CursorHistoryEntry{before, captureEditorState(), true});
        }
        redoCursorHistory_.clear();
        synchronizeCursorHistory();
        invalidateLayout();
        emit documentChanged(result.value().revision.value());
        if (coalesceTyping) {
            typingGroupActive_ = true;
            typingGroupTimer_->start();
        }
    }
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    return true;
}

void DocumentCanvas::replaceSelection(
    const QString& text, bool coalesceTyping,
    std::optional<std::string> resultingParagraphStyle) {
    const auto snap = session_->snapshot();
    const auto normalizedResult = snap.document.normalizeRange(selection_);
    if (!normalizedResult) {
        emit operationFailed(errorText(normalizedResult.error()));
        return;
    }
    auto normalized = normalizedResult.value();
    core::Range effectiveSelection = selection_;
    QString normalizedText = text;
    normalizedText.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalizedText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (normalized.empty() && normalizedText.isEmpty()) return;
    const auto parts = normalizedText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    auto formattingPlan = planReplacementFormatting(
        snap.document, normalized, typingFormat_, typingOverrideMask_,
        defaultFontFamily_, defaultFontPointSize_);
    const auto effectiveTypingFormat =
        formattingPlan.effective_typing_format;
    const auto effectiveTypingOverrideMask =
        formattingPlan.effective_typing_override_mask;
    const auto firstFormat = formattingPlan.insertion_format;
    const auto followingFormat =
        std::optional<core::CharacterFormat>(effectiveTypingFormat);
    bool clearResultingList = false;
    const auto* startParagraph = snap.document.findParagraph(
        normalized.start.paragraph_id);
    std::optional<PlainTextListMarker> continuedListMarker;
    core::NodeId continuedListId{};
    core::ListLayout continuedListLayout;
    std::size_t continuedListLevel = 0;
    bool adoptContinuedList = false;
    std::optional<core::ReplaceRange> prefixNormalization;
    if (startParagraph) {
        const auto marker = plainTextListMarker(fromUtf16(startParagraph->text()));
        clearResultingList = startParagraph->format().list_id && marker &&
            normalized.start.utf16_offset <
                static_cast<std::size_t>(marker->prefixLength);
        if (parts.size() > 1 && marker &&
            normalized.start.utf16_offset >=
                static_cast<std::size_t>(marker->prefixLength)) {
            continuedListMarker = marker;
            continuedListId = startParagraph->format().list_id.value_or(
                core::NodeId::generate());
            continuedListLayout = startParagraph->format().list_layout.value_or(
                core::ListLayout{});
            continuedListLevel = startParagraph->format().list_level
                ? static_cast<std::size_t>(*startParagraph->format().list_level)
                : listLevelForMarker(*marker, tabWidthSpaces_);
            adoptContinuedList = !startParagraph->format().list_id;

            const QString normalizedPrefix = listPrefix(
                marker->kind, marker->marker, continuedListLevel,
                continuedListLayout);
            const QString oldPrefix = fromUtf16(startParagraph->text())
                .first(marker->prefixLength);
            if (oldPrefix != normalizedPrefix) {
                const std::size_t oldLength =
                    static_cast<std::size_t>(marker->prefixLength);
                const std::size_t newLength =
                    static_cast<std::size_t>(normalizedPrefix.size());
                prefixNormalization = core::ReplaceRange{
                    {{startParagraph->id(), 0},
                     {startParagraph->id(), oldLength}},
                    toUtf16(normalizedPrefix),
                    startParagraph->characterFormatAt(0)};
                const auto adjust = [&](core::Position& position) {
                    if (position.paragraph_id != startParagraph->id()) return;
                    position.utf16_offset = position.utf16_offset <= oldLength
                        ? newLength
                        : position.utf16_offset - oldLength + newLength;
                };
                adjust(normalized.start);
                adjust(normalized.end);
                adjust(effectiveSelection.anchor);
                adjust(effectiveSelection.focus);
            }
        }
    }

    std::vector<core::Operation> operations =
        std::move(formattingPlan.operations);
    if (prefixNormalization) {
        operations.push_back(std::move(*prefixNormalization));
    }
    std::vector<core::NodeId> continuedParagraphIds;
    if (adoptContinuedList) {
        continuedParagraphIds.push_back(normalized.start.paragraph_id);
    }
    core::Position cursor = normalized.start;
    const auto first = toUtf16(parts.front());
    const auto directTypingDelta = maskedCharacterFormatDelta(
        effectiveTypingFormat, effectiveTypingOverrideMask);
    std::vector<core::NodeId> emptyTypingCandidates;
    appendInitialReplacementOperations(
        operations, normalized, effectiveSelection, first, firstFormat,
        directTypingDelta, cursor, emptyTypingCandidates);
    if (clearResultingList) {
        operations.push_back(core::SetParagraphFormat{
            {normalized.start.paragraph_id}, clearSemanticListDelta()});
    }

    for (int index = 1; index < parts.size(); ++index) {
        const auto newId = core::NodeId::generate();
        operations.push_back(core::SplitParagraph{
            cursor, newId, followingFormat});
        cursor = {newId, 0};
        QString insertedPart = parts[index];
        if (continuedListMarker) {
            const auto nextMarker = continuationMarker(
                *continuedListMarker, continuedListLevel);
            if (nextMarker) {
                continuedListMarker->marker = *nextMarker;
                insertedPart.prepend(listPrefix(
                    continuedListMarker->kind,
                    continuedListMarker->marker,
                    continuedListLevel, continuedListLayout));
                if (adoptContinuedList) continuedParagraphIds.push_back(newId);
            } else {
                continuedListMarker.reset();
            }
        }
        const auto part = toUtf16(insertedPart);
        if (!part.empty()) {
            const core::Position partStart = cursor;
            operations.push_back(core::InsertText{cursor, part, followingFormat});
            cursor.utf16_offset = part.size();
            if (!directTypingDelta.empty()) {
                operations.push_back(core::SetCharacterFormat{
                    {partStart, cursor}, directTypingDelta});
            }
        } else if (!directTypingDelta.empty()) {
            emptyTypingCandidates.push_back(cursor.paragraph_id);
        }
    }
    if (adoptContinuedList && !continuedParagraphIds.empty()) {
        operations.push_back(core::SetParagraphFormat{
            std::move(continuedParagraphIds),
            semanticListDelta(continuedListId, continuedListLevel,
                              continuedListLayout)});
    }

    QString planningError;
    if (!appendEmptyTypingOverrideOperations(
            snap.document, operations, std::move(emptyTypingCandidates),
            directTypingDelta, planningError)) {
        emit operationFailed(planningError);
        return;
    }

    if (operations.empty()) {
        return;
    }
    std::optional<LineAffinity> resultingLineAffinity;
    if (!normalizedText.isEmpty() &&
        normalizedText.back() == QChar::LineSeparator) {
        resultingLineAffinity = LineAffinity{
            cursor.paragraph_id, static_cast<int>(cursor.utf16_offset)};
    }
    std::optional<core::CharacterFormat> resultingTypingFormat =
        std::move(formattingPlan.resulting_typing_format);
    if (resultingParagraphStyle) {
        const auto* target =
            core::findBuiltInParagraphStyle(*resultingParagraphStyle);
        if (!target) {
            emit operationFailed(tr("Unknown next paragraph style."));
            return;
        }

        // Plan against the exact post-replacement document, but do not expose
        // it as a live revision. This handles a replacement spanning several
        // paragraphs and formatting runs without ever publishing the
        // intermediate inherited-style paragraph.
        core::DocumentSession projectedSession(snap.document);
        const auto projectedBase = projectedSession.snapshot();
        const auto projected = projectedSession.applyBatch(
            projectedBase.revision, operations);
        if (!projected) {
            emit operationFailed(errorText(projected.error()));
            return;
        }
        const auto projectedSnapshot = projectedSession.snapshot();
        const std::array<core::NodeId, 1> targetParagraphs{
            cursor.paragraph_id};
        auto stylePlan = planParagraphStyleApplication(
            projectedSnapshot.document, targetParagraphs, *target,
            defaultFontFamily_, defaultFontPointSize_,
            effectiveTypingFormat,
            cursor, false, effectiveTypingOverrideMask);
        operations.insert(
            operations.end(),
            std::make_move_iterator(stylePlan.operations.begin()),
            std::make_move_iterator(stylePlan.operations.end()));
        resultingTypingFormat =
            std::move(stylePlan.resultingTypingFormat);
        formattingPlan.resulting_typing_override_mask =
            std::move(stylePlan.resultingTypingOverrideMask);
    }

    const bool shouldResequence = continuedListMarker &&
        continuedListMarker->kind == PlainTextListMarker::Kind::numbered;
    if (apply(std::move(operations), cursor,
              std::move(resultingTypingFormat), coalesceTyping,
              std::nullopt, false, false, std::nullopt, std::nullopt,
              resultingLineAffinity,
              formattingPlan.resulting_typing_override_mask.value_or(
                  effectiveTypingOverrideMask)) && shouldResequence) {
        static_cast<void>(resequenceNumberedList(cursor.paragraph_id, true));
    }
}

void DocumentCanvas::insertParagraphBreak() {
    const auto before = session_->snapshot();
    const auto normalized = before.document.normalizeRange(selection_);
    const core::ParagraphStyleDefinition* previousStyle = nullptr;
    if (normalized) {
        if (const auto* paragraph = before.document.findParagraph(
                normalized.value().start.paragraph_id)) {
            previousStyle = core::findBuiltInParagraphStyle(
                paragraph->styleId().value_or("Normal"));
        }
    }
    const std::string nextStyleId = previousStyle
        ? std::string(previousStyle->next_style_id)
        : std::string{};
    const bool changesStyle = !nextStyleId.empty() &&
        (!previousStyle || previousStyle->id != nextStyleId);
    replaceSelection(
        QStringLiteral("\n"), false,
        changesStyle ? std::optional<std::string>(nextStyleId)
                     : std::nullopt);
}

void DocumentCanvas::insertText(const QString& text) {
    clearPendingSpellingWord();
    if (tableCursor_) {
        static_cast<void>(replaceTableCellText(text, false));
        return;
    }
    if (selectedTable_) {
        emit operationFailed(tr(
            "Press Enter or F2 to edit the selected table."));
        return;
    }
    replaceSelection(text);
}

bool DocumentCanvas::insertEquation(const QString& latex, bool display) {
    if (selectedTable_) {
        emit operationFailed(tr(
            "Equations inside table cells are not supported in this release."));
        return false;
    }
    if (rejectLiveEditDuringPreview()) return false;
    const QByteArray encoded = latex.toUtf8();
    const auto parsed = math::parseLatex(
        std::string_view(encoded.constData(),
                         static_cast<std::size_t>(encoded.size())),
        editorEquationLimits());
    if (!parsed) {
        emit operationFailed(tr("Invalid equation at byte %1: %2")
                                 .arg(static_cast<qulonglong>(
                                     parsed.error().byte_offset))
                                 .arg(QString::fromStdString(
                                     parsed.error().message)));
        return false;
    }

    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return false;
    }
    const core::Position insertion = normalized.value().start;
    auto formattingPlan = planReplacementFormatting(
        snap.document, normalized.value(), typingFormat_,
        typingOverrideMask_, defaultFontFamily_, defaultFontPointSize_);
    const auto directTypingDelta = maskedCharacterFormatDelta(
        formattingPlan.effective_typing_format,
        formattingPlan.effective_typing_override_mask);
    auto operations = std::move(formattingPlan.operations);
    if (!normalized.value().empty()) {
        operations.push_back(core::DeleteRange{selection_});
    }
    operations.push_back(core::InsertEquation{
        insertion, math::toCanonicalLatex(parsed.value()), display,
        core::NodeId::generate(),
        formattingPlan.insertion_format});
    core::Position cursor = insertion;
    ++cursor.utf16_offset;
    if (!directTypingDelta.empty()) {
        operations.emplace_back(core::SetCharacterFormat{
            {insertion, cursor}, directTypingDelta});
    }
    return apply(
        std::move(operations), cursor,
        std::move(formattingPlan.resulting_typing_format), false,
        std::nullopt, false, false, std::nullopt, std::nullopt,
        std::nullopt,
        std::move(formattingPlan.resulting_typing_override_mask));
}

bool DocumentCanvas::insertInlineImage(
    std::vector<std::uint8_t> encodedBytes,
    const QString& accessibleName) {
    return insertInlineImageWithGeometry(
        std::move(encodedBytes), accessibleName, std::nullopt, std::nullopt);
}

bool DocumentCanvas::insertHeaderFooterImage(
    std::vector<std::uint8_t> encodedBytes,
    const QString& accessibleName) {
    if (!headerFooterEditing_ || encodedBytes.empty() ||
        encodedBytes.size() > core::kMaximumEncodedImageBytes) {
        return false;
    }
    raster::ValidationLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    const auto inspection = raster::inspect(encodedBytes,
                                             raster::Format::unknown,
                                             limits);
    if (!inspection.ok()) {
        emit operationFailed(tr(
            "The picture is not a valid bounded PNG or JPEG image."));
        return false;
    }
    const QString mimeType = inspection.format == raster::Format::png
        ? QStringLiteral("image/png") : QStringLiteral("image/jpeg");
    QMimeData mime;
    mime.setData(mimeType, QByteArray(
        reinterpret_cast<const char*>(encodedBytes.data()),
        static_cast<qsizetype>(encodedBytes.size())));
    auto& editors = activeStoryIsFooter_ ? footerEditors_ : headerEditors_;
    auto* target = editors[static_cast<std::size_t>(
        std::clamp(activeStoryRegion_, 0, 2))];
    return pasteStoryImage(target, &mime, accessibleName);
}

std::optional<QByteArray> DocumentCanvas::selectedExcalidrawScene() const {
    const auto selected = selectedInlineImage();
    if (!selected || selected->second.format != core::ImageFormat::png) {
        return std::nullopt;
    }
    const auto bytes = selected->second.encoded_payload.bytes();
    return excalidrawSceneFromPng(QByteArray(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qsizetype>(bytes.size())));
}

bool DocumentCanvas::replaceSelectedExcalidrawFigure(
    std::vector<std::uint8_t> encodedPng) {
    if (rejectLiveEditDuringPreview()) return false;
    const auto selected = selectedInlineImage();
    if (!selected || !selectedExcalidrawScene()) return false;
    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    limits.maximum_decoded_bytes = kMaximumAggregateDecodedRasterBytes;
    auto decoded = decodeRasterImage(encodedPng, limits);
    if (!decoded.ok() || decoded.format != raster::Format::png) {
        emit operationFailed(tr(
            "The edited figure did not return a valid bounded PNG preview."));
        return false;
    }
    const auto& old = selected->second;
    const double aspect = static_cast<double>(decoded.image.height()) /
        static_cast<double>(decoded.image.width());
    const auto height = static_cast<std::int64_t>(std::llround(
        static_cast<double>(old.width_emu) * aspect));
    if (height <= 0 ||
        height > core::kMaximumInlineImageDimensionEmu) {
        emit operationFailed(tr(
            "The edited figure's display dimensions are invalid."));
        return false;
    }
    if (!apply({core::ReplaceImagePayload{
            old.id, core::EncodedImagePayload(std::move(encodedPng)),
            core::ImageFormat::png, old.width_emu, height}},
            std::nullopt, std::nullopt, false, selection_)) {
        return false;
    }
    const auto cached = decodedImages_.find(old.id);
    if (cached != decodedImages_.end()) {
        decodedImageBytes_ -= cached->second.sizeInBytes();
        decodedImages_.erase(cached);
    }
    const auto [inserted, wasInserted] =
        decodedImages_.emplace(old.id, std::move(decoded.image));
    if (wasInserted) decodedImageBytes_ += inserted->second.sizeInBytes();
    invalidateLayout();
    viewport()->update();
    return true;
}

bool DocumentCanvas::insertInlineImageWithGeometry(
    std::vector<std::uint8_t> encodedBytes,
    const QString& accessibleName,
    std::optional<std::int64_t> requestedWidthEmu,
    std::optional<std::int64_t> requestedHeightEmu,
    core::ImageLayout layout) {
    if (rejectLiveEditDuringPreview()) return false;
    if (selectedTable_ || tableCursor_ || tableCellSelection_) {
        emit operationFailed(tr(
            "Pictures inside table cells are not supported in this release."));
        return false;
    }
    if (encodedBytes.empty() ||
        encodedBytes.size() > core::kMaximumEncodedImageBytes) {
        emit operationFailed(tr(
            "The picture is empty or exceeds the 16 MiB encoded-picture limit."));
        return false;
    }

    const auto snapshot = session_->snapshot();
    const auto normalized = snapshot.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return false;
    }
    std::size_t survivingImages = 0;
    std::size_t survivingEncodedBytes = 0;
    for (std::size_t paragraphIndex = 0;
         paragraphIndex < snapshot.document.paragraphs().size();
         ++paragraphIndex) {
        const auto& paragraph = snapshot.document.paragraphs()[paragraphIndex];
        for (const auto& image : paragraph.images()) {
            const bool replaced = !normalized.value().empty() &&
                paragraphIndex >= normalized.value().start_paragraph_index &&
                paragraphIndex <= normalized.value().end_paragraph_index &&
                (paragraphIndex != normalized.value().start_paragraph_index ||
                 image.utf16_offset >=
                     normalized.value().start.utf16_offset) &&
                (paragraphIndex != normalized.value().end_paragraph_index ||
                 image.utf16_offset < normalized.value().end.utf16_offset);
            if (replaced) continue;
            ++survivingImages;
            survivingEncodedBytes += image.encoded_payload.size();
        }
    }
    if (survivingImages >= core::kMaximumInlineImagesPerDocument ||
        encodedBytes.size() >
            core::kMaximumDocumentEncodedImageBytes -
                std::min(survivingEncodedBytes,
                         core::kMaximumDocumentEncodedImageBytes)) {
        emit operationFailed(tr(
            "This document has reached its picture count or 32 MiB encoded-picture limit."));
        return false;
    }
    if (decodedImageBytes_ >= kMaximumAggregateDecodedRasterBytes) {
        emit operationFailed(tr(
            "This document has reached the decoded-picture memory limit."));
        return false;
    }
    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    limits.maximum_decoded_bytes = std::min(
        limits.maximum_decoded_bytes,
        kMaximumAggregateDecodedRasterBytes - decodedImageBytes_);
    auto decoded = decodeRasterImage(encodedBytes, limits);
    if (!decoded.ok() ||
        (decoded.format != raster::Format::png &&
         decoded.format != raster::Format::jpeg)) {
        emit operationFailed(tr(
            "The picture is not a valid bounded PNG or JPEG image."));
        return false;
    }

    const core::Position insertion = normalized.value().start;
    std::int64_t widthEmu = requestedWidthEmu.value_or(0);
    std::int64_t heightEmu = requestedHeightEmu.value_or(0);
    if (requestedWidthEmu.has_value() != requestedHeightEmu.has_value()) {
        emit operationFailed(tr("The pasted picture geometry is incomplete."));
        return false;
    }
    if (!requestedWidthEmu) {
        // Use Word's common 96-DPI fallback, then fit to the current text area.
        double widthPoints = static_cast<double>(decoded.image.width()) * 0.75;
        double heightPoints = static_cast<double>(decoded.image.height()) * 0.75;
        const double availableWidth = std::max(
            18.0, pageWidthPoints_ - marginLeftPoints_ - marginRightPoints_);
        const double availableHeight = std::max(
            18.0, pageHeightPoints_ - marginTopPoints_ - marginBottomPoints_);
        const double scale = std::min(
            {1.0, availableWidth / widthPoints,
             availableHeight / heightPoints});
        widthEmu = static_cast<std::int64_t>(
            std::llround(widthPoints * scale * kEmuPerPoint));
        heightEmu = static_cast<std::int64_t>(
            std::llround(heightPoints * scale * kEmuPerPoint));
    }
    if (widthEmu <= 0 || heightEmu <= 0 ||
        widthEmu > core::kMaximumInlineImageDimensionEmu ||
        heightEmu > core::kMaximumInlineImageDimensionEmu) {
        emit operationFailed(tr("The picture display size is invalid."));
        return false;
    }

    QString safeName = accessibleName.trimmed();
    if (safeName.isEmpty()) safeName = tr("Picture");
    QByteArray encodedName = safeName.toUtf8();
    while (encodedName.size() >
               static_cast<qsizetype>(core::kMaximumImageAccessibleNameBytes) &&
           !safeName.isEmpty()) {
        safeName.chop(1);
        encodedName = safeName.toUtf8();
    }

    const auto imageId = core::NodeId::generate();
    auto formattingPlan = planReplacementFormatting(
        snapshot.document, normalized.value(), typingFormat_,
        typingOverrideMask_, defaultFontFamily_, defaultFontPointSize_);
    const auto directTypingDelta = maskedCharacterFormatDelta(
        formattingPlan.effective_typing_format,
        formattingPlan.effective_typing_override_mask);
    auto operations = std::move(formattingPlan.operations);
    if (!normalized.value().empty()) {
        operations.emplace_back(core::DeleteRange{selection_});
    }
    operations.emplace_back(core::InsertImage{
        insertion, core::EncodedImagePayload(std::move(encodedBytes)),
        decoded.format == raster::Format::png ? core::ImageFormat::png
                                              : core::ImageFormat::jpeg,
        encodedName.toStdString(), widthEmu, heightEmu, imageId,
        formattingPlan.insertion_format, layout});
    core::Position after = insertion;
    ++after.utf16_offset;
    if (!directTypingDelta.empty()) {
        operations.emplace_back(core::SetCharacterFormat{
            {insertion, after}, directTypingDelta});
    }
    const core::Range imageSelection{insertion, after};
    if (!apply(
            std::move(operations), std::nullopt,
            std::move(formattingPlan.resulting_typing_format), false,
            imageSelection, false, false, std::nullopt, std::nullopt,
            std::nullopt,
            std::move(formattingPlan.resulting_typing_override_mask))) {
        return false;
    }
    const auto [cached, inserted] = decodedImages_.emplace(
        imageId, std::move(decoded.image));
    if (inserted) decodedImageBytes_ += cached->second.sizeInBytes();
    invalidateLayout();
    viewport()->update();
    return true;
}

bool DocumentCanvas::selectInlineImage(core::NodeId imageId) {
    if (previewId_) return false;
    const auto snapshot = session_->snapshot();
    std::optional<core::Position> start;
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        const auto found = std::find_if(
            paragraph.images().begin(), paragraph.images().end(),
            [imageId](const core::ImageAtom& image) {
                return image.id == imageId;
            });
        if (found != paragraph.images().end()) {
            start = core::Position{paragraph.id(), found->utf16_offset};
            break;
        }
    }
    if (!start) return false;

    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    core::Position end = *start;
    ++end.utf16_offset;
    selection_ = {*start, end};
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_.reset();
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    return true;
}

bool DocumentCanvas::deleteSelectedInlineImage() {
    if (rejectLiveEditDuringPreview()) return false;
    const auto selected = selectedInlineImage();
    if (!selected) return false;
    const core::Position start = selected->first;
    core::Position end = start;
    ++end.utf16_offset;
    return apply({core::DeleteRange{{start, end}}}, start);
}

bool DocumentCanvas::resizeSelectedInlineImage(double widthPoints,
                                               double heightPoints) {
    if (rejectLiveEditDuringPreview()) return false;
    constexpr double kMinimumPicturePoints = 1.0;
    constexpr double kMaximumPicturePoints = 20'000.0;
    if (!std::isfinite(widthPoints) || !std::isfinite(heightPoints) ||
        widthPoints < kMinimumPicturePoints ||
        heightPoints < kMinimumPicturePoints ||
        widthPoints > kMaximumPicturePoints ||
        heightPoints > kMaximumPicturePoints) {
        emit operationFailed(tr("The requested picture size is invalid."));
        return false;
    }
    const auto selected = selectedInlineImage();
    if (!selected) return false;
    const auto widthEmu = static_cast<std::int64_t>(
        std::llround(widthPoints * kEmuPerPoint));
    const auto heightEmu = static_cast<std::int64_t>(
        std::llround(heightPoints * kEmuPerPoint));
    if (selected->second.width_emu == widthEmu &&
        selected->second.height_emu == heightEmu) {
        return true;
    }
    return apply({core::ResizeImage{selected->second.id, widthEmu, heightEmu}},
                 std::nullopt, std::nullopt, false, selection_);
}

bool DocumentCanvas::setSelectedImageLayout(
    const core::ImageLayout& layout) {
    if (rejectLiveEditDuringPreview()) return false;
    const auto validation = layout.validate();
    if (!validation) {
        emit operationFailed(errorText(validation.error()));
        return false;
    }
    const auto selected = selectedInlineImage();
    if (!selected) return false;
    if (selected->second.layout == layout) return true;
    return apply({core::SetImageLayout{selected->second.id, layout}},
                 std::nullopt, std::nullopt, false, selection_);
}

bool DocumentCanvas::setSelectedImageAccessibleName(
    const QString& accessibleName) {
    if (rejectLiveEditDuringPreview()) return false;
    const QByteArray utf8 = accessibleName.toUtf8();
    if (utf8.size() > static_cast<qsizetype>(
                          core::kMaximumImageAccessibleNameBytes)) {
        emit operationFailed(tr("Alt text is too long."));
        return false;
    }
    const auto selected = selectedInlineImage();
    if (!selected) return false;
    const std::string value(utf8.constData(),
                            static_cast<std::size_t>(utf8.size()));
    if (selected->second.accessible_name == value) return true;
    return apply({core::SetImageAccessibleName{selected->second.id, value}},
                 std::nullopt, std::nullopt, false, selection_);
}

void DocumentCanvas::showSelectedImageSizeDialog() {
    const auto selected = selectedInlineImage();
    if (!selected) return;
    const double selectedWidthPoints =
        static_cast<double>(selected->second.width_emu) / kEmuPerPoint;
    const double selectedHeightPoints =
        static_cast<double>(selected->second.height_emu) / kEmuPerPoint;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pictureSizeDialog"));
    dialog.setWindowTitle(tr("Picture Size"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* width = new QDoubleSpinBox(&dialog);
    width->setObjectName(QStringLiteral("pictureSize.width"));
    constexpr double kEmuPerInch = 72.0 * kEmuPerPoint;
    constexpr double kMinimumPictureInches = 1.0 / 72.0;
    const double maximumPictureInches =
        static_cast<double>(core::kMaximumInlineImageDimensionEmu) /
        kEmuPerInch;
    // Six decimal inches resolve to less than half an EMU, so accepting an
    // untouched value can never silently quantize valid stored geometry.
    width->setDecimals(6);
    width->setRange(kMinimumPictureInches, maximumPictureInches);
    width->setSuffix(tr(" in"));
    width->setValue(selectedWidthPoints / 72.0);
    auto* height = new QDoubleSpinBox(&dialog);
    height->setObjectName(QStringLiteral("pictureSize.height"));
    height->setDecimals(6);
    height->setRange(kMinimumPictureInches, maximumPictureInches);
    height->setSuffix(tr(" in"));
    height->setValue(selectedHeightPoints / 72.0);
    const double initialWidthValue = width->value();
    const double initialHeightValue = height->value();
    auto* lockAspect = new QCheckBox(tr("Lock aspect ratio"), &dialog);
    lockAspect->setObjectName(QStringLiteral("pictureSize.lockAspect"));
    lockAspect->setChecked(true);
    form->addRow(tr("Width:"), width);
    form->addRow(tr("Height:"), height);
    form->addRow(QString(), lockAspect);
    outer->addLayout(form);

    const double aspect = selectedHeightPoints > 0.0
        ? selectedWidthPoints / selectedHeightPoints
        : 1.0;
    connect(width, qOverload<double>(&QDoubleSpinBox::valueChanged),
            &dialog, [height, lockAspect, aspect](double value) {
                if (!lockAspect->isChecked() || aspect <= 0.0) return;
                const QSignalBlocker blocker(height);
                height->setValue(value / aspect);
            });
    connect(height, qOverload<double>(&QDoubleSpinBox::valueChanged),
            &dialog, [width, lockAspect, aspect](double value) {
                if (!lockAspect->isChecked() || aspect <= 0.0) return;
                const QSignalBlocker blocker(width);
                width->setValue(value * aspect);
            });
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("pictureSize.buttons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted,
            &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            &dialog, &QDialog::reject);
    width->setFocus();
    width->selectAll();
    if (dialog.exec() == QDialog::Accepted) {
        const auto toEmu = [](double inches) {
            return static_cast<std::int64_t>(
                std::llround(inches * 72.0 * kEmuPerPoint));
        };
        const std::int64_t widthEmu = width->value() == initialWidthValue
            ? selected->second.width_emu
            : toEmu(width->value());
        const std::int64_t heightEmu = height->value() == initialHeightValue
            ? selected->second.height_emu
            : toEmu(height->value());
        static_cast<void>(resizeSelectedInlineImage(
            static_cast<double>(widthEmu) / kEmuPerPoint,
            static_cast<double>(heightEmu) / kEmuPerPoint));
    }
}

void DocumentCanvas::showSelectedImageAltTextDialog() {
    const auto selected = selectedInlineImage();
    if (!selected) return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pictureAltTextDialog"));
    dialog.setWindowTitle(tr("Picture Alt Text"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        tr("Describe the picture for people who use a screen reader."),
        &dialog);
    explanation->setWordWrap(true);
    outer->addWidget(explanation);
    auto* edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("pictureAltText.value"));
    edit->setAccessibleName(tr("Picture description"));
    // The model limit is measured in UTF-8 bytes, not UTF-16 code units.
    // Keep one extra code unit as an overflow sentinel: an over-limit ASCII
    // paste must remain visibly invalid instead of being silently truncated
    // to an apparently valid 4096-byte value.
    edit->setMaxLength(
        static_cast<int>(core::kMaximumImageAccessibleNameBytes) + 1);
    edit->setText(QString::fromUtf8(
        selected->second.accessible_name.data(),
        static_cast<qsizetype>(selected->second.accessible_name.size())));
    edit->selectAll();
    outer->addWidget(edit);
    auto* byteCount = new QLabel(&dialog);
    byteCount->setObjectName(QStringLiteral("pictureAltText.byteCount"));
    byteCount->setAccessibleName(tr("Alt text length"));
    outer->addWidget(byteCount);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("pictureAltText.buttons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const auto refreshByteCount = [edit, byteCount, buttons] {
        const qsizetype encodedBytes = edit->text().toUtf8().size();
        const qsizetype maximumBytes = static_cast<qsizetype>(
            core::kMaximumImageAccessibleNameBytes);
        const bool valid = encodedBytes <= maximumBytes;
        byteCount->setText(valid
            ? QObject::tr("%1 of %2 UTF-8 bytes")
                  .arg(encodedBytes)
                  .arg(maximumBytes)
            : QObject::tr("Alt text is %1 bytes; the maximum is %2.")
                  .arg(encodedBytes)
                  .arg(maximumBytes));
        byteCount->setStyleSheet(valid
            ? QString()
            : QStringLiteral("color: #c01c28;"));
        if (auto* ok = buttons->button(QDialogButtonBox::Ok)) {
            ok->setEnabled(valid);
        }
    };
    connect(edit, &QLineEdit::textChanged, &dialog,
            [refreshByteCount](const QString&) { refreshByteCount(); });
    refreshByteCount();
    edit->setFocus();
    if (dialog.exec() == QDialog::Accepted) {
        static_cast<void>(setSelectedImageAccessibleName(edit->text()));
    }
}

void DocumentCanvas::showSelectedImageLayoutDialog() {
    const auto selected = selectedInlineImage();
    if (!selected) return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pictureLayoutDialog"));
    dialog.setWindowTitle(tr("Picture Layout"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* placement = new QComboBox(&dialog);
    placement->setObjectName(QStringLiteral("pictureLayout.placement"));
    placement->addItem(tr("In Line with Text"),
                       static_cast<int>(
                           core::ImagePlacement::inline_with_text));
    placement->addItem(tr("Square"),
                       static_cast<int>(core::ImagePlacement::square));
    placement->addItem(tr("Top and Bottom"),
                       static_cast<int>(
                           core::ImagePlacement::top_and_bottom));
    const int selectedPlacement = placement->findData(
        static_cast<int>(selected->second.layout.placement));
    placement->setCurrentIndex(std::max(0, selectedPlacement));
    form->addRow(tr("Text wrapping:"), placement);

    const auto makeDistance = [&dialog](const char* objectName,
                                        std::int64_t emu) {
        auto* value = new QDoubleSpinBox(&dialog);
        value->setObjectName(QString::fromLatin1(objectName));
        value->setDecimals(6);
        value->setRange(
            0.0,
            static_cast<double>(core::kMaximumImageWrapDistanceEmu) /
                (72.0 * kEmuPerPoint));
        value->setSingleStep(0.05);
        value->setSuffix(QObject::tr(" in"));
        value->setValue(static_cast<double>(emu) / (72.0 * kEmuPerPoint));
        return value;
    };
    auto* top = makeDistance("pictureLayout.distanceTop",
                             selected->second.layout.distance_top_emu);
    auto* right = makeDistance("pictureLayout.distanceRight",
                               selected->second.layout.distance_right_emu);
    auto* bottom = makeDistance("pictureLayout.distanceBottom",
                                selected->second.layout.distance_bottom_emu);
    auto* left = makeDistance("pictureLayout.distanceLeft",
                              selected->second.layout.distance_left_emu);
    const std::array<double, 4> initialDistanceValues{
        top->value(), right->value(), bottom->value(), left->value()};
    form->addRow(tr("Distance above:"), top);
    form->addRow(tr("Distance right:"), right);
    form->addRow(tr("Distance below:"), bottom);
    form->addRow(tr("Distance left:"), left);
    auto* moveWithText = new QCheckBox(tr("Move with text"), &dialog);
    moveWithText->setObjectName(QStringLiteral("pictureLayout.moveWithText"));
    moveWithText->setChecked(selected->second.layout.move_with_text);
    form->addRow(QString(), moveWithText);
    outer->addLayout(form);

    bool anchoredMoveWithText = moveWithText->isChecked();
    connect(moveWithText, &QCheckBox::toggled, &dialog,
            [&anchoredMoveWithText, moveWithText](bool checked) {
                if (moveWithText->isEnabled()) {
                    anchoredMoveWithText = checked;
                }
            });
    const auto refreshPlacementControls = [
        placement, moveWithText, top, right, bottom, left,
        &anchoredMoveWithText] {
        const bool isInline = placement->currentData().toInt() ==
            static_cast<int>(core::ImagePlacement::inline_with_text);
        const QSignalBlocker moveBlocker(moveWithText);
        moveWithText->setChecked(isInline ? true : anchoredMoveWithText);
        moveWithText->setEnabled(!isInline);
        // Inline distances are retained for round-trip compatibility but the
        // current page engine does not apply them visually.
        top->setEnabled(!isInline);
        right->setEnabled(!isInline);
        bottom->setEnabled(!isInline);
        left->setEnabled(!isInline);
    };
    connect(placement, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [refreshPlacementControls](int) {
                refreshPlacementControls();
            });
    refreshPlacementControls();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("pictureLayout.buttons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    placement->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;

    core::ImageLayout layout;
    layout.placement = static_cast<core::ImagePlacement>(
        placement->currentData().toInt());
    const auto toEmu = [](double inches) {
        return static_cast<std::int64_t>(
            std::llround(inches * 72.0 * kEmuPerPoint));
    };
    const auto retainedEmu = [&toEmu](QDoubleSpinBox* field,
                                     double initialValue,
                                     std::int64_t originalEmu) {
        return field->value() == initialValue
            ? originalEmu
            : toEmu(field->value());
    };
    layout.distance_top_emu = retainedEmu(
        top, initialDistanceValues[0],
        selected->second.layout.distance_top_emu);
    layout.distance_right_emu = retainedEmu(
        right, initialDistanceValues[1],
        selected->second.layout.distance_right_emu);
    layout.distance_bottom_emu = retainedEmu(
        bottom, initialDistanceValues[2],
        selected->second.layout.distance_bottom_emu);
    layout.distance_left_emu = retainedEmu(
        left, initialDistanceValues[3],
        selected->second.layout.distance_left_emu);
    layout.move_with_text = layout.placement ==
            core::ImagePlacement::inline_with_text
        ? true
        : moveWithText->isChecked();
    static_cast<void>(setSelectedImageLayout(layout));
}

bool DocumentCanvas::insertTable(std::size_t rows, std::size_t columns,
                                 bool headerRow) {
    if (selectedTable_) {
        emit operationFailed(tr(
            "Nested tables are not supported in this release."));
        return false;
    }
    if (rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return false;
    }
    auto table = core::Table::create(rows, columns, headerRow);
    if (!table) {
        emit operationFailed(errorText(table.error()));
        return false;
    }

    const auto rightParagraphId = core::NodeId::generate();
    const auto tableId = table.value().id();
    std::vector<core::Operation> operations;
    if (!normalized.value().empty()) {
        operations.push_back(core::DeleteRange{selection_});
    }
    operations.push_back(core::SplitParagraph{
        normalized.value().start, rightParagraphId,
        std::optional<core::CharacterFormat>(typingFormat_)});
    operations.push_back(core::InsertTable{
        rightParagraphId, std::move(table.value())});
    const core::Range bodyCursor{{rightParagraphId, 0}, {rightParagraphId, 0}};
    return apply(std::move(operations), std::nullopt, std::nullopt, false,
                 bodyCursor, false, true,
                 TableCursor{tableId, 0, 0, 0}, tableId);
}

bool DocumentCanvas::activateTableCell(core::NodeId tableId, std::size_t row,
                                       std::size_t column,
                                       std::size_t utf16Offset) {
    if (previewId_) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(tableId);
    const auto* cell = table ? table->cell(row, column) : nullptr;
    if (!cell) return false;
    utf16Offset = std::min(utf16Offset, cell->text.size());
    while (utf16Offset > 0 &&
           !core::isUtf16Boundary(cell->text, utf16Offset)) {
        --utf16Offset;
    }
    endTypingGroup();
    resetVerticalNavigation();
    tableCursor_ = TableCursor{tableId, row, column, utf16Offset};
    tableSelectionAnchor_ = utf16Offset;
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_ = tableId;
    typingFormat_ = currentCharacterFormat();
    typingOverrideMask_ = {};
    commitPendingSpellingWordIfCaretLeft();
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    updateMicroFocus();
    return true;
}

bool DocumentCanvas::selectTableCells(core::NodeId tableId,
                                      std::size_t anchorRow,
                                      std::size_t anchorColumn,
                                      std::size_t focusRow,
                                      std::size_t focusColumn) {
    if (previewId_) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(tableId);
    if (!table || anchorRow >= table->rowCount() ||
        focusRow >= table->rowCount() ||
        anchorColumn >= table->columnCount() ||
        focusColumn >= table->columnCount()) {
        return false;
    }
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_ = TableCellSelection{
        tableId, anchorRow, anchorColumn, focusRow, focusColumn};
    tableMouseSelectionAnchor_.reset();
    selectedTable_ = tableId;
    typingFormat_ = selectedCharacterFormat();
    typingOverrideMask_ = {};
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    updateMicroFocus();
    return true;
}

bool DocumentCanvas::selectTable(core::NodeId tableId) {
    if (previewId_ || !session_->snapshot().document.findTable(tableId)) {
        return false;
    }
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_ = tableId;
    typingOverrideMask_ = {};
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    return true;
}

std::vector<std::pair<std::size_t, std::size_t>>
DocumentCanvas::selectedTableCells(const core::Table& table) const {
    std::vector<std::pair<std::size_t, std::size_t>> result;
    if (!selectedTable_ || *selectedTable_ != table.id()) return result;
    std::size_t firstRow = 0;
    std::size_t lastRow = table.rowCount() - 1;
    std::size_t firstColumn = 0;
    std::size_t lastColumn = table.columnCount() - 1;
    if (tableCellSelection_ && tableCellSelection_->tableId == table.id()) {
        firstRow = std::min(tableCellSelection_->anchorRow,
                            tableCellSelection_->focusRow);
        lastRow = std::max(tableCellSelection_->anchorRow,
                           tableCellSelection_->focusRow);
        firstColumn = std::min(tableCellSelection_->anchorColumn,
                               tableCellSelection_->focusColumn);
        lastColumn = std::max(tableCellSelection_->anchorColumn,
                              tableCellSelection_->focusColumn);
    } else if (tableCursor_ && tableCursor_->tableId == table.id()) {
        firstRow = lastRow = tableCursor_->row;
        firstColumn = lastColumn = tableCursor_->column;
    }
    result.reserve((lastRow - firstRow + 1) *
                   (lastColumn - firstColumn + 1));
    for (std::size_t row = firstRow; row <= lastRow; ++row) {
        for (std::size_t column = firstColumn; column <= lastColumn;
             ++column) {
            result.emplace_back(row, column);
        }
    }
    return result;
}

bool DocumentCanvas::tableCellIsSelected(core::NodeId tableId,
                                         std::size_t row,
                                         std::size_t column) const noexcept {
    if (!tableCellSelection_ ||
        tableCellSelection_->tableId != tableId) {
        return false;
    }
    const auto firstRow = std::min(tableCellSelection_->anchorRow,
                                   tableCellSelection_->focusRow);
    const auto lastRow = std::max(tableCellSelection_->anchorRow,
                                  tableCellSelection_->focusRow);
    const auto firstColumn = std::min(tableCellSelection_->anchorColumn,
                                      tableCellSelection_->focusColumn);
    const auto lastColumn = std::max(tableCellSelection_->anchorColumn,
                                     tableCellSelection_->focusColumn);
    return row >= firstRow && row <= lastRow && column >= firstColumn &&
           column <= lastColumn;
}

bool DocumentCanvas::insertTableRow(bool after) {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(*selectedTable_);
    if (!table) return false;
    if (table->rowCount() >= core::Table::maximum_rows) {
        emit operationFailed(tr(
            "This table already has the maximum number of rows."));
        return false;
    }
    std::size_t index = after ? table->rowCount() : 0U;
    std::size_t column = 0;
    if (tableCursor_) {
        index = tableCursor_->row + (after ? 1U : 0U);
        column = tableCursor_->column;
    } else if (tableCellSelection_) {
        const auto first = std::min(tableCellSelection_->anchorRow,
                                    tableCellSelection_->focusRow);
        const auto last = std::max(tableCellSelection_->anchorRow,
                                   tableCellSelection_->focusRow);
        index = after ? last + 1U : first;
        column = std::min(tableCellSelection_->anchorColumn,
                          tableCellSelection_->focusColumn);
    }
    std::vector<core::NodeId> ids;
    ids.reserve(table->columnCount());
    for (std::size_t current = 0; current < table->columnCount(); ++current) {
        ids.push_back(core::NodeId::generate());
    }
    return apply({core::InsertTableRow{
                     table->id(), index, std::move(ids),
                     after ? core::TableInsertionSource::preceding
                           : core::TableInsertionSource::following}},
                 std::nullopt, std::nullopt, false, std::nullopt, false,
                 true, TableCursor{table->id(), index, column, 0},
                 table->id());
}

bool DocumentCanvas::insertTableColumn(bool after) {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(*selectedTable_);
    if (!table) return false;
    if (table->columnCount() >= core::Table::maximum_columns) {
        emit operationFailed(tr(
            "This table already has the maximum number of columns."));
        return false;
    }
    std::size_t index = after ? table->columnCount() : 0U;
    std::size_t row = 0;
    if (tableCursor_) {
        index = tableCursor_->column + (after ? 1U : 0U);
        row = tableCursor_->row;
    } else if (tableCellSelection_) {
        const auto first = std::min(tableCellSelection_->anchorColumn,
                                    tableCellSelection_->focusColumn);
        const auto last = std::max(tableCellSelection_->anchorColumn,
                                   tableCellSelection_->focusColumn);
        index = after ? last + 1U : first;
        row = std::min(tableCellSelection_->anchorRow,
                       tableCellSelection_->focusRow);
    }
    std::vector<core::NodeId> ids;
    ids.reserve(table->rowCount());
    for (std::size_t current = 0; current < table->rowCount(); ++current) {
        ids.push_back(core::NodeId::generate());
    }
    return apply(
        {core::InsertTableColumn{
            table->id(), index, std::move(ids),
            after ? core::TableInsertionSource::preceding
                  : core::TableInsertionSource::following}},
        std::nullopt, std::nullopt, false, std::nullopt, false, true,
        TableCursor{table->id(), row, index, 0}, table->id());
}

bool DocumentCanvas::deleteSelectedTableRows() {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(*selectedTable_);
    if (!table) return false;
    if (!tableCursor_ && !tableCellSelection_) {
        emit operationFailed(tr(
            "Select a cell or cell range before deleting table rows."));
        return false;
    }
    const std::size_t first = tableCellSelection_
        ? std::min(tableCellSelection_->anchorRow,
                   tableCellSelection_->focusRow)
        : tableCursor_->row;
    const std::size_t last = tableCellSelection_
        ? std::max(tableCellSelection_->anchorRow,
                   tableCellSelection_->focusRow)
        : tableCursor_->row;
    const std::size_t count = last - first + 1U;
    if (count >= table->rowCount()) {
        emit operationFailed(tr(
            "A table must keep at least one row."));
        return false;
    }
    const std::size_t column = tableCursor_
        ? tableCursor_->column
        : std::min(tableCellSelection_->anchorColumn,
                   tableCellSelection_->focusColumn);
    const std::size_t targetRow = std::min(
        first, table->rowCount() - count - 1U);
    return apply({core::DeleteTableRows{table->id(), first, count}},
                 std::nullopt, std::nullopt, false, std::nullopt, false,
                 true, TableCursor{table->id(), targetRow, column, 0},
                 table->id());
}

bool DocumentCanvas::deleteSelectedTableColumns() {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(*selectedTable_);
    if (!table) return false;
    if (!tableCursor_ && !tableCellSelection_) {
        emit operationFailed(tr(
            "Select a cell or cell range before deleting table columns."));
        return false;
    }
    const std::size_t first = tableCellSelection_
        ? std::min(tableCellSelection_->anchorColumn,
                   tableCellSelection_->focusColumn)
        : tableCursor_->column;
    const std::size_t last = tableCellSelection_
        ? std::max(tableCellSelection_->anchorColumn,
                   tableCellSelection_->focusColumn)
        : tableCursor_->column;
    const std::size_t count = last - first + 1U;
    if (count >= table->columnCount()) {
        emit operationFailed(tr(
            "A table must keep at least one column."));
        return false;
    }
    const std::size_t row = tableCursor_
        ? tableCursor_->row
        : std::min(tableCellSelection_->anchorRow,
                   tableCellSelection_->focusRow);
    const std::size_t targetColumn = std::min(
        first, table->columnCount() - count - 1U);
    return apply({core::DeleteTableColumns{table->id(), first, count}},
                 std::nullopt, std::nullopt, false, std::nullopt, false,
                 true, TableCursor{table->id(), row, targetColumn, 0},
                 table->id());
}

bool DocumentCanvas::setTableStyle(const QString& styleKey) {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    static const std::map<QString, core::TableStyle> styles{
        {QStringLiteral("plain"), core::TableStyle::plain},
        {QStringLiteral("grid"), core::TableStyle::grid},
        {QStringLiteral("light-gray"), core::TableStyle::light_gray},
        {QStringLiteral("light-blue"), core::TableStyle::light_blue},
        {QStringLiteral("light-orange"), core::TableStyle::light_orange},
        {QStringLiteral("medium-blue"), core::TableStyle::medium_blue},
        {QStringLiteral("medium-green"), core::TableStyle::medium_green},
        {QStringLiteral("medium-orange"), core::TableStyle::medium_orange},
        {QStringLiteral("aubergine"), core::TableStyle::aubergine},
        {QStringLiteral("orange-accent"), core::TableStyle::orange_accent},
        {QStringLiteral("banded-blue"), core::TableStyle::banded_blue},
        {QStringLiteral("banded-aubergine"),
         core::TableStyle::banded_aubergine},
        {QStringLiteral("dark-header"), core::TableStyle::dark_header},
    };
    const auto found = styles.find(styleKey);
    if (found == styles.end()) {
        emit operationFailed(tr("Unknown table style."));
        return false;
    }
    return apply({core::SetTableStyle{*selectedTable_, found->second}});
}

bool DocumentCanvas::replaceTableCellText(const QString& insertedText,
                                          bool coalesceTyping) {
    if (!tableCursor_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(tableCursor_->tableId);
    const auto* cell = table
        ? table->cell(tableCursor_->row, tableCursor_->column) : nullptr;
    if (!cell) return false;
    QString normalized = insertedText;
    normalized.replace(QStringLiteral("\r\n"), QString(QChar::LineSeparator));
    normalized.replace(QLatin1Char('\r'), QChar::LineSeparator);
    normalized.replace(QLatin1Char('\n'), QChar::LineSeparator);
    auto updated = fromUtf16(cell->text);
    const auto focus = static_cast<qsizetype>(std::min(
        tableCursor_->utf16Offset, cell->text.size()));
    const auto anchor = static_cast<qsizetype>(std::min(
        tableSelectionAnchor_.value_or(tableCursor_->utf16Offset),
        cell->text.size()));
    const auto start = std::min(anchor, focus);
    const auto end = std::max(anchor, focus);
    updated.replace(start, end - start, normalized);
    const TableCursor cursor{tableCursor_->tableId, tableCursor_->row,
                             tableCursor_->column,
                             static_cast<std::size_t>(start + normalized.size())};
    return apply({core::SetTableCellText{
                      cursor.tableId, cursor.row, cursor.column,
                      toUtf16(updated), typingFormat_}},
                 std::nullopt, std::nullopt, coalesceTyping, std::nullopt,
                 false, true, cursor, cursor.tableId);
}

bool DocumentCanvas::moveActiveTableCell(bool forward) {
    if (!tableCursor_) return false;
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(tableCursor_->tableId);
    if (!table) return false;
    const std::size_t current = tableCursor_->row * table->columnCount() +
                                tableCursor_->column;
    const std::size_t count = table->rowCount() * table->columnCount();
    if (forward && current + 1 >= count) {
        if (table->rowCount() >= core::Table::maximum_rows) {
            emit operationFailed(tr(
                "This table already has the maximum number of rows."));
            return false;
        }
        std::vector<core::NodeId> cellIds;
        cellIds.reserve(table->columnCount());
        for (std::size_t column = 0; column < table->columnCount(); ++column) {
            cellIds.push_back(core::NodeId::generate());
        }
        const TableCursor cursor{table->id(), table->rowCount(), 0, 0};
        const bool moved = apply(
            {core::AppendTableRow{table->id(), std::move(cellIds)}},
            std::nullopt, std::nullopt, false, std::nullopt,
            false, true, cursor, table->id());
        if (moved) clearPendingSpellingWord();
        return moved;
    }
    if (!forward && current == 0) {
        return true;
    }
    const std::size_t next = forward ? current + 1 : current - 1;
    return activateTableCell(table->id(), next / table->columnCount(),
                             next % table->columnCount(), 0);
}

bool DocumentCanvas::moveSelectedTable(bool forward) {
    if (!selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto& blocks = snap.document.bodyBlocks();
    const auto found = std::find_if(
        blocks.begin(), blocks.end(), [this](const core::BodyBlockRef& block) {
            return block.kind == core::BodyBlockKind::table &&
                   block.id == *selectedTable_;
        });
    if (found == blocks.end()) return false;
    const auto index = static_cast<std::size_t>(std::distance(blocks.begin(), found));
    std::optional<core::NodeId> before;
    if (forward) {
        if (index + 1 >= blocks.size()) return true;
        if (index + 2 < blocks.size()) before = blocks[index + 2].id;
    } else {
        if (index == 0) return true;
        before = blocks[index - 1].id;
    }
    const auto tableId = *selectedTable_;
    const auto cellCursor = tableCursor_;
    return apply({core::MoveTable{tableId, before}}, std::nullopt,
                 std::nullopt, false, std::nullopt, false, true,
                 cellCursor, tableId);
}

bool DocumentCanvas::deleteSelectedTable() {
    if (!selectedTable_) return false;
    const auto tableId = *selectedTable_;
    return apply({core::DeleteTable{tableId}}, std::nullopt, std::nullopt,
                 false, std::nullopt, false, true, std::nullopt,
                 std::nullopt);
}

bool DocumentCanvas::clearSelectedTableCells() {
    if (!selectedTable_ || !tableCellSelection_ ||
        rejectLiveEditDuringPreview()) {
        return false;
    }
    const auto snap = session_->snapshot();
    const auto* table = snap.document.findTable(*selectedTable_);
    if (!table) return false;
    std::vector<core::Operation> operations;
    for (const auto& [row, column] : selectedTableCells(*table)) {
        const auto* cell = table->cell(row, column);
        if (cell && !cell->text.empty()) {
            operations.emplace_back(core::SetTableCellText{
                table->id(), row, column, {}});
        }
    }
    if (operations.empty()) return true;
    return apply(std::move(operations));
}

void DocumentCanvas::toggleBullets() { togglePlainTextList(false); }

void DocumentCanvas::toggleNumbering() { togglePlainTextList(true); }

bool DocumentCanvas::hasActiveList() const {
    if (selectedTable_ || tableCursor_) return false;
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) return false;
    std::optional<core::NodeId> selectedListId;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        if (!plainTextListMarker(fromUtf16(paragraph.text()))) return false;
        if (normalized.value().start_paragraph_index ==
            normalized.value().end_paragraph_index) {
            continue;
        }
        if (!paragraph.format().list_id) return false;
        if (!selectedListId) selectedListId = paragraph.format().list_id;
        else if (selectedListId != paragraph.format().list_id) return false;
    }
    return true;
}

int DocumentCanvas::activeListLevel() const {
    if (!hasActiveList()) return 0;
    const auto snap = session_->snapshot();
    const auto* paragraph = snap.document.findParagraph(
        selection_.focus.paragraph_id);
    if (paragraph && paragraph->format().list_level) {
        return static_cast<int>(*paragraph->format().list_level) + 1;
    }
    const auto marker = plainTextListMarker(
        paragraphText(selection_.focus.paragraph_id));
    return marker
        ? static_cast<int>(listLevelForMarker(*marker, tabWidthSpaces_)) + 1
        : 0;
}

std::optional<core::ListLayout> DocumentCanvas::currentListLayout() const {
    if (!hasActiveList()) return std::nullopt;
    const auto snap = session_->snapshot();
    const auto* paragraph = snap.document.findParagraph(
        selection_.focus.paragraph_id);
    if (paragraph && paragraph->format().list_layout) {
        return paragraph->format().list_layout;
    }
    return core::ListLayout{};
}

bool DocumentCanvas::setCurrentListLayout(const core::ListLayout& layout) {
    if (selectedTable_ || rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    const auto focusIndex = snap.document.paragraphIndex(
        selection_.focus.paragraph_id);
    if (!focusIndex) return false;
    const auto focusMarker = plainTextListMarker(fromUtf16(
        snap.document.paragraphs()[*focusIndex].text()));
    if (!focusMarker) return false;

    const auto& focusFormat = snap.document.paragraphs()[*focusIndex].format();
    core::NodeId listId = focusFormat.list_id.value_or(
        core::NodeId::generate());
    std::vector<std::size_t> indices;
    if (focusFormat.list_id) {
        const auto isSameVisibleListItem = [&](std::size_t index) {
            const auto& paragraph = snap.document.paragraphs()[index];
            const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
            return marker && marker->kind == focusMarker->kind &&
                   paragraph.format().list_id == listId;
        };
        std::size_t first = *focusIndex;
        while (first > 0 && isSameVisibleListItem(first - 1)) --first;
        std::size_t last = *focusIndex;
        while (last + 1 < snap.document.paragraphs().size() &&
               isSameVisibleListItem(last + 1)) {
            ++last;
        }
        for (std::size_t index = first; index <= last; ++index) {
            indices.push_back(index);
        }

        // A marker removal or kind conversion can divide a former list into
        // multiple visible runs. Give the edited run a fresh identity so
        // later properties and marker-width calculations cannot leak across
        // the intervening ordinary paragraph or differently styled list.
        bool sameIdentityOutsideRun = false;
        for (std::size_t index = 0;
             index < snap.document.paragraphs().size(); ++index) {
            if ((index < first || index > last) &&
                snap.document.paragraphs()[index].format().list_id == listId) {
                sameIdentityOutsideRun = true;
                break;
            }
        }
        if (sameIdentityOutsideRun) listId = core::NodeId::generate();
    } else {
        std::size_t first = *focusIndex;
        while (first > 0) {
            const auto marker = plainTextListMarker(fromUtf16(
                snap.document.paragraphs()[first - 1].text()));
            if (!marker || marker->kind != focusMarker->kind) break;
            --first;
        }
        std::size_t last = *focusIndex;
        while (last + 1 < snap.document.paragraphs().size()) {
            const auto marker = plainTextListMarker(fromUtf16(
                snap.document.paragraphs()[last + 1].text()));
            if (!marker || marker->kind != focusMarker->kind) break;
            ++last;
        }
        for (std::size_t index = first; index <= last; ++index) {
            indices.push_back(index);
        }
    }
    if (indices.empty()) return false;

    struct PrefixChange {
        core::NodeId paragraphId;
        std::size_t oldLength{};
        std::size_t newLength{};
    };
    std::vector<PrefixChange> changes;
    std::vector<core::Operation> operations;
    std::vector<core::NodeId> existingIds;
    for (const auto index : indices) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (!marker) continue;
        const std::size_t level = paragraph.format().list_level
            ? static_cast<std::size_t>(*paragraph.format().list_level)
            : listLevelForMarker(*marker, tabWidthSpaces_);
        const QString replacement = listPrefix(
            marker->kind, marker->marker, level, layout);
        const auto oldLength = static_cast<std::size_t>(marker->prefixLength);
        const auto newLength = static_cast<std::size_t>(replacement.size());
        changes.push_back({paragraph.id(), oldLength, newLength});
        if (fromUtf16(paragraph.text()).first(marker->prefixLength) !=
            replacement) {
            operations.push_back(core::ReplaceRange{
                {{paragraph.id(), 0}, {paragraph.id(), oldLength}},
                toUtf16(replacement), paragraph.characterFormatAt(0)});
        }
        if (paragraph.format().list_id) {
            existingIds.push_back(paragraph.id());
        } else {
            operations.push_back(core::SetParagraphFormat{
                {paragraph.id()}, semanticListDelta(listId, level, layout)});
        }
    }
    if (!existingIds.empty()) {
        core::ParagraphFormatDelta delta;
        if (focusFormat.list_id && listId != *focusFormat.list_id) {
            delta.list_id = core::PropertyDelta<core::NodeId>::set(listId);
        }
        delta.list_layout =
            core::PropertyDelta<core::ListLayout>::set(layout);
        operations.push_back(core::SetParagraphFormat{
            std::move(existingIds), std::move(delta)});
    }

    const auto adjustPosition = [&changes](core::Position position) {
        const auto found = std::find_if(
            changes.begin(), changes.end(), [&position](const PrefixChange& change) {
                return change.paragraphId == position.paragraph_id;
            });
        if (found == changes.end()) return position;
        if (position.utf16_offset <= found->oldLength) {
            position.utf16_offset = found->newLength;
        } else {
            position.utf16_offset = position.utf16_offset - found->oldLength +
                                    found->newLength;
        }
        return position;
    };
    const core::Range adjusted{
        adjustPosition(selection_.anchor), adjustPosition(selection_.focus)};
    return apply(std::move(operations), std::nullopt, std::nullopt, false,
                 adjusted);
}

void DocumentCanvas::togglePlainTextList(bool numbered) {
    if (selectedTable_) {
        emit operationFailed(tr(
            "Lists inside table cells are not supported in this release."));
        return;
    }
    if (rejectLiveEditDuringPreview()) return;
    endTypingGroup();
    resetVerticalNavigation();

    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return;
    }

    const auto targetKind = numbered ? PlainTextListMarker::Kind::numbered
                                     : PlainTextListMarker::Kind::bullet;
    bool removeList = true;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        const auto marker = plainTextListMarker(
            fromUtf16(snap.document.paragraphs()[index].text()));
        if (!marker || marker->kind != targetKind) {
            removeList = false;
            break;
        }
    }

    core::NodeId targetListId = core::NodeId::generate();
    core::ListLayout targetLayout = defaultListLayout_;
    if (!removeList) {
        const auto& firstParagraph = snap.document.paragraphs()[
            normalized.value().start_paragraph_index];
        if (firstParagraph.format().list_id &&
            firstParagraph.format().list_layout) {
            const auto candidateId = *firstParagraph.format().list_id;
            bool oneExistingList = true;
            for (std::size_t index = normalized.value().start_paragraph_index;
                 index <= normalized.value().end_paragraph_index; ++index) {
                if (snap.document.paragraphs()[index].format().list_id !=
                    candidateId) {
                    oneExistingList = false;
                    break;
                }
            }
            if (oneExistingList) {
                targetListId = candidateId;
                targetLayout = *firstParagraph.format().list_layout;
            }
        }
    }

    struct PrefixChange {
        core::NodeId paragraphId;
        std::size_t oldLength;
        std::size_t newLength;
    };
    std::vector<core::Operation> operations;
    std::vector<PrefixChange> changes;
    std::array<qulonglong, core::kListLevelCount> ordinals{};
    static const QRegularExpression leadingIndent(QStringLiteral(R"(^([ \t]*))"));

    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const QString text = fromUtf16(paragraph.text());
        const auto marker = plainTextListMarker(text);
        std::size_t oldLength = 0;
        std::size_t level = 0;
        if (marker) {
            oldLength = static_cast<std::size_t>(marker->prefixLength);
            level = paragraph.format().list_level
                ? static_cast<std::size_t>(*paragraph.format().list_level)
                : listLevelForMarker(*marker, tabWidthSpaces_);
        } else {
            const auto indentMatch = leadingIndent.match(text);
            const QString indent = indentMatch.captured(1);
            oldLength = static_cast<std::size_t>(indent.size());
            level = std::min<std::size_t>(
                core::kListLevelCount - 1,
                static_cast<std::size_t>(std::max(
                    0, indentationColumns(indent, tabWidthSpaces_) /
                           std::max(1, tabWidthSpaces_))));
        }

        QString replacement;
        if (!removeList) {
            for (std::size_t deeper = level + 1;
                 deeper < ordinals.size(); ++deeper) {
                ordinals[deeper] = 0;
            }
            ++ordinals[level];
            const QString listMarker = numbered
                ? numberedMarker(ordinals[level], level)
                : bulletForLevel(level);
            replacement = listPrefix(targetKind, listMarker, level,
                                     targetLayout);
        }

        const std::size_t newLength = static_cast<std::size_t>(replacement.size());
        changes.push_back({paragraph.id(), oldLength, newLength});
        const QString oldPrefix = text.first(static_cast<qsizetype>(oldLength));
        if (oldPrefix != replacement) {
            const auto replacementFormat =
                normalized.value().empty() && marker && marker->emptyItem
                ? std::optional<core::CharacterFormat>(typingFormat_)
                : std::optional<core::CharacterFormat>(
                      paragraph.characterFormatAt(0));
            operations.push_back(core::ReplaceRange{
                {{paragraph.id(), 0}, {paragraph.id(), oldLength}},
                toUtf16(replacement), replacementFormat});
        }
        operations.push_back(core::SetParagraphFormat{
            {paragraph.id()}, removeList
                                ? clearSemanticListDelta()
                                : semanticListDelta(targetListId, level,
                                                    targetLayout)});
    }

    auto adjustPosition = [&changes](core::Position position) {
        const auto found = std::find_if(
            changes.begin(), changes.end(), [&position](const PrefixChange& change) {
                return change.paragraphId == position.paragraph_id;
            });
        if (found == changes.end()) return position;
        if (position.utf16_offset <= found->oldLength) {
            position.utf16_offset = found->newLength;
        } else {
            position.utf16_offset = position.utf16_offset - found->oldLength +
                                    found->newLength;
        }
        return position;
    };

    const core::Range adjustedSelection{
        adjustPosition(selection_.anchor), adjustPosition(selection_.focus)};
    static_cast<void>(apply(std::move(operations), std::nullopt, std::nullopt,
                            false, adjustedSelection));
}

bool DocumentCanvas::continuePlainTextList() {
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized || normalized.value().start_paragraph_index !=
                           normalized.value().end_paragraph_index) {
        return false;
    }

    const auto& rangeStart = normalized.value().start;
    const QString text = paragraphText(rangeStart.paragraph_id);
    const auto marker = plainTextListMarker(text);
    if (!marker || rangeStart.utf16_offset <
                       static_cast<std::size_t>(marker->prefixLength)) {
        return false;
    }

    if (normalized.value().empty() && marker->emptyItem &&
        selection_.focus.utf16_offset ==
                                 static_cast<std::size_t>(text.size())) {
        const core::Position start{selection_.focus.paragraph_id, 0};
        const core::Position end{selection_.focus.paragraph_id,
                                 static_cast<std::size_t>(text.size())};
        apply({core::DeleteRange{
                   {start, end},
                   std::optional<core::CharacterFormat>(typingFormat_)},
               core::SetParagraphFormat{{start.paragraph_id},
                                        clearSemanticListDelta()}},
              start);
        return true;
    }

    const auto* paragraph = snap.document.findParagraph(rangeStart.paragraph_id);
    if (!paragraph) return false;
    const core::NodeId listId = paragraph->format().list_id.value_or(
        core::NodeId::generate());
    const core::ListLayout listLayout = paragraph->format().list_layout.value_or(
        core::ListLayout{});
    const std::size_t level = paragraph->format().list_level
        ? static_cast<std::size_t>(*paragraph->format().list_level)
        : listLevelForMarker(*marker, tabWidthSpaces_);
    const auto nextMarker = continuationMarker(*marker, level);
    if (!nextMarker) {
        return false;
    }
    const QString currentPrefix = listPrefix(
        marker->kind, marker->marker, level, listLayout);
    const QString nextPrefix = listPrefix(
        marker->kind, *nextMarker, level, listLayout);

    const std::size_t oldPrefixLength =
        static_cast<std::size_t>(marker->prefixLength);
    const std::size_t currentPrefixLength =
        static_cast<std::size_t>(currentPrefix.size());
    const auto adjustOffset = [oldPrefixLength, currentPrefixLength](
                                  std::size_t offset) {
        if (offset <= oldPrefixLength) return currentPrefixLength;
        return offset - oldPrefixLength + currentPrefixLength;
    };
    const core::Range adjustedRange{
        {selection_.anchor.paragraph_id,
         adjustOffset(selection_.anchor.utf16_offset)},
        {selection_.focus.paragraph_id,
         adjustOffset(selection_.focus.utf16_offset)}};
    const core::Position splitPosition{
        rangeStart.paragraph_id, adjustOffset(rangeStart.utf16_offset)};
    const auto newId = core::NodeId::generate();
    std::vector<core::Operation> operations;
    if (text.first(marker->prefixLength) != currentPrefix) {
        operations.push_back(core::ReplaceRange{
            {{paragraph->id(), 0}, {paragraph->id(), oldPrefixLength}},
            toUtf16(currentPrefix), paragraph->characterFormatAt(0)});
    }
    if (!normalized.value().empty()) {
        operations.push_back(core::DeleteRange{adjustedRange});
    }
    operations.push_back(core::SplitParagraph{
        splitPosition, newId,
        std::optional<core::CharacterFormat>(typingFormat_)});
    operations.push_back(core::InsertText{
        {newId, 0}, toUtf16(nextPrefix),
        std::optional<core::CharacterFormat>(typingFormat_)});
    if (!paragraph->format().list_id) {
        const auto delta = semanticListDelta(listId, level, listLayout);
        operations.push_back(core::SetParagraphFormat{
            {paragraph->id(), newId}, delta});
    }
    if (!apply(std::move(operations),
               core::Position{newId,
                              static_cast<std::size_t>(nextPrefix.size())})) {
        return false;
    }
    if (marker->kind == PlainTextListMarker::Kind::numbered) {
        // The split is already committed. A defensive resequencing failure
        // must not report Enter as unhandled and cause keyPressEvent to insert
        // a second ordinary paragraph.
        static_cast<void>(resequenceNumberedList(newId, true));
    }
    return true;
}

bool DocumentCanvas::changeListLevel(bool outdent) {
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        return false;
    }

    const auto& paragraphs = snap.document.paragraphs();
    const std::size_t selectedFirst =
        normalized.value().start_paragraph_index;
    const std::size_t selectedLast =
        normalized.value().end_paragraph_index;
    const auto firstMarker = plainTextListMarker(
        fromUtf16(paragraphs[selectedFirst].text()));
    if (!firstMarker) return false;
    for (std::size_t index = selectedFirst; index <= selectedLast; ++index) {
        const auto marker = plainTextListMarker(
            fromUtf16(paragraphs[index].text()));
        if (!marker || marker->kind != firstMarker->kind) return false;
    }

    // Numbered lists are counters, not decorated paragraphs. When an item's
    // level changes, resequence the entire visible list so the first child is
    // A/I/a/i (depending on its level) and later siblings remain contiguous.
    // Bullets have no counters, so only the selected bullet paragraphs need
    // rewriting.
    std::size_t processFirst = selectedFirst;
    std::size_t processLast = selectedLast;
    const bool numbered =
        firstMarker->kind == PlainTextListMarker::Kind::numbered;
    const auto selectedListId = paragraphs[selectedFirst].format().list_id;
    const auto belongsToSameVisibleList = [&](std::size_t index) {
        const auto marker = plainTextListMarker(
            fromUtf16(paragraphs[index].text()));
        if (!marker || marker->kind != firstMarker->kind) return false;
        const auto candidateId = paragraphs[index].format().list_id;
        return selectedListId ? candidateId == selectedListId
                              : !candidateId.has_value();
    };
    if (numbered) {
        while (processFirst > 0 &&
               belongsToSameVisibleList(processFirst - 1)) {
            --processFirst;
        }
        while (processLast + 1 < paragraphs.size() &&
               belongsToSameVisibleList(processLast + 1)) {
            ++processLast;
        }
    }

    struct PrefixChange {
        core::NodeId paragraphId;
        std::size_t oldLength;
        std::size_t newLength;
    };
    std::vector<core::Operation> operations;
    std::vector<PrefixChange> changes;
    const core::NodeId listId = selectedListId.value_or(
        core::NodeId::generate());
    const core::ListLayout sharedLayout =
        paragraphs[selectedFirst].format().list_layout.value_or(
            core::ListLayout{});
    std::array<qulonglong, core::kListLevelCount> counters{};
    std::array<bool, core::kListLevelCount> restartAtOne{};
    for (std::size_t index = processFirst; index <= processLast; ++index) {
        const auto& paragraph = paragraphs[index];
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (!marker || marker->kind != firstMarker->kind) return false;

        const auto& paragraphFormat = paragraph.format();
        const std::size_t oldLevel = paragraphFormat.list_level
            ? std::min<std::size_t>(*paragraphFormat.list_level,
                                    core::kListLevelCount - 1)
            : listLevelForMarker(*marker, tabWidthSpaces_);
        const bool selected = index >= selectedFirst && index <= selectedLast;
        const std::size_t newLevel = !selected
            ? oldLevel
            : outdent
                ? (oldLevel == 0 ? 0 : oldLevel - 1)
                : std::min(oldLevel + 1, core::kListLevelCount - 1);
        const core::ListLayout layout = paragraphFormat.list_layout.value_or(
            sharedLayout);
        QString markerAtNewLevel = marker->marker;
        if (numbered) {
            for (std::size_t deeper = newLevel + 1;
                 deeper < counters.size(); ++deeper) {
                counters[deeper] = 0;
                restartAtOne[deeper] = true;
            }
            if (counters[newLevel] == 0) {
                const bool movedToAnotherLevel = selected && newLevel != oldLevel;
                const bool startsNewSequence =
                    movedToAnotherLevel || restartAtOne[newLevel];
                counters[newLevel] = startsNewSequence
                    ? 1U
                    : numberedOrdinal(marker->marker, oldLevel).value_or(1U);
                restartAtOne[newLevel] = false;
            } else if (counters[newLevel] <
                       std::numeric_limits<qulonglong>::max()) {
                ++counters[newLevel];
            }
            markerAtNewLevel = numberedMarker(
                counters[newLevel], newLevel, marker->marker.back());
        }
        const QString replacement = listPrefix(
            marker->kind, markerAtNewLevel, newLevel, layout);
        const auto oldLength = static_cast<std::size_t>(marker->prefixLength);
        const auto newLength = static_cast<std::size_t>(replacement.size());
        changes.push_back({paragraph.id(), oldLength, newLength});
        if (fromUtf16(paragraph.text()).first(marker->prefixLength) !=
            replacement) {
            operations.push_back(core::ReplaceRange{
                {{paragraph.id(), 0}, {paragraph.id(), oldLength}},
                toUtf16(replacement), paragraph.characterFormatAt(0)});
        }
        core::ParagraphFormatDelta delta;
        if (paragraphFormat.list_id == listId &&
            paragraphFormat.list_layout && paragraphFormat.list_level) {
            if (!selected || newLevel == oldLevel) continue;
            delta.list_level = core::PropertyDelta<std::uint8_t>::set(
                static_cast<std::uint8_t>(newLevel));
        } else {
            delta = semanticListDelta(listId, newLevel, layout);
        }
        operations.push_back(core::SetParagraphFormat{
            {paragraph.id()}, std::move(delta)});
    }

    auto adjustPosition = [&changes](core::Position position) {
        const auto found = std::find_if(
            changes.begin(), changes.end(), [&position](const PrefixChange& change) {
                return change.paragraphId == position.paragraph_id;
            });
        if (found == changes.end()) return position;
        if (position.utf16_offset <= found->oldLength) {
            position.utf16_offset = found->newLength;
        } else {
            position.utf16_offset = position.utf16_offset - found->oldLength +
                                    found->newLength;
        }
        return position;
    };
    const core::Range adjustedSelection{
        adjustPosition(selection_.anchor), adjustPosition(selection_.focus)};
    if (operations.empty()) return true;
    static_cast<void>(apply(std::move(operations), std::nullopt, std::nullopt,
                            false, adjustedSelection));
    return true;
}

bool DocumentCanvas::resequenceNumberedList(
    core::NodeId paragraphId, bool coalesceWithPrevious) {
    const auto snap = session_->snapshot();
    const auto anchorIndex = snap.document.paragraphIndex(paragraphId);
    if (!anchorIndex) return true;
    const auto& paragraphs = snap.document.paragraphs();
    const auto anchorMarker = plainTextListMarker(
        fromUtf16(paragraphs[*anchorIndex].text()));
    if (!anchorMarker ||
        anchorMarker->kind != PlainTextListMarker::Kind::numbered) {
        return true;
    }

    const auto anchorListId = paragraphs[*anchorIndex].format().list_id;
    const auto belongsToList = [&](std::size_t index) {
        const auto marker = plainTextListMarker(
            fromUtf16(paragraphs[index].text()));
        if (!marker || marker->kind != PlainTextListMarker::Kind::numbered) {
            return false;
        }
        const auto candidateId = paragraphs[index].format().list_id;
        return anchorListId ? (!candidateId || candidateId == anchorListId)
                            : !candidateId.has_value();
    };
    std::size_t first = *anchorIndex;
    while (first > 0 && belongsToList(first - 1)) --first;
    std::size_t last = *anchorIndex;
    while (last + 1 < paragraphs.size() && belongsToList(last + 1)) ++last;

    const core::NodeId listId = anchorListId.value_or(core::NodeId::generate());
    const core::ListLayout sharedLayout =
        paragraphs[*anchorIndex].format().list_layout.value_or(
            core::ListLayout{});
    struct PrefixChange {
        core::NodeId paragraphId;
        std::size_t oldLength;
        std::size_t newLength;
    };
    std::vector<PrefixChange> changes;
    std::vector<core::Operation> operations;
    std::array<qulonglong, core::kListLevelCount> counters{};
    std::array<bool, core::kListLevelCount> restartAtOne{};
    for (std::size_t index = first; index <= last; ++index) {
        const auto& paragraph = paragraphs[index];
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (!marker) return false;
        const auto& format = paragraph.format();
        const std::size_t level = format.list_level
            ? std::min<std::size_t>(*format.list_level,
                                    core::kListLevelCount - 1)
            : listLevelForMarker(*marker, tabWidthSpaces_);
        for (std::size_t deeper = level + 1;
             deeper < counters.size(); ++deeper) {
            counters[deeper] = 0;
            restartAtOne[deeper] = true;
        }
        if (counters[level] == 0) {
            counters[level] = restartAtOne[level]
                ? 1U
                : numberedOrdinal(marker->marker, level).value_or(1U);
            restartAtOne[level] = false;
        } else if (counters[level] <
                   std::numeric_limits<qulonglong>::max()) {
            ++counters[level];
        }

        const core::ListLayout layout = format.list_layout.value_or(
            sharedLayout);
        const QString expectedPrefix = listPrefix(
            PlainTextListMarker::Kind::numbered,
            numberedMarker(counters[level], level, marker->marker.back()),
            level, layout);
        const std::size_t oldLength =
            static_cast<std::size_t>(marker->prefixLength);
        const std::size_t newLength =
            static_cast<std::size_t>(expectedPrefix.size());
        changes.push_back({paragraph.id(), oldLength, newLength});
        if (fromUtf16(paragraph.text()).first(marker->prefixLength) !=
            expectedPrefix) {
            operations.push_back(core::ReplaceRange{
                {{paragraph.id(), 0}, {paragraph.id(), oldLength}},
                toUtf16(expectedPrefix), paragraph.characterFormatAt(0)});
        }
        if (format.list_id != listId || !format.list_level ||
            !format.list_layout) {
            operations.push_back(core::SetParagraphFormat{
                {paragraph.id()}, semanticListDelta(listId, level, layout)});
        }
    }
    if (operations.empty()) return true;

    const auto adjustPosition = [&changes](core::Position position) {
        const auto found = std::find_if(
            changes.begin(), changes.end(), [&position](const auto& change) {
                return change.paragraphId == position.paragraph_id;
            });
        if (found == changes.end()) return position;
        position.utf16_offset = position.utf16_offset <= found->oldLength
            ? found->newLength
            : position.utf16_offset - found->oldLength + found->newLength;
        return position;
    };
    const core::Range adjustedSelection{
        adjustPosition(selection_.anchor), adjustPosition(selection_.focus)};
    return apply(std::move(operations), std::nullopt, std::nullopt, false,
                 adjustedSelection, coalesceWithPrevious);
}

bool DocumentCanvas::handlePlainTextListBackspace() {
    if (selection_.anchor != selection_.focus) {
        return false;
    }

    const QString text = paragraphText(selection_.focus.paragraph_id);
    const auto marker = plainTextListMarker(text);
    if (!marker || selection_.focus.utf16_offset == 0 ||
        selection_.focus.utf16_offset >
            static_cast<std::size_t>(marker->prefixLength)) {
        return false;
    }

    const auto snap = session_->snapshot();
    const auto* paragraph = snap.document.findParagraph(
        selection_.focus.paragraph_id);
    const std::size_t level = paragraph && paragraph->format().list_level
        ? static_cast<std::size_t>(*paragraph->format().list_level)
        : listLevelForMarker(*marker, tabWidthSpaces_);
    if (level > 0) {
        return changeListLevel(true);
    }

    const core::Position start{selection_.focus.paragraph_id, 0};
    const core::Position end{
        selection_.focus.paragraph_id,
        static_cast<std::size_t>(marker->prefixLength)};
    apply({core::DeleteRange{
               {start, end},
               std::optional<core::CharacterFormat>(typingFormat_)},
           core::SetParagraphFormat{{start.paragraph_id},
                                    clearSemanticListDelta()}},
          start);
    return true;
}

void DocumentCanvas::undo() {
    endTypingGroup();
    clearPendingSpellingWord();
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    synchronizeCursorHistory();
    if (undoCursorHistory_.empty()) {
        return;
    }
    auto entry = undoCursorHistory_.back();
    if (entry.documentTransaction) {
        const auto snap = session_->snapshot();
        const auto result = session_->undo(snap.revision);
        if (!result) {
            if (result.error().code != core::ErrorCode::history_empty) {
                emit operationFailed(errorText(result.error()));
            }
            return;
        }
        reconcileDecodedImageCache();
    }
    undoCursorHistory_.pop_back();
    restoreEditorState(entry.before);
    redoCursorHistory_.push_back(std::move(entry));
    synchronizeCursorHistory();
    invalidateLayout();
    emit documentChanged(session_->snapshot().revision.value());
    emit selectionChanged();
    emitCursorFormat();
    viewport()->update();
    updateStatus();
    if (headerFooterEditing_) {
        loadStoryEditors(activeStoryIsFooter_);
        storyEditHasTransaction_ = false;
        updateStoryEditorGeometry();
    } else {
        revealCursor();
    }
}

void DocumentCanvas::redo() {
    endTypingGroup();
    clearPendingSpellingWord();
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    synchronizeCursorHistory();
    if (redoCursorHistory_.empty()) {
        return;
    }
    auto entry = redoCursorHistory_.back();
    if (entry.documentTransaction) {
        const auto snap = session_->snapshot();
        const auto result = session_->redo(snap.revision);
        if (!result) {
            if (result.error().code != core::ErrorCode::history_empty) {
                emit operationFailed(errorText(result.error()));
            }
            return;
        }
        reconcileDecodedImageCache();
    }
    redoCursorHistory_.pop_back();
    restoreEditorState(entry.after);
    undoCursorHistory_.push_back(std::move(entry));
    synchronizeCursorHistory();
    invalidateLayout();
    emit documentChanged(session_->snapshot().revision.value());
    emit selectionChanged();
    emitCursorFormat();
    viewport()->update();
    updateStatus();
    if (headerFooterEditing_) {
        loadStoryEditors(activeStoryIsFooter_);
        storyEditHasTransaction_ = false;
        updateStoryEditorGeometry();
    } else {
        revealCursor();
    }
}

void DocumentCanvas::copy() {
    if (previewId_ || !hasClipboardSelection()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    if (const auto selected = selectedInlineImage()) {
        const QByteArray native = encodeClipboardInlineImage(selected->second);
        const QByteArray legacy = encodeClipboardInlineImage(
            selected->second, true);
        if (native.isEmpty()) return;
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(kInlineImageClipboardMime), native);
        if (!legacy.isEmpty()) {
            mime->setData(
                QString::fromLatin1(kInlineImageClipboardLegacyMime), legacy);
        }
        const auto payload = selected->second.encoded_payload.bytes();
        const QByteArray encoded(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<qsizetype>(payload.size()));
        const QString standardMime = selected->second.format ==
                core::ImageFormat::png
            ? QStringLiteral("image/png")
            : QStringLiteral("image/jpeg");
        mime->setData(standardMime, encoded);
        mime->setText(QString::fromStdString(
            selected->second.accessible_name));
        QApplication::clipboard()->setMimeData(mime);
        return;
    }
    const auto text = selectedText();
    if (!text.isEmpty()) {
        QApplication::clipboard()->setText(text);
    }
}

void DocumentCanvas::cut() {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    if (!hasClipboardSelection()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    if (selectedTable_) {
        copy();
        if (tableCursor_) {
            static_cast<void>(replaceTableCellText({}, false));
        } else if (tableCellSelection_) {
            static_cast<void>(clearSelectedTableCells());
        } else {
            static_cast<void>(deleteSelectedTable());
        }
        return;
    }
    if (selection_.anchor == selection_.focus) {
        return;
    }
    copy();
    replaceSelection({});
}

void DocumentCanvas::paste() {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (headerFooterEditing_) {
        auto& editors = activeStoryIsFooter_ ? footerEditors_ : headerEditors_;
        auto* target = editors[static_cast<std::size_t>(
            std::clamp(activeStoryRegion_, 0, 2))];
        const bool hasPicture = mime &&
            (mime->hasFormat(QString::fromLatin1(
                 kInlineImageClipboardMime)) ||
             mime->hasFormat(QString::fromLatin1(
                 kInlineImageClipboardLegacyMime)) ||
             mime->hasFormat(QStringLiteral("image/png")) ||
             mime->hasFormat(QStringLiteral("image/jpeg")) ||
             mime->hasImage());
        if (hasPicture) {
            static_cast<void>(pasteStoryImage(target, mime));
        } else if (target) {
            target->paste();
        }
        return;
    }
    if (!tableCursor_ && !selectedTable_ && mime &&
        (mime->hasFormat(QString::fromLatin1(kInlineImageClipboardMime)) ||
         mime->hasFormat(
             QString::fromLatin1(kInlineImageClipboardLegacyMime)))) {
        const QString nativeFormat = mime->hasFormat(
            QString::fromLatin1(kInlineImageClipboardMime))
            ? QString::fromLatin1(kInlineImageClipboardMime)
            : QString::fromLatin1(kInlineImageClipboardLegacyMime);
        const auto image = decodeClipboardInlineImage(
            mime->data(nativeFormat));
        if (!image) {
            emit operationFailed(tr(
                "The Owl Docs picture on the clipboard is malformed or exceeds the safety limits."));
            return;
        }
        static_cast<void>(insertInlineImageWithGeometry(
            image->encodedBytes, image->accessibleName,
            image->widthEmu, image->heightEmu, image->layout));
        return;
    }
    if (!tableCursor_ && !selectedTable_ && mime) {
        for (const QString& format : {QStringLiteral("image/png"),
                                      QStringLiteral("image/jpeg")}) {
            if (!mime->hasFormat(format)) continue;
            const QByteArray encoded = mime->data(format);
            if (encoded.isEmpty() ||
                encoded.size() > static_cast<qsizetype>(
                    core::kMaximumEncodedImageBytes)) {
                emit operationFailed(tr(
                    "The clipboard picture is empty or exceeds the 16 MiB encoded-picture limit."));
                return;
            }
            std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(encoded.constData()),
                reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
                    encoded.size());
            static_cast<void>(insertInlineImage(
                std::move(bytes), tr("Pasted picture")));
            return;
        }
    }
    if (tableCursor_) {
        static_cast<void>(replaceTableCellText(
            QApplication::clipboard()->text(), false));
    } else if (selectedTable_) {
        emit operationFailed(tr(
            "Press Enter or F2 before pasting into the selected table."));
    } else {
        replaceSelection(QApplication::clipboard()->text());
    }
}

void DocumentCanvas::pasteTextOnly() {
    if (rejectLiveEditDuringPreview()) {
        return;
    }

    // Read the clipboard exactly once before changing selection or document
    // state. In particular, do not fall back to image alt text, HTML, or a
    // native Owl Docs object when text/plain is unavailable.
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasText()) {
        return;
    }
    const QString text = mime->text();
    if (text.isEmpty()) {
        return;
    }

    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    if (tableCursor_) {
        static_cast<void>(replaceTableCellText(text, false));
        return;
    }
    if (selectedTable_ && tableCellSelection_) {
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(*selectedTable_);
        if (!table) return;
        const auto cells = selectedTableCells(*table);
        if (cells.empty()) return;

        QString normalized = text;
        normalized.replace(QStringLiteral("\r\n"),
                           QString(QChar::LineSeparator));
        normalized.replace(QLatin1Char('\r'), QChar::LineSeparator);
        normalized.replace(QLatin1Char('\n'), QChar::LineSeparator);

        // A rectangular cell range is one selection. Replace that selection
        // with the plain text in its top-left cell and clear the remaining
        // selected cells. The entire replacement is one core batch, hence one
        // normal Undo transaction. Tabs/newlines remain text and are never
        // interpreted as a pasted table.
        std::vector<core::Operation> operations;
        operations.reserve(cells.size());
        bool first = true;
        for (const auto& [row, column] : cells) {
            operations.emplace_back(core::SetTableCellText{
                table->id(), row, column,
                first ? toUtf16(normalized) : std::u16string{},
                first
                    ? std::optional<core::CharacterFormat>(typingFormat_)
                    : std::nullopt});
            first = false;
        }
        const auto [row, column] = cells.front();
        const TableCursor cursor{
            table->id(), row, column,
            static_cast<std::size_t>(normalized.size())};
        static_cast<void>(apply(
            std::move(operations), std::nullopt, std::nullopt, false,
            std::nullopt, false, true, cursor, table->id()));
        return;
    }
    if (selectedTable_) {
        emit operationFailed(tr(
            "Press Enter or F2 before pasting into the selected table."));
        return;
    }
    replaceSelection(text);
}

void DocumentCanvas::selectAll() {
    if (previewId_) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    if ((tableCursor_ || tableCellSelection_) && selectedTable_) {
        tableCursor_.reset();
        tableSelectionAnchor_.reset();
        tableCellSelection_.reset();
        tableMouseSelectionAnchor_.reset();
        typingOverrideMask_ = {};
        emit selectionChanged();
        emitCursorFormat();
        viewport()->update();
        return;
    }
    const auto snap = session_->snapshot();
    const auto& first = snap.document.paragraphs().front();
    const auto& last = snap.document.paragraphs().back();
    selection_ = {{first.id(), 0}, {last.id(), last.text().size()}};
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_.reset();
    typingFormat_ = selectedCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    emit selectionChanged();
    emitCursorFormat();
    viewport()->update();
}

void DocumentCanvas::deleteBackward(bool byWord) {
    if (selection_.anchor != selection_.focus) {
        replaceSelection({});
        return;
    }
    const auto snap = session_->snapshot();
    const auto index = snap.document.paragraphIndex(selection_.focus.paragraph_id);
    if (!index) {
        return;
    }
    const auto& paragraph = snap.document.paragraphs()[*index];
    if (selection_.focus.utf16_offset > 0) {
        const auto text = fromUtf16(paragraph.text());
        qsizetype previous = 0;
        if (byWord) {
            previous = previousWordStart(
                text, static_cast<qsizetype>(selection_.focus.utf16_offset));
        } else {
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
            finder.setPosition(static_cast<qsizetype>(selection_.focus.utf16_offset));
            previous = finder.toPreviousBoundary();
            if (previous < 0) {
                previous = static_cast<qsizetype>(selection_.focus.utf16_offset) - 1;
            }
        }
        core::Position start{paragraph.id(), static_cast<std::size_t>(previous)};
        const bool emptiesParagraph = start.utf16_offset == 0 &&
            selection_.focus.utf16_offset == paragraph.text().size();
        apply({core::DeleteRange{
                   {start, selection_.focus},
                   emptiesParagraph
                       ? std::optional<core::CharacterFormat>(typingFormat_)
                       : std::nullopt}},
              start);
    } else if (*index > 0) {
        const auto& previous = snap.document.paragraphs()[*index - 1];
        const core::Position cursor{previous.id(), previous.text().size()};
        std::vector<core::Operation> operations;
        const auto marker = plainTextListMarker(fromUtf16(paragraph.text()));
        if (marker) {
            operations.push_back(core::DeleteRange{{
                {paragraph.id(), 0},
                {paragraph.id(),
                 static_cast<std::size_t>(marker->prefixLength)}}});
        }
        operations.push_back(core::MergeWithNextParagraph{previous.id()});
        if (apply(std::move(operations), cursor)) {
            static_cast<void>(resequenceNumberedList(previous.id(), true));
        }
    }
}

void DocumentCanvas::deleteForward(bool byWord) {
    if (selection_.anchor != selection_.focus) {
        replaceSelection({});
        return;
    }
    const auto snap = session_->snapshot();
    const auto index = snap.document.paragraphIndex(selection_.focus.paragraph_id);
    if (!index) return;
    const auto& paragraph = snap.document.paragraphs()[*index];
    if (selection_.focus.utf16_offset < paragraph.text().size()) {
        const auto text = fromUtf16(paragraph.text());
        qsizetype next = text.size();
        if (byWord) {
            next = nextWordStart(
                text, static_cast<qsizetype>(selection_.focus.utf16_offset));
        } else {
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
            finder.setPosition(static_cast<qsizetype>(selection_.focus.utf16_offset));
            next = finder.toNextBoundary();
            if (next < 0) {
                next = static_cast<qsizetype>(selection_.focus.utf16_offset) + 1;
            }
        }
        const core::Position end{paragraph.id(), static_cast<std::size_t>(next)};
        const bool emptiesParagraph = selection_.focus.utf16_offset == 0 &&
            end.utf16_offset == paragraph.text().size();
        apply({core::DeleteRange{
                   {selection_.focus, end},
                   emptiesParagraph
                       ? std::optional<core::CharacterFormat>(typingFormat_)
                       : std::nullopt}},
              selection_.focus);
    } else if (*index + 1 < snap.document.paragraphs().size()) {
        const auto& nextParagraph = snap.document.paragraphs()[*index + 1];
        std::vector<core::Operation> operations;
        const auto marker = plainTextListMarker(fromUtf16(nextParagraph.text()));
        if (marker) {
            operations.push_back(core::DeleteRange{{
                {nextParagraph.id(), 0},
                {nextParagraph.id(),
                 static_cast<std::size_t>(marker->prefixLength)}}});
        }
        operations.push_back(core::MergeWithNextParagraph{paragraph.id()});
        if (apply(std::move(operations), selection_.focus)) {
            static_cast<void>(resequenceNumberedList(paragraph.id(), true));
        }
    }
}

void DocumentCanvas::moveHorizontal(bool forward, bool extend, bool byWord) {
    const auto snap = session_->snapshot();
    if (!extend && selection_.anchor != selection_.focus) {
        const auto normalized = snap.document.normalizeRange(selection_);
        if (normalized) {
            core::Position target =
                forward ? normalized.value().end : normalized.value().start;

            // Left/Right are physical directions.  For a single visual line,
            // collapse an RTL or mixed-direction selection to the endpoint on
            // that side instead of assuming that increasing UTF-16 offsets
            // always move right.  Multi-line/cross-paragraph selections keep
            // the existing document-order fallback.
            if (selection_.anchor.paragraph_id ==
                    selection_.focus.paragraph_id) {
                ensureLayout();
                const auto visual = std::find_if(
                    visuals_.begin(), visuals_.end(), [this](const auto& item) {
                        return item->id == selection_.focus.paragraph_id;
                    });
                if (visual != visuals_.end()) {
                    for (const auto& line : (*visual)->lines) {
                        const int lineStart = line.line.textStart();
                        const int lineEnd =
                            lineStart + line.line.textLength();
                        const int anchor = (*visual)->layoutOffsetForCore(
                            selection_.anchor.utf16_offset);
                        const int focus = (*visual)->layoutOffsetForCore(
                            selection_.focus.utf16_offset);
                        if (anchor < lineStart || anchor > lineEnd ||
                            focus < lineStart || focus > lineEnd) {
                            continue;
                        }
                        const qreal anchorX = line.line.cursorToX(anchor);
                        const qreal focusX = line.line.cursorToX(focus);
                        if (!qFuzzyCompare(anchorX + 1.0, focusX + 1.0)) {
                            const bool anchorIsTarget = forward
                                ? anchorX > focusX
                                : anchorX < focusX;
                            target = anchorIsTarget
                                ? selection_.anchor
                                : selection_.focus;
                        }
                        break;
                    }
                }
            }
            setCursor(target, false);
        }
        return;
    }
    auto position = selection_.focus;
    const auto index = snap.document.paragraphIndex(position.paragraph_id);
    if (!index) return;
    const auto& paragraph = snap.document.paragraphs()[*index];
    const auto text = fromUtf16(paragraph.text());
    const auto marker = plainTextListMarker(text);
    const std::size_t contentStart = marker
        ? static_cast<std::size_t>(marker->prefixLength)
        : 0;

    const bool baseRightToLeft = text.isRightToLeft();
    if (!byWord && !text.isEmpty()) {
        ensureLayout();
        const auto visual = std::find_if(
            visuals_.begin(), visuals_.end(), [&position](const auto& item) {
                return item->id == position.paragraph_id;
            });
        if (visual != visuals_.end() && (*visual)->layout) {
            const int oldLayoutOffset = (*visual)->layoutOffsetForCore(
                position.utf16_offset);
            int adjacentLayoutOffset = oldLayoutOffset;
            std::size_t adjacentCoreOffset = position.utf16_offset;
            do {
                const int next = forward
                    ? (*visual)->layout->rightCursorPosition(
                          adjacentLayoutOffset)
                    : (*visual)->layout->leftCursorPosition(
                          adjacentLayoutOffset);
                if (next == adjacentLayoutOffset) break;
                adjacentLayoutOffset = next;
                adjacentCoreOffset = (*visual)->coreOffsetForLayout(
                    adjacentLayoutOffset);
            } while (adjacentCoreOffset == position.utf16_offset);
            if (adjacentCoreOffset >= contentStart &&
                adjacentCoreOffset <= static_cast<std::size_t>(text.size()) &&
                adjacentCoreOffset != position.utf16_offset) {
                position.utf16_offset = adjacentCoreOffset;
                setCursor(position, extend);
                return;
            }
        }
    }

    // Word navigation follows the physical arrow direction as well.  In a
    // right-to-left paragraph, Ctrl+Left advances through logical text while
    // Ctrl+Right retreats.  Character navigation reaches this path only at a
    // visual edge of the QTextLayout.
    const bool logicalForward = baseRightToLeft ? !forward : forward;
    if (byWord && logicalForward &&
        position.utf16_offset < paragraph.text().size()) {
        position.utf16_offset = static_cast<std::size_t>(
            nextWordStart(text,
                          static_cast<qsizetype>(position.utf16_offset)));
    } else if (byWord && !logicalForward &&
               position.utf16_offset > contentStart) {
        position.utf16_offset = std::max(
            contentStart,
            static_cast<std::size_t>(previousWordStart(
                text, static_cast<qsizetype>(position.utf16_offset))));
    } else if (logicalForward &&
               *index + 1 < snap.document.paragraphs().size()) {
        const auto& next = snap.document.paragraphs()[*index + 1];
        const auto nextMarker = plainTextListMarker(fromUtf16(next.text()));
        position = {
            next.id(), nextMarker
                ? static_cast<std::size_t>(nextMarker->prefixLength)
                : 0};
    } else if (!logicalForward && *index > 0) {
        const auto& previous = snap.document.paragraphs()[*index - 1];
        position = {previous.id(), previous.text().size()};
    } else {
        position.utf16_offset = logicalForward
            ? paragraph.text().size()
            : contentStart;
    }
    setCursor(position, extend);
}

void DocumentCanvas::moveVertical(bool down, bool extend) {
    endTypingGroup();
    ensureLayout();
    std::vector<std::pair<const VisualLine*, const ParagraphVisual*>> lines;
    for (const auto& paragraph : visuals_) {
        for (const auto& line : paragraph->lines) lines.push_back({&line, paragraph.get()});
    }
    std::optional<std::size_t> fallback;
    std::optional<std::size_t> source;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto* line = lines[i].first;
        const auto* paragraph = lines[i].second;
        const int start = line->line.textStart();
        const int end = start + line->line.textLength();
        const int layoutCaret = paragraph->layoutOffsetForCore(
            selection_.focus.utf16_offset);
        if (paragraph->id != selection_.focus.paragraph_id ||
            layoutCaret < start || layoutCaret > end) continue;
        if (!fallback) {
            fallback = i;
        }
        if (lineAffinity_ && lineAffinity_->paragraphId == paragraph->id &&
            lineAffinity_->textStart == start) {
            source = i;
            break;
        }
    }
    if (!source) {
        source = fallback;
    }
    if (!source) return;

    const int target = down ? static_cast<int>(*source) + 1
                            : static_cast<int>(*source) - 1;
    if (target < 0 || target >= static_cast<int>(lines.size())) return;
    const auto* sourceLine = lines[*source].first;
    const auto* sourceParagraph = lines[*source].second;
    const qreal x = preferredVerticalX_.value_or(sourceLine->line.cursorToX(
        sourceParagraph->layoutOffsetForCore(
            selection_.focus.utf16_offset)));
    const auto targetIndex = static_cast<std::size_t>(target);
    const auto* targetLine = lines[targetIndex].first;
    const auto* targetParagraph = lines[targetIndex].second;
    const int offset = targetLine->line.xToCursor(
        x, QTextLine::CursorBetweenCharacters);
    lineAffinity_ = LineAffinity{targetParagraph->id,
                                 targetLine->line.textStart()};
    preferredVerticalX_ = x;
    setCursor(
        {targetParagraph->id,
         targetParagraph->coreOffsetForLayout(offset)},
        extend, true);
}

void DocumentCanvas::keyPressEvent(QKeyEvent* event) {
    if (imageResizeDrag_ && event->key() == Qt::Key_Escape) {
        imageResizeDrag_.reset();
        viewport()->setCursor(Qt::ArrowCursor);
        viewport()->update();
        event->accept();
        return;
    }
    if (previewId_) {
        const bool unmodifiedText =
            !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                    Qt::MetaModifier));
        const bool wouldMutate =
            event->matches(QKeySequence::Cut) ||
            event->matches(QKeySequence::Paste) ||
            isPasteTextOnlyShortcut(*event) ||
            event->key() == Qt::Key_Backspace ||
            event->key() == Qt::Key_Delete ||
            event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Tab ||
            event->key() == Qt::Key_Backtab ||
            (containsPrintableKeyText(event->text()) &&
             (unmodifiedText || isAltGrTextInput(*event)));
        if (wouldMutate) {
            static_cast<void>(rejectLiveEditDuringPreview());
        }
        event->accept();
        return;
    }
    // Keep the canonical editor shortcuts functional even on platforms where
    // a QWidgetWithChildren QAction shortcut is not dispatched through an
    // item-view viewport (notably some Linux/Wayland and headless backends).
    // If Qt dispatches the QAction first, this key press never reaches here;
    // otherwise the canvas performs the same command exactly once.
    if (event->matches(QKeySequence::Undo)) { undo(); return; }
    if (event->matches(QKeySequence::Redo)) { redo(); return; }
    if (isPasteTextOnlyShortcut(*event)) { pasteTextOnly(); return; }
    // Character-format shortcuts are valid in body text, an active table
    // cell, and an explicit cell range. Route them before the table-specific
    // key handling so table editing never swallows the standard shortcuts.
    if (event->matches(QKeySequence::Bold)) { toggleBold(); return; }
    if (event->matches(QKeySequence::Italic)) { toggleItalic(); return; }
    if (event->matches(QKeySequence::Underline)) { toggleUnderline(); return; }
    if (selectedExcalidrawScene() &&
        (event->key() == Qt::Key_Return ||
         event->key() == Qt::Key_Enter)) {
        emit editExcalidrawFigureRequested();
        return;
    }
    if (selectedInlineImageId() && event->key() == Qt::Key_F2) {
        if (event->modifiers() & Qt::ShiftModifier) {
            showSelectedImageAltTextDialog();
        } else if (selectedExcalidrawScene()) {
            emit editExcalidrawFigureRequested();
        } else {
            showSelectedImageSizeDialog();
        }
        return;
    }
    const auto tableMoveShortcut =
        (event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier |
                               Qt::ControlModifier | Qt::MetaModifier)) ==
        (Qt::ShiftModifier | Qt::AltModifier);
    if (selectedTable_ && tableMoveShortcut &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        static_cast<void>(moveSelectedTable(event->key() == Qt::Key_Down));
        return;
    }
    if (selectedTable_ && !tableCursor_) {
        if (event->matches(QKeySequence::Copy)) { copy(); return; }
        if (event->matches(QKeySequence::Cut)) { cut(); return; }
        if (event->matches(QKeySequence::Paste)) { paste(); return; }
        if (event->matches(QKeySequence::SelectAll)) {
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace ||
            event->key() == Qt::Key_Delete) {
            if (tableCellSelection_) {
                static_cast<void>(clearSelectedTableCells());
            } else {
                static_cast<void>(deleteSelectedTable());
            }
            return;
        }
        if (event->key() == Qt::Key_Return ||
            event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Tab ||
            event->key() == Qt::Key_Backtab ||
            event->key() == Qt::Key_F2) {
            const std::size_t row = tableCellSelection_
                ? std::min(tableCellSelection_->anchorRow,
                           tableCellSelection_->focusRow)
                : 0;
            const std::size_t column = tableCellSelection_
                ? std::min(tableCellSelection_->anchorColumn,
                           tableCellSelection_->focusColumn)
                : 0;
            static_cast<void>(activateTableCell(
                *selectedTable_, row, column, 0));
            return;
        }
        // A whole-table selection owns keyboard input. Never let an
        // unhandled keystroke fall through to the stale body-text caret.
        event->accept();
        return;
    }
    if (tableCursor_) {
        const auto cursor = *tableCursor_;
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(cursor.tableId);
        const auto* cell = table ? table->cell(cursor.row, cursor.column) : nullptr;
        if (!cell) {
            tableCursor_.reset();
            tableSelectionAnchor_.reset();
            tableCellSelection_.reset();
            tableMouseSelectionAnchor_.reset();
            selectedTable_.reset();
            event->accept();
            return;
        }
        const QString cellText = fromUtf16(cell->text);
        const qsizetype caret = static_cast<qsizetype>(std::min(
            cursor.utf16Offset, cell->text.size()));
        const qsizetype anchor = static_cast<qsizetype>(std::min(
            tableSelectionAnchor_.value_or(cursor.utf16Offset),
            cell->text.size()));
        const qsizetype selectionStart = std::min(anchor, caret);
        const qsizetype selectionEnd = std::max(anchor, caret);
        const bool hasCellSelection = selectionStart != selectionEnd;
        ensureLayout();
        const TableCellVisual* activeCellVisual = nullptr;
        for (const auto& tableVisual : tableVisuals_) {
            if (tableVisual->id != cursor.tableId) continue;
            const auto found = std::find_if(
                tableVisual->cells.begin(), tableVisual->cells.end(),
                [&cursor](const TableCellVisual& visual) {
                    return visual.row == cursor.row &&
                           visual.column == cursor.column;
                });
            if (found != tableVisual->cells.end()) {
                activeCellVisual = &*found;
            }
            break;
        }
        const auto collapsedCellOffset =
            [activeCellVisual, anchor, caret, selectionStart, selectionEnd](
                bool right) {
                const qsizetype fallback = right ? selectionEnd : selectionStart;
                if (!activeCellVisual) return fallback;
                for (const auto& line : activeCellVisual->lines) {
                    const int lineStart = line.textStart();
                    const int lineEnd = lineStart + line.textLength();
                    if (anchor < lineStart || anchor > lineEnd ||
                        caret < lineStart || caret > lineEnd) {
                        continue;
                    }
                    const qreal anchorX = line.cursorToX(
                        static_cast<int>(anchor));
                    const qreal caretX = line.cursorToX(
                        static_cast<int>(caret));
                    if (qFuzzyCompare(anchorX + 1.0, caretX + 1.0)) {
                        return fallback;
                    }
                    return right
                        ? (anchorX > caretX ? anchor : caret)
                        : (anchorX < caretX ? anchor : caret);
                }
                return fallback;
            };
        const auto adjacentCellOffset =
            [activeCellVisual, caret](bool right) {
                if (!activeCellVisual || !activeCellVisual->layout) {
                    return caret;
                }
                const int adjacent = right
                    ? activeCellVisual->layout->rightCursorPosition(
                          static_cast<int>(caret))
                    : activeCellVisual->layout->leftCursorPosition(
                          static_cast<int>(caret));
                return adjacent < 0
                    ? caret
                    : static_cast<qsizetype>(adjacent);
            };
        const auto moveCellCaret =
            [this, cursor](std::size_t offset, bool extend) {
                if (!extend) {
                    return activateTableCell(
                        cursor.tableId, cursor.row, cursor.column, offset);
                }
                tableCursor_ = TableCursor{
                    cursor.tableId, cursor.row, cursor.column, offset};
                if (!tableSelectionAnchor_) {
                    tableSelectionAnchor_ = cursor.utf16Offset;
                }
                tableCellSelection_.reset();
                selectedTable_ = cursor.tableId;
                typingFormat_ = selectedCharacterFormat();
                typingOverrideMask_ = {};
                commitPendingSpellingWordIfCaretLeft();
                emit selectionChanged();
                emitCursorFormat();
                updateStatus();
                revealCursor();
                viewport()->update();
                updateMicroFocus();
                return true;
            };
        const auto replaceCellRange =
            [this, cursor, &cellText](qsizetype start, qsizetype end,
                                     const QString& replacement,
                                     bool coalesceTyping) {
                start = std::clamp<qsizetype>(start, 0, cellText.size());
                end = std::clamp<qsizetype>(end, start, cellText.size());
                QString updated = cellText;
                updated.replace(start, end - start, replacement);
                const TableCursor next{
                    cursor.tableId, cursor.row, cursor.column,
                    static_cast<std::size_t>(start + replacement.size())};
                return apply({core::SetTableCellText{
                                  cursor.tableId, cursor.row, cursor.column,
                                  toUtf16(updated), typingFormat_}},
                             std::nullopt, std::nullopt, coalesceTyping,
                             std::nullopt, false, true, next,
                             cursor.tableId);
            };

        if (event->matches(QKeySequence::Copy)) { copy(); return; }
        if (event->matches(QKeySequence::Cut)) { cut(); return; }
        if (event->matches(QKeySequence::Paste)) { paste(); return; }
        if (event->matches(QKeySequence::SelectAll)) { selectAll(); return; }
        switch (event->key()) {
            case Qt::Key_Escape:
                static_cast<void>(selectTable(cursor.tableId));
                return;
            case Qt::Key_Backspace: {
                if (hasCellSelection) {
                    static_cast<void>(replaceCellRange(
                        selectionStart, selectionEnd, {}, true));
                    refreshPendingSpellingWordAfterEdit();
                    return;
                }
                if (caret == 0) return;
                qsizetype start = 0;
                if (event->modifiers() & Qt::ControlModifier) {
                    start = previousWordStart(cellText, caret);
                } else {
                    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,
                                               cellText);
                    finder.setPosition(caret);
                    start = finder.toPreviousBoundary();
                    if (start < 0) start = caret - 1;
                }
                static_cast<void>(replaceCellRange(start, caret, {}, true));
                refreshPendingSpellingWordAfterEdit();
                return;
            }
            case Qt::Key_Delete: {
                if (hasCellSelection) {
                    static_cast<void>(replaceCellRange(
                        selectionStart, selectionEnd, {}, true));
                    refreshPendingSpellingWordAfterEdit();
                    return;
                }
                if (caret >= cellText.size()) return;
                qsizetype end = cellText.size();
                if (event->modifiers() & Qt::ControlModifier) {
                    end = nextWordStart(cellText, caret);
                } else {
                    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,
                                               cellText);
                    finder.setPosition(caret);
                    end = finder.toNextBoundary();
                    if (end < 0) end = caret + 1;
                }
                static_cast<void>(replaceCellRange(caret, end, {}, true));
                refreshPendingSpellingWordAfterEdit();
                return;
            }
            case Qt::Key_Left: {
                const bool extend =
                    event->modifiers() & Qt::ShiftModifier;
                qsizetype next = hasCellSelection && !extend
                    ? collapsedCellOffset(false)
                    : adjacentCellOffset(false);
                if (!hasCellSelection &&
                    event->modifiers() & Qt::ControlModifier) {
                    next = cellText.isRightToLeft()
                        ? nextWordStart(cellText, caret)
                        : previousWordStart(cellText, caret);
                }
                static_cast<void>(moveCellCaret(
                    static_cast<std::size_t>(std::clamp<qsizetype>(
                        next, 0, cellText.size())),
                    extend));
                return;
            }
            case Qt::Key_Right: {
                const bool extend =
                    event->modifiers() & Qt::ShiftModifier;
                qsizetype next = hasCellSelection && !extend
                    ? collapsedCellOffset(true)
                    : adjacentCellOffset(true);
                if (!hasCellSelection &&
                    event->modifiers() & Qt::ControlModifier) {
                    next = cellText.isRightToLeft()
                        ? previousWordStart(cellText, caret)
                        : nextWordStart(cellText, caret);
                }
                static_cast<void>(moveCellCaret(
                    static_cast<std::size_t>(std::clamp<qsizetype>(
                        next, 0, cellText.size())),
                    extend));
                return;
            }
            case Qt::Key_Up:
            case Qt::Key_Down: {
                const bool down = event->key() == Qt::Key_Down;
                const std::size_t row = down
                    ? std::min(cursor.row + 1, table->rowCount() - 1)
                    : (cursor.row == 0 ? 0 : cursor.row - 1);
                const auto* target = table->cell(row, cursor.column);
                static_cast<void>(activateTableCell(
                    cursor.tableId, row, cursor.column,
                    std::min(cursor.utf16Offset,
                             target ? target->text.size() : std::size_t{0})));
                return;
            }
            case Qt::Key_Home:
                static_cast<void>(moveCellCaret(
                    0, event->modifiers() & Qt::ShiftModifier));
                return;
            case Qt::Key_End:
                static_cast<void>(moveCellCaret(
                    cell->text.size(),
                    event->modifiers() & Qt::ShiftModifier));
                return;
            case Qt::Key_Backtab:
                static_cast<void>(moveActiveTableCell(false));
                return;
            case Qt::Key_Tab:
                static_cast<void>(moveActiveTableCell(
                    !(event->modifiers() & Qt::ShiftModifier)));
                return;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                static_cast<void>(replaceCellRange(
                    selectionStart, selectionEnd,
                    QString(QChar::LineSeparator), false));
                clearPendingSpellingWord();
                viewport()->update();
                return;
            default: break;
        }
        const bool unmodifiedText =
            !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                    Qt::MetaModifier));
        if (containsPrintableKeyText(event->text()) &&
            (unmodifiedText || isAltGrTextInput(*event))) {
            if (replaceCellRange(
                    selectionStart, selectionEnd, event->text(), true)) {
                setPendingSpellingWordFromTypedText(event->text());
            }
            return;
        }
        event->accept();
        return;
    }
    const bool extend = event->modifiers() & Qt::ShiftModifier;
    const bool byWord = event->modifiers() & Qt::ControlModifier;
    if (event->matches(QKeySequence::Bold)) { toggleBold(); return; }
    if (event->matches(QKeySequence::Italic)) { toggleItalic(); return; }
    if (event->matches(QKeySequence::Underline)) { toggleUnderline(); return; }
    if (event->matches(QKeySequence::Copy)) { copy(); return; }
    if (event->matches(QKeySequence::Cut)) { cut(); return; }
    if (event->matches(QKeySequence::Paste)) { paste(); return; }
    if (event->matches(QKeySequence::SelectAll)) { selectAll(); return; }
    switch (event->key()) {
        case Qt::Key_Backspace:
            if (!(event->modifiers() & (Qt::AltModifier |
                                        Qt::MetaModifier)) &&
                handlePlainTextListBackspace()) {
                refreshPendingSpellingWordAfterEdit();
                return;
            }
            deleteBackward(byWord);
            refreshPendingSpellingWordAfterEdit();
            return;
        case Qt::Key_Delete:
            deleteForward(byWord);
            refreshPendingSpellingWordAfterEdit();
            return;
        case Qt::Key_Left: moveHorizontal(false, extend, byWord); return;
        case Qt::Key_Right: moveHorizontal(true, extend, byWord); return;
        case Qt::Key_Up: moveVertical(false, extend); return;
        case Qt::Key_Down: moveVertical(true, extend); return;
        case Qt::Key_Home: {
            auto position = selection_.focus;
            if (byWord) {
                const auto snap = session_->snapshot();
                const auto& first = snap.document.paragraphs().front();
                const auto marker = plainTextListMarker(fromUtf16(first.text()));
                position = {
                    first.id(), marker
                        ? static_cast<std::size_t>(marker->prefixLength)
                        : 0};
            } else {
                const auto marker = plainTextListMarker(
                    paragraphText(position.paragraph_id));
                position.utf16_offset = marker
                    ? static_cast<std::size_t>(marker->prefixLength)
                    : 0;
            }
            setCursor(position, extend);
            return;
        }
        case Qt::Key_End: {
            auto position = selection_.focus;
            if (byWord) {
                const auto snap = session_->snapshot();
                const auto& last = snap.document.paragraphs().back();
                position = {last.id(), last.text().size()};
            } else {
                position.utf16_offset =
                    static_cast<std::size_t>(paragraphText(position.paragraph_id).size());
            }
            setCursor(position, extend);
            return;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            const auto modifiers = event->modifiers() &
                (Qt::ShiftModifier | Qt::ControlModifier |
                 Qt::AltModifier | Qt::MetaModifier);
            if (modifiers == Qt::ShiftModifier) {
                replaceSelection(QString(QChar::LineSeparator));
                clearPendingSpellingWord();
                viewport()->update();
                return;
            }
            if (modifiers == Qt::ControlModifier) {
                clearPendingSpellingWord();
                insertPageBreak();
                return;
            }
            if (!(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                        Qt::AltModifier | Qt::MetaModifier)) &&
                continuePlainTextList()) {
                clearPendingSpellingWord();
                viewport()->update();
                return;
            }
            insertParagraphBreak();
            clearPendingSpellingWord();
            viewport()->update();
            return;
        }
        case Qt::Key_Backtab:
            if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                        Qt::MetaModifier)) &&
                changeListLevel(true)) {
                clearPendingSpellingWord();
                viewport()->update();
                return;
            }
            break;
        case Qt::Key_Tab:
            if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                        Qt::MetaModifier)) &&
                changeListLevel(extend)) {
                clearPendingSpellingWord();
                viewport()->update();
                return;
            }
            replaceSelection(QStringLiteral("\t"));
            clearPendingSpellingWord();
            viewport()->update();
            return;
        default: break;
    }
    const bool unmodifiedText =
        !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                Qt::MetaModifier));
    if (containsPrintableKeyText(event->text()) &&
        (unmodifiedText || isAltGrTextInput(*event))) {
        replaceSelection(event->text(), true);
        setPendingSpellingWordFromTypedText(event->text());
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void DocumentCanvas::inputMethodEvent(QInputMethodEvent* event) {
    if (previewId_) {
        if (!event->commitString().isEmpty()) {
            static_cast<void>(rejectLiveEditDuringPreview());
        }
        event->accept();
        return;
    }
    if (!event->commitString().isEmpty()) {
        if (tableCursor_) {
            if (replaceTableCellText(event->commitString(), true)) {
                setPendingSpellingWordFromTypedText(event->commitString());
            }
        } else if (selectedTable_) {
            emit operationFailed(tr(
                "Press Enter or F2 to edit the selected table."));
        } else {
            replaceSelection(event->commitString(), true);
            setPendingSpellingWordFromTypedText(event->commitString());
        }
    }
    event->accept();
}

QVariant DocumentCanvas::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (tableCursor_) {
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column) : nullptr;
        const QString surrounding = cell ? fromUtf16(cell->text) : QString();
        switch (query) {
            case Qt::ImCursorRectangle: return caretRectInContent().toRect();
            case Qt::ImCursorPosition:
                return static_cast<int>(tableCursor_->utf16Offset);
            case Qt::ImAnchorPosition:
                return static_cast<int>(tableSelectionAnchor_.value_or(
                    tableCursor_->utf16Offset));
            case Qt::ImSurroundingText: return surrounding;
            case Qt::ImCurrentSelection: return selectedText();
            default: break;
        }
    }
    switch (query) {
        case Qt::ImCursorRectangle: return caretRectInContent().toRect();
        case Qt::ImCursorPosition: return static_cast<int>(selection_.focus.utf16_offset);
        case Qt::ImAnchorPosition: return static_cast<int>(selection_.anchor.utf16_offset);
        case Qt::ImSurroundingText: return paragraphText(selection_.focus.paragraph_id);
        case Qt::ImCurrentSelection: return selectedText();
        default: return QAbstractScrollArea::inputMethodQuery(query);
    }
}

bool DocumentCanvas::focusNextPrevChild(bool) {
    // QWidget normally consumes Tab and Backtab for focus traversal before
    // keyPressEvent() can apply editor semantics. Keep both keys in the
    // editing surface so body tabs and list indentation remain available.
    return false;
}

DocumentCanvas::Hit DocumentCanvas::hitTest(const QPoint& viewportPoint) const {
    ensureLayout();
    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    const double pagePixelWidth = pageWidthPoints_ * scale;
    const double documentWidth = std::max(pagePixelWidth + 2 * kCanvasPaddingPixels,
                                          static_cast<double>(viewport()->width()));
    const double left = (documentWidth - pagePixelWidth) / 2.0 - horizontalScrollBar()->value();
    const double contentY = viewportPoint.y() + verticalScrollBar()->value() - kCanvasPaddingPixels;
    const int page = static_cast<int>(std::floor(contentY /
        (pageHeightPoints_ * scale + kPageGapPixels)));
    if (page < 0 || page >= pageCount_) return {};
    const double pageTop = kCanvasPaddingPixels + page * (pageHeightPoints_ * scale + kPageGapPixels) -
                           verticalScrollBar()->value();
    const QPointF point((viewportPoint.x() - left) / scale,
                        (viewportPoint.y() - pageTop) / scale);
    if (const auto selectedImage = selectedInlineImageId()) {
        constexpr double kHandleHitPixels = 8.0;
        const double radius = kHandleHitPixels / std::max(0.01, scale);
        for (const auto& paragraph : visuals_) {
            const auto found = std::find_if(
                paragraph->images.begin(), paragraph->images.end(),
                [selectedImage, page](const ParagraphImageVisual& image) {
                    return image.id == *selectedImage &&
                           image.pageIndex == page;
                });
            if (found == paragraph->images.end()) continue;
            const QRectF rect = imageResizeDrag_ &&
                    imageResizeDrag_->imageId == found->id &&
                    imageResizeDrag_->pageIndex == page
                ? imageResizeDrag_->previewRect
                : found->rect;
            const std::array<std::pair<ImageResizeHandle, QPointF>, 3>
                handles{{
                    // Prefer the corner when handles overlap on a very small
                    // picture; it changes both dimensions and matches the
                    // visible bottom-right affordance.
                    {ImageResizeHandle::bottom_right, rect.bottomRight()},
                    {ImageResizeHandle::right, QPointF(rect.right(),
                                                       rect.center().y())},
                    {ImageResizeHandle::bottom, QPointF(rect.center().x(),
                                                        rect.bottom())},
                }};
            for (const auto& [handle, center] : handles) {
                if (!QRectF(center.x() - radius, center.y() - radius,
                            radius * 2.0, radius * 2.0)
                         .contains(point)) {
                    continue;
                }
                Hit hit;
                hit.image = found->id;
                hit.position = {paragraph->id, found->coreUtf16Offset};
                hit.imageResizeHandle = handle;
                hit.pagePoint = point;
                hit.pageIndex = page;
                return hit;
            }
            break;
        }
    }
    for (const auto& tableVisual : tableVisuals_) {
        if (tableVisual->handlePageIndex == page &&
            tableVisual->handleRect.adjusted(-2.0, -2.0, 2.0, 2.0)
                .contains(point)) {
            Hit hit;
            hit.tableHandle = tableVisual->id;
            return hit;
        }
        for (const auto& cell : tableVisual->cells) {
            if (cell.pageIndex != page || !cell.rect.contains(point)) continue;
            std::size_t offset = 0;
            const QTextLine* nearestLine = nullptr;
            double nearestDistance = std::numeric_limits<double>::max();
            for (const auto& line : cell.lines) {
                const double distance = std::abs(
                    point.y() - (line.y() + line.height() / 2.0));
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestLine = &line;
                }
                if (point.y() >= line.y() &&
                    point.y() <= line.y() + line.height()) {
                    nearestLine = &line;
                    break;
                }
            }
            if (nearestLine) {
                const int candidate = nearestLine->xToCursor(
                    point.x(), QTextLine::CursorBetweenCharacters);
                offset = static_cast<std::size_t>(std::clamp(
                    candidate, 0, static_cast<int>(cell.text.size())));
            }
            Hit hit;
            hit.tableCursor = TableCursor{tableVisual->id, cell.row,
                                          cell.column, offset};
            if (nearestLine) {
                hit.lineStart = nearestLine->textStart();
                hit.lineEnd = nearestLine->textStart() +
                              nearestLine->textLength();
            }
            return hit;
        }
    }
    // Pictures are painted in document order, so the last intersecting
    // picture is visually on top and must receive the click first.
    for (auto paragraph = visuals_.rbegin(); paragraph != visuals_.rend();
         ++paragraph) {
        for (auto image = (*paragraph)->images.rbegin();
             image != (*paragraph)->images.rend(); ++image) {
            if (image->pageIndex == page &&
                image->rect.adjusted(-2.0, -2.0, 2.0, 2.0)
                    .contains(point)) {
                Hit hit;
                hit.image = image->id;
                hit.position = {(*paragraph)->id, image->coreUtf16Offset};
                hit.pagePoint = point;
                hit.pageIndex = page;
                return hit;
            }
        }
    }
    const VisualLine* nearest = nullptr;
    const ParagraphVisual* owner = nullptr;
    double best = std::numeric_limits<double>::max();
    for (const auto& paragraph : visuals_) {
        for (const auto& line : paragraph->lines) {
            if (line.pageIndex != page) continue;
            const double distance = std::abs(point.y() - (line.line.y() + line.line.height() / 2.0));
            if (distance < best) { best = distance; nearest = &line; owner = paragraph.get(); }
            if (point.y() >= line.line.y() && point.y() <= line.line.y() + line.line.height()) {
                nearest = &line; owner = paragraph.get(); best = -1; break;
            }
        }
        if (best < 0) break;
    }
    if (!nearest || !owner) return {};
    const int offset = nearest->line.xToCursor(
        point.x(), QTextLine::CursorBetweenCharacters);
    Hit hit;
    hit.position = {owner->id, owner->coreOffsetForLayout(offset)};
    hit.valid = true;
    hit.lineStart = static_cast<int>(owner->coreOffsetForLayout(
        nearest->line.textStart()));
    hit.lineEnd = static_cast<int>(owner->coreOffsetForLayout(
        nearest->line.textStart() + nearest->line.textLength()));
    hit.layoutLineStart = nearest->line.textStart();
    return hit;
}

std::optional<DocumentCanvas::StoryRegionHit> DocumentCanvas::storyRegionAt(
    const QPoint& viewportPoint) const {
    ensureLayout();
    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    const double pagePixelWidth = pageWidthPoints_ * scale;
    const double documentWidth = std::max(
        pagePixelWidth + 2 * kCanvasPaddingPixels,
        static_cast<double>(viewport()->width()));
    const double left = (documentWidth - pagePixelWidth) / 2.0 -
                        horizontalScrollBar()->value();
    const double contentY = viewportPoint.y() + verticalScrollBar()->value() -
                            kCanvasPaddingPixels;
    const double pageStride = pageHeightPoints_ * scale + kPageGapPixels;
    const int page = static_cast<int>(std::floor(contentY / pageStride));
    if (page < 0 || page >= pageCount_) return std::nullopt;
    const double pageTop = kCanvasPaddingPixels + page * pageStride -
                           verticalScrollBar()->value();
    const QPointF point((viewportPoint.x() - left) / scale,
                        (viewportPoint.y() - pageTop) / scale);
    const double storyWidth = pageWidthPoints_ - marginLeftPoints_ -
                              marginRightPoints_;
    if (storyWidth <= 0.0 || point.x() < marginLeftPoints_ ||
        point.x() > marginLeftPoints_ + storyWidth) {
        return std::nullopt;
    }
    const bool inHeader = point.y() >= 8.0 &&
        point.y() <= std::max(32.0, marginTopPoints_ - 3.0);
    const bool inFooter = point.y() >= std::min(
        pageHeightPoints_ - 32.0,
        pageHeightPoints_ - marginBottomPoints_ + 3.0) &&
        point.y() <= pageHeightPoints_ - 8.0;
    if (!inHeader && !inFooter) return std::nullopt;
    const double relativeX = point.x() - marginLeftPoints_;
    const int section = std::clamp(
        static_cast<int>(relativeX * 3.0 / storyWidth), 0, 2);
    return StoryRegionHit{inFooter, section, page};
}

void DocumentCanvas::mousePressEvent(QMouseEvent* event) {
    if (previewId_) {
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (const auto story = storyRegionAt(event->position().toPoint())) {
            beginHeaderFooterEditing(story->footer, story->section,
                                     story->pageIndex);
            selecting_ = false;
            event->accept();
            return;
        }
        if (headerFooterEditing_) endHeaderFooterEditing();
        setFocus();
        const auto hit = hitTest(event->position().toPoint());
        const bool isTripleClick =
            tripleClickArmed_ && multiClickTimer_.isValid() &&
            multiClickTimer_.elapsed() <= QApplication::doubleClickInterval() &&
            (event->position().toPoint() - lastDoubleClickPosition_)
                    .manhattanLength() <= QApplication::startDragDistance();
        tripleClickArmed_ = false;
        if (hit.tableHandle) {
            static_cast<void>(selectTable(*hit.tableHandle));
            draggingTable_ = true;
            tableDropTargetValid_ = false;
            tableDropBefore_.reset();
            selecting_ = false;
            viewport()->setCursor(Qt::SizeAllCursor);
            event->accept();
            return;
        }
        if (hit.image && hit.imageResizeHandle) {
            static_cast<void>(selectInlineImage(*hit.image));
            ensureLayout();
            for (const auto& paragraph : visuals_) {
                const auto found = std::find_if(
                    paragraph->images.begin(), paragraph->images.end(),
                    [&hit](const ParagraphImageVisual& image) {
                        return image.id == *hit.image;
                    });
                if (found == paragraph->images.end()) continue;
                const auto selected = selectedInlineImage();
                if (!selected) break;
                imageResizeDrag_ = ImageResizeDrag{
                    found->id, *hit.imageResizeHandle, hit.pagePoint,
                    found->rect, found->rect, found->pageIndex,
                    static_cast<double>(selected->second.width_emu) /
                        kEmuPerPoint,
                    static_cast<double>(selected->second.height_emu) /
                        kEmuPerPoint};
                break;
            }
            selecting_ = false;
            event->accept();
            return;
        }
        if (hit.image) {
            static_cast<void>(selectInlineImage(*hit.image));
            selecting_ = false;
            viewport()->setCursor(Qt::ArrowCursor);
            event->accept();
            return;
        }
        if (hit.tableCursor) {
            if (event->modifiers() & Qt::ShiftModifier) {
                std::optional<std::pair<std::size_t, std::size_t>> anchor;
                if (tableCellSelection_ &&
                    tableCellSelection_->tableId ==
                        hit.tableCursor->tableId) {
                    anchor = std::pair{
                        tableCellSelection_->anchorRow,
                        tableCellSelection_->anchorColumn};
                } else if (tableCursor_ &&
                           tableCursor_->tableId ==
                               hit.tableCursor->tableId) {
                    anchor = std::pair{tableCursor_->row,
                                       tableCursor_->column};
                }
                if (anchor && selectTableCells(
                                  hit.tableCursor->tableId,
                                  anchor->first, anchor->second,
                                  hit.tableCursor->row,
                                  hit.tableCursor->column)) {
                    selecting_ = false;
                    event->accept();
                    return;
                }
            }
            auto targetOffset = hit.tableCursor->utf16Offset;
            std::optional<std::size_t> lineAnchor;
            if (isTripleClick) {
                const auto snap = session_->snapshot();
                const auto* table = snap.document.findTable(
                    hit.tableCursor->tableId);
                const auto* cell = table
                    ? table->cell(hit.tableCursor->row,
                                  hit.tableCursor->column)
                    : nullptr;
                if (cell) {
                    auto lineStart = static_cast<std::size_t>(
                        std::max(0, hit.lineStart));
                    auto lineEnd = static_cast<std::size_t>(
                        std::max(hit.lineStart, hit.lineEnd));
                    lineStart = std::min(lineStart, cell->text.size());
                    lineEnd = std::min(lineEnd, cell->text.size());
                    while (lineEnd > lineStart &&
                           cell->text[lineEnd - 1] == u'\u2028') {
                        --lineEnd;
                    }
                    lineAnchor = lineStart;
                    targetOffset = lineEnd;
                }
            }
            static_cast<void>(activateTableCell(
                hit.tableCursor->tableId, hit.tableCursor->row,
                hit.tableCursor->column, targetOffset));
            if (lineAnchor) {
                tableSelectionAnchor_ = *lineAnchor;
                typingFormat_ = selectedCharacterFormat();
                typingOverrideMask_ = {};
                emit selectionChanged();
                emitCursorFormat();
                viewport()->update();
                updateMicroFocus();
            }
            tableMouseSelectionAnchor_ = *hit.tableCursor;
            selecting_ = true;
            event->accept();
            return;
        }
        if (isTripleClick && hit.valid) {
            const QString text = paragraphText(hit.position.paragraph_id);
            int lineEnd = std::min(hit.lineEnd, static_cast<int>(text.size()));
            while (lineEnd > hit.lineStart &&
                   text.at(lineEnd - 1) == QChar::LineSeparator) {
                --lineEnd;
            }
            selectRange(
                {{hit.position.paragraph_id,
                  static_cast<std::size_t>(std::max(0, hit.lineStart))},
                 {hit.position.paragraph_id,
                  static_cast<std::size_t>(std::max(hit.lineStart, lineEnd))}},
                {hit.position.paragraph_id, hit.layoutLineStart});
            selecting_ = false;
            event->accept();
            return;
        }
        if (hit.valid) {
            preferredVerticalX_.reset();
            lineAffinity_ = LineAffinity{hit.position.paragraph_id,
                                         hit.layoutLineStart};
            setCursor(hit.position, event->modifiers() & Qt::ShiftModifier,
                      true);
            selecting_ = true;
        }
    }
}

void DocumentCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if (previewId_) {
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    setFocus();
    const auto hit = hitTest(event->position().toPoint());
    lastDoubleClickPosition_ = event->position().toPoint();
    multiClickTimer_.start();
    tripleClickArmed_ = true;
    selecting_ = false;
    if (hit.image) {
        if (selectInlineImage(*hit.image)) {
            tripleClickArmed_ = false;
            if (selectedExcalidrawScene()) {
                emit editExcalidrawFigureRequested();
            } else {
                showSelectedImageSizeDialog();
            }
        }
    } else if (hit.tableCursor) {
        if (activateTableCell(
            hit.tableCursor->tableId, hit.tableCursor->row,
            hit.tableCursor->column, hit.tableCursor->utf16Offset)) {
            const auto snap = session_->snapshot();
            const auto* table = snap.document.findTable(
                hit.tableCursor->tableId);
            const auto* cell = table
                ? table->cell(hit.tableCursor->row,
                              hit.tableCursor->column)
                : nullptr;
            if (cell) {
                const QString text = fromUtf16(cell->text);
                qsizetype start = std::min<qsizetype>(
                    static_cast<qsizetype>(hit.tableCursor->utf16Offset),
                    text.size());
                qsizetype end = start;
                const auto isWordCharacter = [&text](qsizetype index) {
                    const auto character = text.at(index);
                    return character.isLetterOrNumber() ||
                           character == QLatin1Char('\'') ||
                           character == QChar(0x2019);
                };
                while (start > 0 && isWordCharacter(start - 1)) --start;
                while (end < text.size() && isWordCharacter(end)) ++end;
                if (start != end) {
                    tableSelectionAnchor_ = static_cast<std::size_t>(start);
                    tableCursor_->utf16Offset = static_cast<std::size_t>(end);
                    typingFormat_ = selectedCharacterFormat();
                    typingOverrideMask_ = {};
                    emit selectionChanged();
                    emitCursorFormat();
                    viewport()->update();
                    updateMicroFocus();
                }
            }
        }
    } else if (hit.valid) {
        core::Range wordRange;
        if (!wordAt(hit.position, &wordRange).isEmpty()) {
            selectRange(wordRange,
                        {hit.position.paragraph_id, hit.layoutLineStart});
        } else {
            lineAffinity_ = LineAffinity{hit.position.paragraph_id,
                                         hit.layoutLineStart};
            setCursor(hit.position, false, true);
        }
    }
    event->accept();
}

void DocumentCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (previewId_) {
        return;
    }
    if (imageResizeDrag_ && (event->buttons() & Qt::LeftButton)) {
        const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
        const double pagePixelWidth = pageWidthPoints_ * scale;
        const double documentWidth = std::max(
            pagePixelWidth + 2 * kCanvasPaddingPixels,
            static_cast<double>(viewport()->width()));
        const double left = (documentWidth - pagePixelWidth) / 2.0 -
                            horizontalScrollBar()->value();
        const double pageTop = kCanvasPaddingPixels +
            imageResizeDrag_->pageIndex *
                (pageHeightPoints_ * scale + kPageGapPixels) -
            verticalScrollBar()->value();
        const QPointF current(
            (event->position().x() - left) / scale,
            (event->position().y() - pageTop) / scale);
        const QPointF delta = current - imageResizeDrag_->startPoint;
        const QRectF original = imageResizeDrag_->originalRect;
        const auto handle = imageResizeDrag_->handle;
        const bool changesWidth = handle == ImageResizeHandle::right ||
                                  handle == ImageResizeHandle::bottom_right;
        const bool changesHeight = handle == ImageResizeHandle::bottom ||
                                   handle == ImageResizeHandle::bottom_right;
        constexpr double kMinimumPicturePoints = 1.0;
        constexpr double kMaximumPicturePoints = 20'000.0;
        double width = original.width();
        double height = original.height();
        if (changesWidth) width += delta.x();
        if (changesHeight) height += delta.y();
        width = std::clamp(width, kMinimumPicturePoints,
                           kMaximumPicturePoints);
        height = std::clamp(height, kMinimumPicturePoints,
                            kMaximumPicturePoints);

        const bool corner = handle == ImageResizeHandle::bottom_right;
        if (corner && !(event->modifiers() & Qt::ShiftModifier) &&
            original.width() > 0.0 && original.height() > 0.0) {
            const double aspect = original.width() / original.height();
            const double widthChange = std::abs(
                width / original.width() - 1.0);
            const double heightChange = std::abs(
                height / original.height() - 1.0);
            if (widthChange >= heightChange) {
                height = std::clamp(width / aspect,
                                    kMinimumPicturePoints,
                                    kMaximumPicturePoints);
            } else {
                width = std::clamp(height * aspect,
                                   kMinimumPicturePoints,
                                   kMaximumPicturePoints);
            }
        }
        imageResizeDrag_->previewRect = QRectF(
            original.topLeft(), QSizeF(width, height));
        viewport()->update();
        event->accept();
        return;
    }
    if (draggingTable_ && (event->buttons() & Qt::LeftButton)) {
        ensureLayout();
        const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
        const double contentY = event->position().y() +
            verticalScrollBar()->value() - kCanvasPaddingPixels;
        const double pageSpan = pageHeightPoints_ * scale + kPageGapPixels;
        const int page = std::clamp(
            static_cast<int>(std::floor(contentY / pageSpan)),
            0, std::max(0, pageCount_ - 1));
        const double pageTop = kCanvasPaddingPixels + page * pageSpan -
                               verticalScrollBar()->value();
        const double pointY = (event->position().y() - pageTop) / scale;
        tableDropBefore_.reset();
        for (const auto& block : blockPlacements_) {
            if (selectedTable_ && block.id == *selectedTable_) continue;
            const bool beforeBlock = page < block.firstPage ||
                (page == block.firstPage &&
                 pointY < (block.top + block.bottom) / 2.0);
            if (beforeBlock) {
                tableDropBefore_ = block.id;
                break;
            }
        }
        tableDropTargetValid_ = true;
        viewport()->update();
        event->accept();
        return;
    }
    const auto hover = hitTest(event->position().toPoint());
    if (hover.imageResizeHandle) {
        switch (*hover.imageResizeHandle) {
            case ImageResizeHandle::bottom_right:
                viewport()->setCursor(Qt::SizeFDiagCursor);
                break;
            case ImageResizeHandle::bottom:
                viewport()->setCursor(Qt::SizeVerCursor);
                break;
            case ImageResizeHandle::right:
                viewport()->setCursor(Qt::SizeHorCursor);
                break;
        }
    } else {
        viewport()->setCursor(
            hover.tableHandle ? Qt::SizeAllCursor
                              : hover.image ? Qt::ArrowCursor
                                            : Qt::IBeamCursor);
    }
    if (selecting_ && tableMouseSelectionAnchor_ &&
        (event->buttons() & Qt::LeftButton) && hover.tableCursor &&
        hover.tableCursor->tableId == tableMouseSelectionAnchor_->tableId) {
        const auto anchor = *tableMouseSelectionAnchor_;
        const auto focus = *hover.tableCursor;
        selectedTable_ = anchor.tableId;
        if (focus.row == anchor.row && focus.column == anchor.column) {
            tableCellSelection_.reset();
            tableCursor_ = focus;
            tableSelectionAnchor_ = anchor.utf16Offset;
        } else {
            tableCursor_.reset();
            tableSelectionAnchor_.reset();
            tableCellSelection_ = TableCellSelection{
                anchor.tableId, anchor.row, anchor.column,
                focus.row, focus.column};
        }
        typingFormat_ = selectedCharacterFormat();
        typingOverrideMask_ = {};
        commitPendingSpellingWordIfCaretLeft();
        emit selectionChanged();
        emitCursorFormat();
        updateStatus();
        viewport()->update();
        updateMicroFocus();
        event->accept();
        return;
    }
    if (selecting_ && (event->buttons() & Qt::LeftButton)) {
        const auto& hit = hover;
        if (hit.valid) {
            preferredVerticalX_.reset();
            lineAffinity_ = LineAffinity{hit.position.paragraph_id,
                                         hit.layoutLineStart};
            setCursor(hit.position, true, true);
        }
    }
}

void DocumentCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    selecting_ = false;
    tableMouseSelectionAnchor_.reset();
    if (imageResizeDrag_) {
        const auto drag = *imageResizeDrag_;
        imageResizeDrag_.reset();
        viewport()->setCursor(Qt::ArrowCursor);
        const bool changed =
            std::abs(drag.previewRect.width() - drag.originalRect.width()) >
                0.01 ||
            std::abs(drag.previewRect.height() - drag.originalRect.height()) >
                0.01;
        if (changed && drag.originalRect.width() > 0.0 &&
            drag.originalRect.height() > 0.0) {
            const double width = drag.originalWidthPoints *
                drag.previewRect.width() / drag.originalRect.width();
            const double height = drag.originalHeightPoints *
                drag.previewRect.height() / drag.originalRect.height();
            static_cast<void>(resizeSelectedInlineImage(width, height));
        }
        viewport()->update();
        event->accept();
        return;
    }
    if (draggingTable_) {
        draggingTable_ = false;
        viewport()->setCursor(Qt::IBeamCursor);
        if (tableDropTargetValid_ && selectedTable_) {
            const auto tableId = *selectedTable_;
            const auto cellCursor = tableCursor_;
            static_cast<void>(apply(
                {core::MoveTable{tableId, tableDropBefore_}}, std::nullopt,
                std::nullopt, false, std::nullopt, false, true,
                cellCursor, tableId));
        }
        tableDropTargetValid_ = false;
        tableDropBefore_.reset();
        viewport()->update();
        event->accept();
    }
}

void DocumentCanvas::contextMenuEvent(QContextMenuEvent* event) {
    if (previewId_) {
        event->accept();
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    const auto hit = hitTest(event->pos());
    if (hit.tableHandle) {
        static_cast<void>(selectTable(*hit.tableHandle));
    } else if (hit.image) {
        static_cast<void>(selectInlineImage(*hit.image));
    } else if (hit.tableCursor) {
        const bool insideSelectedCellRange = tableCellIsSelected(
            hit.tableCursor->tableId, hit.tableCursor->row,
            hit.tableCursor->column);
        if (!insideSelectedCellRange) {
            static_cast<void>(activateTableCell(
                hit.tableCursor->tableId, hit.tableCursor->row,
                hit.tableCursor->column, hit.tableCursor->utf16Offset));
        }
    } else if (hit.valid) {
        // Context actions must target the list under the pointer, not a stale
        // caret or selection elsewhere in the document.
        lineAffinity_ = LineAffinity{hit.position.paragraph_id,
                                     hit.layoutLineStart};
        setCursor(hit.position, false, true);
    }
    core::Range wordRange;
    std::optional<TableCursor> tableWordCell;
    std::optional<std::pair<std::size_t, std::size_t>> tableWordRange;
    QString word;
    if (hit.tableCursor) {
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(hit.tableCursor->tableId);
        const auto* cell = table
            ? table->cell(hit.tableCursor->row, hit.tableCursor->column)
            : nullptr;
        if (cell) {
            const QString text = fromUtf16(cell->text);
            const auto spellingWord = spellingWordAt(
                text, hit.tableCursor->utf16Offset);
            if (spellingWord) {
                word = spellingWord->text;
                tableWordCell = *hit.tableCursor;
                tableWordRange = std::pair{
                    spellingWord->start, spellingWord->end};
            }
        }
    } else if (hit.valid) {
        const QString text = paragraphText(hit.position.paragraph_id);
        const auto spellingWord = spellingWordAt(
            text, hit.position.utf16_offset);
        if (spellingWord) {
            word = spellingWord->text;
            wordRange = {
                {hit.position.paragraph_id, spellingWord->start},
                {hit.position.paragraph_id, spellingWord->end}};
        }
    }
    QMenu menu(this);
    if (selectedInlineImageId()) {
        if (selectedExcalidrawScene()) {
            auto* editFigure = menu.addAction(tr("Edit Excalidraw Figure…"));
            editFigure->setObjectName(
                QStringLiteral("context.editExcalidrawFigure"));
            connect(editFigure, &QAction::triggered, this,
                    &DocumentCanvas::editExcalidrawFigureRequested);
            menu.addSeparator();
        }
        auto* size = menu.addAction(tr("Picture Size…"));
        size->setObjectName(QStringLiteral("context.pictureSize"));
        connect(size, &QAction::triggered, this,
                &DocumentCanvas::showSelectedImageSizeDialog);
        auto* altText = menu.addAction(tr("Alt Text…"));
        altText->setObjectName(QStringLiteral("context.pictureAltText"));
        connect(altText, &QAction::triggered, this,
                &DocumentCanvas::showSelectedImageAltTextDialog);
        auto* layout = menu.addAction(tr("Wrap and Layout…"));
        layout->setObjectName(QStringLiteral("context.pictureLayout"));
        connect(layout, &QAction::triggered, this,
                &DocumentCanvas::showSelectedImageLayoutDialog);
        auto* remove = menu.addAction(tr("Delete Picture"));
        remove->setObjectName(QStringLiteral("context.deletePicture"));
        connect(remove, &QAction::triggered, this,
                [this] { static_cast<void>(deleteSelectedInlineImage()); });
        menu.addSeparator();
    }
    if (hasActiveList()) {
        auto* properties = menu.addAction(tr("List Properties…"));
        properties->setObjectName(QStringLiteral("context.listProperties"));
        connect(properties, &QAction::triggered, this,
                &DocumentCanvas::listPropertiesRequested);
        menu.addSeparator();
    }
    // Cell-range selection is distinct from selecting the table object. Keep
    // whole-table move/delete commands out of its context menu so a routine
    // cell-formatting click cannot accidentally remove the table.
    if (selectedTable_ && !tableCursor_ && !tableCellSelection_) {
        auto* earlier = menu.addAction(tr("Move Table Earlier"));
        earlier->setObjectName(QStringLiteral("context.moveTableEarlier"));
        earlier->setShortcut(QKeySequence(QStringLiteral("Alt+Shift+Up")));
        connect(earlier, &QAction::triggered, this,
                [this] { static_cast<void>(moveSelectedTable(false)); });
        auto* later = menu.addAction(tr("Move Table Later"));
        later->setObjectName(QStringLiteral("context.moveTableLater"));
        later->setShortcut(QKeySequence(QStringLiteral("Alt+Shift+Down")));
        connect(later, &QAction::triggered, this,
                [this] { static_cast<void>(moveSelectedTable(true)); });
        menu.addSeparator();
        auto* remove = menu.addAction(tr("Delete Table"));
        remove->setObjectName(QStringLiteral("context.deleteTable"));
        connect(remove, &QAction::triggered, this,
                [this] { static_cast<void>(deleteSelectedTable()); });
        menu.addSeparator();
    }
    if (!word.isEmpty() && !spelling_.isCorrect(word)) {
        const auto suggestions = spelling_.suggestions(word);
        for (const auto& suggestion : suggestions) {
            auto* action = menu.addAction(suggestion);
            action->setObjectName(
                QStringLiteral("context.spellingSuggestion"));
            if (tableWordCell && tableWordRange) {
                connect(
                    action, &QAction::triggered, this,
                    [this, cell = *tableWordCell,
                     range = *tableWordRange, suggestion] {
                        clearPendingSpellingWord();
                        if (!activateTableCell(
                                cell.tableId, cell.row, cell.column,
                                range.second)) {
                            return;
                        }
                        tableSelectionAnchor_ = range.first;
                        typingFormat_ = selectedCharacterFormat();
                        typingOverrideMask_ = {};
                        static_cast<void>(replaceTableCellText(
                            suggestion, false));
                    });
            } else {
                connect(
                    action, &QAction::triggered, this,
                    [this, wordRange, suggestion] {
                        clearPendingSpellingWord();
                        selection_ = wordRange;
                        typingFormat_ = selectedCharacterFormat();
                        typingOverrideMask_ =
                            selectedCharacterOverrideMask();
                        replaceSelection(suggestion);
                    });
            }
        }
        if (!suggestions.isEmpty()) menu.addSeparator();
        auto* ignore = menu.addAction(tr("Ignore all “%1”").arg(word));
        connect(ignore, &QAction::triggered, this, [this, word] {
            spelling_.ignoreAll(word); viewport()->update();
        });
        auto* add = menu.addAction(tr("Add to personal dictionary"));
        connect(add, &QAction::triggered, this, [this, word] {
            spelling_.addToPersonalDictionary(word); viewport()->update();
        });
        menu.addSeparator();
    }
    auto* cutAction = menu.addAction(tr("Cut"), this, &DocumentCanvas::cut);
    cutAction->setObjectName(QStringLiteral("context.cut"));
    cutAction->setEnabled(hasClipboardSelection());
    auto* copyAction = menu.addAction(tr("Copy"), this, &DocumentCanvas::copy);
    copyAction->setObjectName(QStringLiteral("context.copy"));
    copyAction->setEnabled(hasClipboardSelection());
    auto* pasteAction = menu.addAction(tr("Paste"), this,
                                       &DocumentCanvas::paste);
    pasteAction->setObjectName(QStringLiteral("context.paste"));
    auto* pasteTextOnlyAction = menu.addAction(
        tr("Paste as Text Only"), this, &DocumentCanvas::pasteTextOnly);
    pasteTextOnlyAction->setObjectName(
        QStringLiteral("context.pasteTextOnly"));
    const QMimeData* clipboardMime = QApplication::clipboard()->mimeData();
    pasteTextOnlyAction->setEnabled(
        clipboardMime && clipboardMime->hasText() &&
        !clipboardMime->text().isEmpty());
    menu.exec(event->globalPos());
}

void DocumentCanvas::focusInEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusInEvent(event); viewport()->update();
}
void DocumentCanvas::focusOutEvent(QFocusEvent* event) {
    endTypingGroup();
    clearPendingSpellingWord();
    imageResizeDrag_.reset();
    QAbstractScrollArea::focusOutEvent(event); viewport()->update();
}

void DocumentCanvas::setCursor(core::Position position, bool extend,
                               bool preserveVerticalNavigation) {
    if (previewId_) {
        return;
    }
    endTypingGroup();
    if (!preserveVerticalNavigation) {
        resetVerticalNavigation();
    }
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_.reset();
    draggingTable_ = false;
    tableDropTargetValid_ = false;
    tableDropBefore_.reset();
    const auto marker = plainTextListMarker(
        paragraphText(position.paragraph_id));
    if (marker) {
        position.utf16_offset = std::max(
            position.utf16_offset,
            static_cast<std::size_t>(marker->prefixLength));
    }
    if (extend) selection_.focus = position;
    else selection_ = {position, position};
    commitPendingSpellingWordIfCaretLeft();
    typingFormat_ = selection_.anchor == selection_.focus
        ? currentCharacterFormat()
        : selectedCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    revealCursor();
    viewport()->update();
    updateMicroFocus();
}

void DocumentCanvas::selectRange(core::Range range,
                                 const LineAffinity& lineAffinity) {
    endTypingGroup();
    preferredVerticalX_.reset();
    clearPendingSpellingWord();
    lineAffinity_ = lineAffinity;
    tableCursor_.reset();
    tableSelectionAnchor_.reset();
    tableCellSelection_.reset();
    tableMouseSelectionAnchor_.reset();
    selectedTable_.reset();
    draggingTable_ = false;
    tableDropTargetValid_ = false;
    tableDropBefore_.reset();
    selection_ = std::move(range);
    typingFormat_ = selectedCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    revealCursor();
    viewport()->update();
    updateMicroFocus();
}

std::vector<core::NodeId> DocumentCanvas::selectedParagraphIds() const {
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    std::vector<core::NodeId> ids;
    if (!normalized) return ids;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        ids.push_back(snap.document.paragraphs()[index].id());
    }
    return ids;
}

core::CharacterFormat DocumentCanvas::currentCharacterFormat() const {
    if (tableCursor_) {
        core::CharacterFormat result;
        result.font_family = defaultFontFamily_.toStdString();
        result.font_size_half_points = static_cast<std::int32_t>(
            std::lround(defaultFontPointSize_ * 2.0));
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        if (table && table->hasHeaderRow() && tableCursor_->row == 0) {
            result.bold = true;
        }
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        return cell
            ? resolvedCharacterFormat(
                  std::move(result),
                  cell->characterFormatAt(tableCursor_->utf16Offset))
            : result;
    }
    const auto snap = session_->snapshot();
    const auto* paragraph = snap.document.findParagraph(selection_.focus.paragraph_id);
    if (!paragraph) return {};
    return paragraph->characterFormatAt(selection_.focus.utf16_offset);
}

core::CharacterFormat DocumentCanvas::selectedCharacterFormat() const {
    const auto snap = session_->snapshot();
    if (selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        if (!table) return {};
        if (tableCursor_) {
            const auto* cell = table->cell(tableCursor_->row,
                                           tableCursor_->column);
            if (!cell) return {};
            const auto focus = std::min(tableCursor_->utf16Offset,
                                        cell->text.size());
            const auto anchor = std::min(
                tableSelectionAnchor_.value_or(focus), cell->text.size());
            const auto offset = std::min(anchor, focus);
            core::CharacterFormat inherited;
            inherited.font_family = defaultFontFamily_.toStdString();
            inherited.font_size_half_points = static_cast<std::int32_t>(
                std::lround(defaultFontPointSize_ * 2.0));
            inherited.bold = table->hasHeaderRow() &&
                             tableCursor_->row == 0;
            return resolvedCharacterFormat(
                std::move(inherited), cell->characterFormatAt(offset + 1));
        }
        const auto cells = selectedTableCells(*table);
        if (cells.empty()) return {};
        const auto [row, column] = cells.front();
        const auto* cell = table->cell(row, column);
        if (!cell) return {};
        core::CharacterFormat inherited;
        inherited.font_family = defaultFontFamily_.toStdString();
        inherited.font_size_half_points = static_cast<std::int32_t>(
            std::lround(defaultFontPointSize_ * 2.0));
        inherited.bold = table->hasHeaderRow() && row == 0;
        return resolvedCharacterFormat(
            std::move(inherited), cell->characterFormatAt(1));
    }
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized || normalized.value().empty()) {
        return currentCharacterFormat();
    }
    const auto& range = normalized.value();
    for (std::size_t index = range.start_paragraph_index;
         index <= range.end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const std::size_t start = index == range.start_paragraph_index
            ? range.start.utf16_offset
            : 0;
        const std::size_t end = index == range.end_paragraph_index
            ? range.end.utf16_offset
            : paragraph.text().size();
        if (start < end) {
            return paragraph.characterFormatAt(start + 1);
        }
    }
    return {};
}

core::CharacterFormatMask
DocumentCanvas::selectedCharacterOverrideMask() const {
    if (selectedTable_ || tableCursor_ || tableCellSelection_) return {};
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) return {};
    const auto& range = normalized.value();
    if (!range.empty()) {
        for (std::size_t index = range.start_paragraph_index;
             index <= range.end_paragraph_index; ++index) {
            const auto& paragraph = snap.document.paragraphs()[index];
            const std::size_t start = index == range.start_paragraph_index
                ? range.start.utf16_offset
                : 0;
            const std::size_t end = index == range.end_paragraph_index
                ? range.end.utf16_offset
                : paragraph.text().size();
            if (start < end) {
                return paragraph.styleOverrideMaskAt(start + 1);
            }
        }
    }
    const auto* paragraph = snap.document.findParagraph(
        selection_.focus.paragraph_id);
    return paragraph
        ? paragraph->styleOverrideMaskAt(selection_.focus.utf16_offset)
        : core::CharacterFormatMask{};
}

core::CharacterFormat DocumentCanvas::activeCharacterFormat() const {
    if (tableCursor_) {
        const bool hasSelection = tableSelectionAnchor_ &&
            *tableSelectionAnchor_ != tableCursor_->utf16Offset;
        return hasSelection ? selectedCharacterFormat() : typingFormat_;
    }
    if (selectedTable_) return selectedCharacterFormat();
    return selection_.anchor == selection_.focus ? typingFormat_
                                                  : selectedCharacterFormat();
}

void DocumentCanvas::applyCharacterFormat(const core::CharacterFormatDelta& delta) {
    applyCharacterFormatInternal(delta, false);
}

void DocumentCanvas::applyCharacterFormatInternal(
    const core::CharacterFormatDelta& delta, bool coalesceWithPrevious) {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    if (selectedTable_) {
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(*selectedTable_);
        if (!table) return;
        auto resultingTypingFormat = activeCharacterFormat();
        delta.applyTo(resultingTypingFormat);
        std::vector<core::Operation> operations;
        if (tableCursor_) {
            const auto* cell = table->cell(tableCursor_->row,
                                           tableCursor_->column);
            if (!cell) return;
            const auto focus = std::min(tableCursor_->utf16Offset,
                                        cell->text.size());
            const auto anchor = std::min(
                tableSelectionAnchor_.value_or(focus), cell->text.size());
            const auto start = std::min(anchor, focus);
            const auto end = std::max(anchor, focus);
            if (start == end && !cell->text.empty()) {
                typingFormat_ = std::move(resultingTypingFormat);
                typingOverrideMask_ = {};
                emitCursorFormat();
                return;
            }
            operations.emplace_back(core::SetTableCellCharacterFormat{
                table->id(), tableCursor_->row, tableCursor_->column,
                start, end, delta});
        } else {
            for (const auto& [row, column] : selectedTableCells(*table)) {
                const auto* cell = table->cell(row, column);
                if (!cell) continue;
                operations.emplace_back(core::SetTableCellCharacterFormat{
                    table->id(), row, column, 0, cell->text.size(), delta});
            }
        }
        if (operations.empty()) {
            typingFormat_ = std::move(resultingTypingFormat);
            typingOverrideMask_ = {};
            emitCursorFormat();
            return;
        }
        apply(std::move(operations), std::nullopt,
              std::move(resultingTypingFormat), false, std::nullopt,
              coalesceWithPrevious);
        return;
    }
    // Formatting a selection also changes the active typing attributes. If it
    // is immediately replaced, the new text uses the color/font/style the
    // user just chose.
    auto resultingTypingOverrideMask = typingOverrideMask_;
    resultingTypingOverrideMask.mark(delta);
    if (selection_.anchor == selection_.focus) {
        const auto snap = session_->snapshot();
        const auto* paragraph = snap.document.findParagraph(
            selection_.focus.paragraph_id);
        if (paragraph && paragraph->text().empty()) {
            const std::array<core::NodeId, 1> paragraphIds{
                paragraph->id()};
            auto initialization =
                planBuiltInStyleProvenanceInitialization(
                    snap.document, paragraphIds, defaultFontFamily_,
                    defaultFontPointSize_, typingFormat_, selection_.focus,
                    typingOverrideMask_);
            auto resultingTypingFormat =
                initialization.resultingTypingFormat.value_or(typingFormat_);
            resultingTypingOverrideMask =
                initialization.resultingTypingOverrideMask.value_or(
                    typingOverrideMask_);
            resultingTypingOverrideMask.mark(delta);
            delta.applyTo(resultingTypingFormat);
            // Empty lines have no character run to recover from after the
            // user clicks elsewhere, so keep their insertion format in the
            // semantic paragraph mark. A non-empty caret remains transient
            // until text is actually entered, matching normal editor
            // behavior and avoiding an invisible document mutation.
            initialization.operations.emplace_back(
                core::SetParagraphMarkCharacterFormat{
                    selection_.focus.paragraph_id, delta});
            apply(std::move(initialization.operations), std::nullopt,
                  std::move(resultingTypingFormat), false, selection_,
                  coalesceWithPrevious, false, std::nullopt, std::nullopt,
                  std::nullopt, resultingTypingOverrideMask);
        } else {
            auto resultingTypingFormat = typingFormat_;
            delta.applyTo(resultingTypingFormat);
            typingFormat_ = std::move(resultingTypingFormat);
            typingOverrideMask_ = resultingTypingOverrideMask;
            emitCursorFormat();
        }
        return;
    }

    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return;
    }
    bool requiresMutation = false;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index &&
         !requiresMutation; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const std::size_t start =
            index == normalized.value().start_paragraph_index
            ? normalized.value().start.utf16_offset
            : 0;
        const std::size_t end =
            index == normalized.value().end_paragraph_index
            ? normalized.value().end.utf16_offset
            : paragraph.text().size();
        if (start == end && paragraph.text().empty()) {
            requiresMutation = characterDeltaRequiresMutation(
                paragraph, 0, true, delta, defaultFontFamily_,
                defaultFontPointSize_);
            continue;
        }
        for (std::size_t offset = start; offset < end; ++offset) {
            if (characterDeltaRequiresMutation(
                    paragraph, offset + 1, false, delta,
                    defaultFontFamily_, defaultFontPointSize_)) {
                requiresMutation = true;
                break;
            }
        }
    }
    if (!requiresMutation) {
        emitCursorFormat();
        return;
    }
    std::vector<core::NodeId> affectedParagraphs;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const std::size_t start =
            index == normalized.value().start_paragraph_index
            ? normalized.value().start.utf16_offset
            : 0;
        const std::size_t end =
            index == normalized.value().end_paragraph_index
            ? normalized.value().end.utf16_offset
            : paragraph.text().size();
        if (start < end || paragraph.text().empty()) {
            affectedParagraphs.push_back(paragraph.id());
        }
    }
    auto initialization = planBuiltInStyleProvenanceInitialization(
        snap.document, affectedParagraphs, defaultFontFamily_,
        defaultFontPointSize_, typingFormat_, selection_.focus,
        typingOverrideMask_);
    auto resultingTypingFormat =
        initialization.resultingTypingFormat.value_or(typingFormat_);
    resultingTypingOverrideMask =
        initialization.resultingTypingOverrideMask.value_or(
            typingOverrideMask_);
    resultingTypingOverrideMask.mark(delta);
    delta.applyTo(resultingTypingFormat);
    initialization.operations.emplace_back(
        core::SetCharacterFormat{selection_, delta});
    apply(std::move(initialization.operations), std::nullopt,
          std::move(resultingTypingFormat), false, std::nullopt,
          coalesceWithPrevious, false, std::nullopt, std::nullopt,
          std::nullopt, resultingTypingOverrideMask);
}

void DocumentCanvas::applyParagraphFormat(const core::ParagraphFormatDelta& delta) {
    if (selectedTable_) {
        if (rejectLiveEditDuringPreview()) return;
        const auto snap = session_->snapshot();
        const auto* table = snap.document.findTable(*selectedTable_);
        if (!table) return;
        std::vector<core::Operation> operations;
        for (const auto& [row, column] : selectedTableCells(*table)) {
            operations.emplace_back(core::SetTableCellParagraphFormat{
                table->id(), row, column, delta});
        }
        if (!operations.empty()) apply(std::move(operations));
        return;
    }
    if (rejectLiveEditDuringPreview()) return;
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return;
    }
    std::vector<core::NodeId> paragraphIds;
    paragraphIds.reserve(
        normalized.value().end_paragraph_index -
        normalized.value().start_paragraph_index + 1);
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        paragraphIds.push_back(snap.document.paragraphs()[index].id());
    }
    const bool requiresMutation = std::any_of(
        paragraphIds.begin(), paragraphIds.end(),
        [&snap, &delta](core::NodeId id) {
            const auto* paragraph = snap.document.findParagraph(id);
            return paragraph && paragraphDeltaRequiresMutation(
                                    *paragraph, delta);
        });
    if (!requiresMutation) return;
    auto initialization = planBuiltInStyleProvenanceInitialization(
        snap.document, paragraphIds, defaultFontFamily_,
        defaultFontPointSize_, typingFormat_, selection_.focus,
        typingOverrideMask_);
    initialization.operations.emplace_back(
        core::SetParagraphFormat{std::move(paragraphIds), delta});
    const auto resultingTypingOverrideMask =
        initialization.resultingTypingOverrideMask.value_or(
            typingOverrideMask_);
    apply(std::move(initialization.operations), std::nullopt,
          std::move(initialization.resultingTypingFormat), false,
          std::nullopt, false, false, std::nullopt, std::nullopt,
          std::nullopt, resultingTypingOverrideMask);
}

bool DocumentCanvas::paragraphStylesAvailable() const noexcept {
    return !previewId_ && !selectedTable_ && !tableCursor_ &&
           !tableCellSelection_;
}

QString DocumentCanvas::currentParagraphStyleId() const {
    if (selectedTable_ || tableCursor_ || tableCellSelection_) return {};
    const auto snap = visibleDocumentSnapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) return {};

    std::optional<std::string> common;
    for (std::size_t index = normalized.value().start_paragraph_index;
         index <= normalized.value().end_paragraph_index; ++index) {
        const auto& paragraph = snap.document.paragraphs()[index];
        const std::string id = paragraph.styleId().value_or("Normal");
        if (!common) {
            common = id;
        } else if (*common != id) {
            return {};
        }
    }
    return common ? QString::fromUtf8(
                        common->data(), static_cast<qsizetype>(common->size()))
                  : QString{};
}

void DocumentCanvas::applyParagraphStyle(const QString& styleId) {
    applyParagraphStyleInternal(styleId, false);
}

std::vector<core::Operation> DocumentCanvas::planParagraphStyleOperations(
    const core::Document& document,
    const std::vector<core::NodeId>& paragraphIds,
    const core::ParagraphStyleDefinition& target,
    bool ensureTargetProvenance) const {
    auto plan = planParagraphStyleApplication(
        document, paragraphIds, target, defaultFontFamily_,
        defaultFontPointSize_, core::CharacterFormat{}, std::nullopt,
        ensureTargetProvenance);
    return std::move(plan.operations);
}

void DocumentCanvas::applyParagraphStyleInternal(
    const QString& styleId, bool coalesceWithPrevious) {
    if (selectedTable_ || tableCursor_ || tableCellSelection_) {
        emit operationFailed(tr(
            "Paragraph styles are not available inside or across table cells."));
        return;
    }
    if (rejectLiveEditDuringPreview()) return;
    const QByteArray encoded = styleId.toUtf8();
    const std::string targetId(encoded.constData(),
                               static_cast<std::size_t>(encoded.size()));
    const auto* target = core::findBuiltInParagraphStyle(targetId);
    if (!target) {
        emit operationFailed(tr("Unknown paragraph style."));
        return;
    }

    const auto snap = session_->snapshot();
    const auto ids = selectedParagraphIds();
    if (ids.empty()) return;
    auto plan = planParagraphStyleApplication(
        snap.document, ids, *target, defaultFontFamily_,
        defaultFontPointSize_, typingFormat_, selection_.focus, false,
        typingOverrideMask_);
    if (plan.operations.empty()) return;
    apply(std::move(plan.operations), std::nullopt,
          std::move(plan.resultingTypingFormat), false, std::nullopt,
          coalesceWithPrevious, false, std::nullopt, std::nullopt,
          std::nullopt, std::move(plan.resultingTypingOverrideMask));
}

void DocumentCanvas::toggleBold() {
    const bool current = activeCharacterFormat().bold.value_or(false);
    core::CharacterFormatDelta delta; delta.bold = core::PropertyDelta<bool>::set(!current);
    applyCharacterFormat(delta);
}
void DocumentCanvas::toggleItalic() {
    const bool current = activeCharacterFormat().italic.value_or(false);
    core::CharacterFormatDelta delta; delta.italic = core::PropertyDelta<bool>::set(!current);
    applyCharacterFormat(delta);
}
void DocumentCanvas::toggleUnderline() {
    const auto current = activeCharacterFormat().underline.value_or(core::UnderlineStyle::none);
    core::CharacterFormatDelta delta;
    delta.underline = core::PropertyDelta<core::UnderlineStyle>::set(
        current == core::UnderlineStyle::none ? core::UnderlineStyle::single : core::UnderlineStyle::none);
    applyCharacterFormat(delta);
}
void DocumentCanvas::toggleStrike() {
    const bool current = activeCharacterFormat().strike.value_or(false);
    core::CharacterFormatDelta delta; delta.strike = core::PropertyDelta<bool>::set(!current);
    applyCharacterFormat(delta);
}
void DocumentCanvas::setBaseline(core::BaselinePosition baseline) {
    core::CharacterFormatDelta delta;
    delta.baseline = core::PropertyDelta<core::BaselinePosition>::set(baseline);
    applyCharacterFormat(delta);
}
void DocumentCanvas::toggleBaseline(core::BaselinePosition baseline) {
    const auto current = activeCharacterFormat().baseline.value_or(
        core::BaselinePosition::normal);
    setBaseline(current == baseline ? core::BaselinePosition::normal
                                    : baseline);
}
void DocumentCanvas::setFontFamily(const QString& family) {
    core::CharacterFormatDelta delta;
    delta.font_family = core::PropertyDelta<std::string>::set(family.toStdString());
    applyCharacterFormat(delta);
}
void DocumentCanvas::setFontPointSize(double points) {
    core::CharacterFormatDelta delta;
    delta.font_size_half_points = core::PropertyDelta<std::int32_t>::set(
        static_cast<std::int32_t>(std::lround(points * 2.0)));
    applyCharacterFormat(delta);
}
void DocumentCanvas::setForeground(const QColor& color) {
    const auto before = session_->snapshot().revision;
    core::CharacterFormatDelta delta;
    delta.foreground_argb = core::PropertyDelta<std::uint32_t>::set(toArgb(color));
    const bool coalesce = colorAdjustmentActive_ &&
                          colorAdjustmentLastRevision_ == before;
    applyCharacterFormatInternal(delta, coalesce);
    const auto after = session_->snapshot().revision;
    if (colorAdjustmentActive_ && after != before) {
        colorAdjustmentLastRevision_ = after;
    }
}
void DocumentCanvas::setHighlight(const QColor& color) {
    if (!color.isValid()) {
        clearHighlight();
        return;
    }
    const auto before = session_->snapshot().revision;
    core::CharacterFormatDelta delta;
    delta.highlight_argb = core::PropertyDelta<std::uint32_t>::set(toArgb(color));
    const bool coalesce = colorAdjustmentActive_ &&
                          colorAdjustmentLastRevision_ == before;
    applyCharacterFormatInternal(delta, coalesce);
    const auto after = session_->snapshot().revision;
    if (colorAdjustmentActive_ && after != before) {
        colorAdjustmentLastRevision_ = after;
    }
}

void DocumentCanvas::clearHighlight() {
    core::CharacterFormatDelta delta;
    delta.highlight_argb = core::PropertyDelta<std::uint32_t>::clear();
    applyCharacterFormat(delta);
}

void DocumentCanvas::beginColorAdjustment() {
    endTypingGroup();
    colorAdjustmentActive_ = true;
    colorAdjustmentLastRevision_.reset();
}

void DocumentCanvas::endColorAdjustment() noexcept {
    colorAdjustmentActive_ = false;
    colorAdjustmentLastRevision_.reset();
}

QColor DocumentCanvas::currentTextColor() const {
    return fromArgb(
        activeCharacterFormat().foreground_argb.value_or(kDefaultTextArgb));
}

QColor DocumentCanvas::currentHighlightColor() const {
    const auto highlight = activeCharacterFormat().highlight_argb;
    return highlight ? fromArgb(*highlight) : QColor{};
}

QString DocumentCanvas::currentFontFamily() const {
    const auto format = activeCharacterFormat();
    return format.font_family ? QString::fromStdString(*format.font_family)
                              : defaultFontFamily_;
}

double DocumentCanvas::currentFontPointSize() const {
    const auto format = activeCharacterFormat();
    return format.font_size_half_points
        ? *format.font_size_half_points / 2.0
        : defaultFontPointSize_;
}

void DocumentCanvas::refreshCursorFormat() { emitCursorFormat(); }
void DocumentCanvas::setAlignment(core::ParagraphAlignment alignment) {
    core::ParagraphFormatDelta delta;
    delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(alignment);
    applyParagraphFormat(delta);
}

void DocumentCanvas::insertPageBreak() {
    if (selectedTable_) {
        emit operationFailed(tr(
            "Page breaks cannot be inserted inside a table cell yet."));
        return;
    }
    const auto snap = session_->snapshot();
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized) {
        emit operationFailed(errorText(normalized.error()));
        return;
    }
    const auto newId = core::NodeId::generate();
    std::vector<core::Operation> operations;
    if (!normalized.value().empty()) {
        operations.push_back(core::DeleteRange{selection_});
    }
    operations.push_back(core::SplitParagraph{
        normalized.value().start, newId,
        std::optional<core::CharacterFormat>(typingFormat_)});
    core::ParagraphFormatDelta delta;
    delta.page_break_before = core::PropertyDelta<bool>::set(true);
    operations.push_back(core::SetParagraphFormat{{newId}, delta});
    apply(std::move(operations), core::Position{newId, 0});
}

QString DocumentCanvas::headerText() const {
    return fromUtf16(session_->snapshot().document.headerText());
}

QString DocumentCanvas::footerText() const {
    return fromUtf16(session_->snapshot().document.footerText());
}

bool DocumentCanvas::setHeaderText(const QString& text) {
    return apply({core::SetHeaderText{toUtf16(text)}});
}

bool DocumentCanvas::setFooterText(const QString& text) {
    return apply({core::SetFooterText{toUtf16(text)}});
}

bool DocumentCanvas::setHeaderFooterText(const QString& header,
                                         const QString& footer) {
    return apply({core::SetHeaderText{toUtf16(header)},
                  core::SetFooterText{toUtf16(footer)}});
}

std::array<QString, 3> DocumentCanvas::splitStorySections(
    const QString& text) {
    const QStringList parts = text.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
    std::array<QString, 3> result{};
    if (!parts.isEmpty()) result[0] = parts[0];
    if (parts.size() > 1) result[1] = parts[1];
    if (parts.size() > 2) {
        result[2] = parts.mid(2).join(QStringLiteral(" "));
    }
    return result;
}

QString DocumentCanvas::joinStorySections(
    const std::array<QPlainTextEdit*, 3>& editors) {
    return editors[0]->toPlainText() + QLatin1Char('\t') +
           editors[1]->toPlainText() + QLatin1Char('\t') +
           editors[2]->toPlainText();
}

void DocumentCanvas::ensureStoryEditors() {
    if (headerEditors_[0]) return;
    const auto create = [this](std::array<QPlainTextEdit*, 3>& editors,
                               const QString& prefix) {
        for (int index = 0; index < 3; ++index) {
            auto* editor = new StoryTextEdit(viewport());
            editor->setObjectName(prefix + QString::number(index));
            editor->setAccessibleName(
                (prefix.startsWith(QStringLiteral("header"))
                     ? tr("Header") : tr("Footer")) +
                QStringLiteral(" ") +
                (index == 0 ? tr("left")
                            : index == 1 ? tr("center") : tr("right")));
            editor->setFrameShape(QFrame::NoFrame);
            editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            editor->setTabChangesFocus(true);
            editor->setWordWrapMode(QTextOption::WordWrap);
            editor->setStyleSheet(QStringLiteral(
                "QPlainTextEdit { background: rgba(255,255,255,235); "
                "border: 1px dashed #7b8794; padding: 2px; color: #000000; } "
                "QPlainTextEdit:focus { border: 2px solid #e95420; }"));
            QFont font(defaultFontFamily_);
            font.setPointSizeF(defaultFontPointSize_);
            editor->setFont(font);
            QTextOption option = editor->document()->defaultTextOption();
            option.setAlignment(index == 0 ? Qt::AlignLeft
                                           : index == 1 ? Qt::AlignHCenter
                                                        : Qt::AlignRight);
            editor->document()->setDefaultTextOption(option);
            editor->hide();
            connect(editor, &QPlainTextEdit::textChanged, this,
                    [this] { applyStoryEditorText(); });
            editor->pasteImage = [this, editor](const QMimeData* mime) {
                static_cast<void>(pasteStoryImage(editor, mime));
            };
            editor->activated = [this, index] {
                activeStoryRegion_ = index;
            };
            editors[static_cast<std::size_t>(index)] = editor;
        }
    };
    create(headerEditors_, QStringLiteral("headerStoryEditor"));
    create(footerEditors_, QStringLiteral("footerStoryEditor"));
}

void DocumentCanvas::loadStoryEditors(bool footer) {
    ensureStoryEditors();
    loadingStoryEditors_ = true;
    const auto values = splitStorySections(footer ? footerText() : headerText());
    auto& editors = footer ? footerEditors_ : headerEditors_;
    for (int index = 0; index < 3; ++index) {
        const QSignalBlocker blocker(
            editors[static_cast<std::size_t>(index)]);
        editors[static_cast<std::size_t>(index)]->setPlainText(
            values[static_cast<std::size_t>(index)]);
    }
    loadingStoryEditors_ = false;
}

void DocumentCanvas::beginHeaderFooterEditing(bool footer, int section,
                                               int pageIndex) {
    if (previewId_) return;
    ensureLayout();
    ensureStoryEditors();
    headerFooterEditing_ = true;
    activeStoryIsFooter_ = footer;
    activeStoryRegion_ = std::clamp(section, 0, 2);
    activeStoryPage_ = std::clamp(pageIndex, 0, std::max(0, pageCount_ - 1));
    storyEditHasTransaction_ = false;
    loadStoryEditors(footer);
    updateStoryEditorGeometry();
    auto& editors = footer ? footerEditors_ : headerEditors_;
    auto* target = editors[static_cast<std::size_t>(activeStoryRegion_)];
    target->setFocus();
    target->moveCursor(QTextCursor::End);
    viewport()->update();
}

void DocumentCanvas::endHeaderFooterEditing() {
    if (!headerFooterEditing_) return;
    for (auto* editor : headerEditors_) if (editor) editor->hide();
    for (auto* editor : footerEditors_) if (editor) editor->hide();
    headerFooterEditing_ = false;
    storyEditHasTransaction_ = false;
    setFocus();
    viewport()->update();
}

void DocumentCanvas::applyStoryEditorText() {
    if (loadingStoryEditors_ || !headerFooterEditing_) return;
    const auto& editors = activeStoryIsFooter_ ? footerEditors_ : headerEditors_;
    std::vector<core::Operation> operations;
    if (activeStoryIsFooter_) {
        operations.push_back(core::SetFooterText{
            toUtf16(joinStorySections(editors))});
    } else {
        operations.push_back(core::SetHeaderText{
            toUtf16(joinStorySections(editors))});
    }
    if (apply(std::move(operations), std::nullopt, std::nullopt, false,
              std::nullopt, storyEditHasTransaction_)) {
        storyEditHasTransaction_ = true;
    }
}

bool DocumentCanvas::pasteStoryImage(
    QPlainTextEdit* editor, const QMimeData* mime,
    const QString& accessibleNameOverride) {
    if (!editor || !mime || !headerFooterEditing_ ||
        rejectLiveEditDuringPreview()) {
        return false;
    }
    std::vector<std::uint8_t> encodedBytes;
    QString accessibleName = tr("Pasted picture");
    std::optional<std::int64_t> requestedWidth;
    std::optional<std::int64_t> requestedHeight;
    core::ImageFormat requestedFormat = core::ImageFormat::png;
    const QString nativeFormat = mime->hasFormat(
        QString::fromLatin1(kInlineImageClipboardMime))
        ? QString::fromLatin1(kInlineImageClipboardMime)
        : mime->hasFormat(QString::fromLatin1(
              kInlineImageClipboardLegacyMime))
            ? QString::fromLatin1(kInlineImageClipboardLegacyMime)
            : QString();
    if (!nativeFormat.isEmpty()) {
        const auto image = decodeClipboardInlineImage(
            mime->data(nativeFormat));
        if (!image) {
            emit operationFailed(tr(
                "The Owl Docs picture on the clipboard is malformed or exceeds the safety limits."));
            return false;
        }
        encodedBytes = image->encodedBytes;
        accessibleName = image->accessibleName;
        requestedWidth = image->widthEmu;
        requestedHeight = image->heightEmu;
        requestedFormat = image->format;
    } else {
        QString format;
        for (const auto& candidate : {QStringLiteral("image/png"),
                                      QStringLiteral("image/jpeg")}) {
            if (mime->hasFormat(candidate)) {
                format = candidate;
                break;
            }
        }
        QByteArray encoded;
        if (!format.isEmpty()) {
            encoded = mime->data(format);
        } else if (mime->hasImage()) {
            const QImage clipboardImage =
                qvariant_cast<QImage>(mime->imageData());
            if (clipboardImage.isNull() || clipboardImage.width() > 16'384 ||
                clipboardImage.height() > 16'384 ||
                static_cast<std::int64_t>(clipboardImage.width()) *
                        clipboardImage.height() >
                    64LL * 1024LL * 1024LL) {
                emit operationFailed(tr(
                    "The clipboard picture exceeds the supported image dimensions."));
                return false;
            }
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly) ||
                !clipboardImage.save(&buffer, "PNG")) {
                emit operationFailed(tr(
                    "The clipboard picture could not be encoded as PNG."));
                return false;
            }
            format = QStringLiteral("image/png");
        } else {
            return false;
        }
        if (encoded.isEmpty() || encoded.size() >
                static_cast<qsizetype>(core::kMaximumEncodedImageBytes)) {
            emit operationFailed(tr(
                "The clipboard picture is empty or exceeds the 16 MiB encoded-picture limit."));
            return false;
        }
        encodedBytes.assign(
            reinterpret_cast<const std::uint8_t*>(encoded.constData()),
            reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
                encoded.size());
        requestedFormat = format == QStringLiteral("image/png")
            ? core::ImageFormat::png : core::ImageFormat::jpeg;
    }

    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = core::kMaximumEncodedImageBytes;
    limits.maximum_decoded_bytes = std::min(
        limits.maximum_decoded_bytes,
        kMaximumAggregateDecodedRasterBytes -
            std::min(decodedImageBytes_,
                     kMaximumAggregateDecodedRasterBytes));
    auto decoded = decodeRasterImage(encodedBytes, limits);
    const auto expected = requestedFormat == core::ImageFormat::png
        ? raster::Format::png : raster::Format::jpeg;
    if (!decoded.ok() || decoded.format != expected) {
        emit operationFailed(tr(
            "The picture is not a valid bounded PNG or JPEG image."));
        return false;
    }

    const double storyHeight = activeStoryIsFooter_
        ? std::max(18.0, marginBottomPoints_ - 14.0)
        : std::max(18.0, marginTopPoints_ - 21.0);
    const double storyWidth = std::max(
        18.0, (pageWidthPoints_ - marginLeftPoints_ -
               marginRightPoints_) / 3.0 - 6.0);
    double widthPoints = requestedWidth
        ? static_cast<double>(*requestedWidth) / kEmuPerPoint
        : static_cast<double>(decoded.image.width()) * 0.75;
    const double sourceAspect = static_cast<double>(decoded.image.width()) /
        static_cast<double>(decoded.image.height());
    double heightPoints = widthPoints / sourceAspect;
    if (requestedHeight && !requestedWidth) {
        heightPoints = static_cast<double>(*requestedHeight) / kEmuPerPoint;
        widthPoints = heightPoints * sourceAspect;
    }
    const double fit = std::min(
        {1.0, storyWidth / std::max(1.0, widthPoints),
         storyHeight / std::max(1.0, heightPoints)});
    const auto widthEmu = static_cast<std::int64_t>(std::llround(
        widthPoints * fit * kEmuPerPoint));
    const auto heightEmu = static_cast<std::int64_t>(std::llround(
        heightPoints * fit * kEmuPerPoint));

    auto& editors = activeStoryIsFooter_ ? footerEditors_ : headerEditors_;
    const auto found = std::find(editors.begin(), editors.end(), editor);
    if (found == editors.end()) return false;
    const int region = static_cast<int>(std::distance(editors.begin(), found));
    auto sections = splitStorySections(
        activeStoryIsFooter_ ? footerText() : headerText());
    QTextCursor cursor = editor->textCursor();
    const int selectionStart = std::min(
        cursor.anchor(), cursor.position());
    const int selectionEnd = std::max(
        cursor.anchor(), cursor.position());
    sections[static_cast<std::size_t>(region)].remove(
        selectionStart, selectionEnd - selectionStart);
    std::size_t globalOffset = static_cast<std::size_t>(selectionStart);
    for (int index = 0; index < region; ++index) {
        globalOffset += static_cast<std::size_t>(
            sections[static_cast<std::size_t>(index)].size()) + 1U;
    }
    const QString updated = sections[0] + QLatin1Char('\t') +
        sections[1] + QLatin1Char('\t') + sections[2];
    QString safeName = (accessibleNameOverride.isEmpty()
                            ? accessibleName
                            : accessibleNameOverride).trimmed();
    if (safeName.isEmpty()) safeName = tr("Picture");
    QByteArray encodedName = safeName.toUtf8();
    while (encodedName.size() >
               static_cast<qsizetype>(core::kMaximumImageAccessibleNameBytes) &&
           !safeName.isEmpty()) {
        safeName.chop(1);
        encodedName = safeName.toUtf8();
    }
    const auto imageId = core::NodeId::generate();
    std::vector<core::Operation> operations;
    operations.emplace_back(activeStoryIsFooter_
        ? core::Operation(core::SetFooterText{toUtf16(updated)})
        : core::Operation(core::SetHeaderText{toUtf16(updated)}));
    operations.emplace_back(core::InsertHeaderFooterImage{
        activeStoryIsFooter_, globalOffset,
        core::EncodedImagePayload(std::move(encodedBytes)), requestedFormat,
        encodedName.toStdString(), widthEmu, heightEmu, imageId});
    if (!apply(std::move(operations))) return false;
    const auto [cached, inserted] = decodedImages_.emplace(
        imageId, std::move(decoded.image));
    if (inserted) decodedImageBytes_ += cached->second.sizeInBytes();
    loadStoryEditors(activeStoryIsFooter_);
    auto* target = editors[static_cast<std::size_t>(region)];
    QTextCursor restored = target->textCursor();
    restored.setPosition(selectionStart + 1);
    target->setTextCursor(restored);
    target->setFocus();
    storyEditHasTransaction_ = false;
    viewport()->update();
    return true;
}

void DocumentCanvas::updateStoryEditorGeometry() {
    if (!headerFooterEditing_ || !headerEditors_[0]) return;
    for (auto* editor : headerEditors_) editor->hide();
    for (auto* editor : footerEditors_) editor->hide();

    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    QFont editorFont(defaultFontFamily_);
    editorFont.setPointSizeF(
        defaultFontPointSize_ * static_cast<double>(zoomPercent_) / 100.0);
    const double pagePixelWidth = pageWidthPoints_ * scale;
    const double documentWidth = std::max(
        pagePixelWidth + 2 * kCanvasPaddingPixels,
        static_cast<double>(viewport()->width()));
    const double pageLeft = (documentWidth - pagePixelWidth) / 2.0 -
                            horizontalScrollBar()->value();
    const double pageTop = kCanvasPaddingPixels + activeStoryPage_ *
        (pageHeightPoints_ * scale + kPageGapPixels) -
        verticalScrollBar()->value();
    const double left = pageLeft + marginLeftPoints_ * scale;
    const double width = std::max(
        3.0, (pageWidthPoints_ - marginLeftPoints_ - marginRightPoints_) * scale);
    const double yPoints = activeStoryIsFooter_
        ? pageHeightPoints_ - marginBottomPoints_ + 7.0 : 14.0;
    const double heightPoints = activeStoryIsFooter_
        ? std::max(18.0, marginBottomPoints_ - 14.0)
        : std::max(18.0, marginTopPoints_ - 21.0);
    const int y = static_cast<int>(std::round(pageTop + yPoints * scale));
    const int height = std::max(24, static_cast<int>(std::round(
        heightPoints * scale)));
    const int gap = 3;
    auto& editors = activeStoryIsFooter_ ? footerEditors_ : headerEditors_;
    for (int index = 0; index < 3; ++index) {
        const int x0 = static_cast<int>(std::round(
            left + width * static_cast<double>(index) / 3.0));
        const int x1 = static_cast<int>(std::round(
            left + width * static_cast<double>(index + 1) / 3.0));
        editors[static_cast<std::size_t>(index)]->setFont(editorFont);
        editors[static_cast<std::size_t>(index)]->setGeometry(
            x0 + gap, y, std::max(20, x1 - x0 - 2 * gap), height);
        editors[static_cast<std::size_t>(index)]->show();
        editors[static_cast<std::size_t>(index)]->raise();
    }
}

void DocumentCanvas::setMarginsPoints(double top, double right, double bottom, double left) {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    const double newTop = std::clamp(top, 0.0, pageHeightPoints_ / 2.0);
    const double newRight = std::clamp(right, 0.0, pageWidthPoints_ / 2.0);
    const double newBottom = std::clamp(bottom, 0.0, pageHeightPoints_ / 2.0);
    const double newLeft = std::clamp(left, 0.0, pageWidthPoints_ / 2.0);
    if (newTop == marginTopPoints_ && newRight == marginRightPoints_ &&
        newBottom == marginBottomPoints_ && newLeft == marginLeftPoints_) {
        return;
    }
    const auto before = captureEditorState();
    marginTopPoints_ = newTop;
    marginRightPoints_ = newRight;
    marginBottomPoints_ = newBottom;
    marginLeftPoints_ = newLeft;
    recordLayoutChange(before);
}

void DocumentCanvas::setPageSizePoints(double width, double height) {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    const double newWidth = std::max(72.0, width);
    const double newHeight = std::max(72.0, height);
    const double newTop = std::clamp(marginTopPoints_, 0.0, newHeight / 2.0);
    const double newRight = std::clamp(marginRightPoints_, 0.0, newWidth / 2.0);
    const double newBottom = std::clamp(marginBottomPoints_, 0.0, newHeight / 2.0);
    const double newLeft = std::clamp(marginLeftPoints_, 0.0, newWidth / 2.0);
    if (newWidth == pageWidthPoints_ && newHeight == pageHeightPoints_ &&
        newTop == marginTopPoints_ && newRight == marginRightPoints_ &&
        newBottom == marginBottomPoints_ && newLeft == marginLeftPoints_) {
        return;
    }
    const auto before = captureEditorState();
    pageWidthPoints_ = newWidth;
    pageHeightPoints_ = newHeight;
    marginTopPoints_ = newTop;
    marginRightPoints_ = newRight;
    marginBottomPoints_ = newBottom;
    marginLeftPoints_ = newLeft;
    recordLayoutChange(before);
}

void DocumentCanvas::setImportedPageLayout(double width, double height,
                                           double top, double right,
                                           double bottom, double left) {
    endTypingGroup();
    resetVerticalNavigation();
    pageWidthPoints_ = std::max(72.0, width);
    pageHeightPoints_ = std::max(72.0, height);
    marginTopPoints_ = std::clamp(top, 0.0, pageHeightPoints_ / 2.0);
    marginRightPoints_ = std::clamp(right, 0.0, pageWidthPoints_ / 2.0);
    marginBottomPoints_ = std::clamp(bottom, 0.0, pageHeightPoints_ / 2.0);
    marginLeftPoints_ = std::clamp(left, 0.0, pageWidthPoints_ / 2.0);
    invalidateLayout();
    viewport()->update();
}

void DocumentCanvas::toggleOrientation() {
    if (rejectLiveEditDuringPreview()) {
        return;
    }
    endTypingGroup();
    resetVerticalNavigation();
    if (pageWidthPoints_ == pageHeightPoints_) return;
    const auto before = captureEditorState();
    std::swap(pageWidthPoints_, pageHeightPoints_);
    marginTopPoints_ = std::clamp(marginTopPoints_, 0.0, pageHeightPoints_ / 2.0);
    marginRightPoints_ = std::clamp(marginRightPoints_, 0.0, pageWidthPoints_ / 2.0);
    marginBottomPoints_ = std::clamp(marginBottomPoints_, 0.0, pageHeightPoints_ / 2.0);
    marginLeftPoints_ = std::clamp(marginLeftPoints_, 0.0, pageWidthPoints_ / 2.0);
    recordLayoutChange(before);
}

std::vector<DocumentSearchMatch> DocumentCanvas::searchMatches(
    const QString& needle, DocumentSearchOptions options) const {
    std::vector<DocumentSearchMatch> matches;
    if (needle.isEmpty()) return matches;
    const auto snap = visibleDocumentSnapshot();
    std::size_t paragraphOrdinal = 0;
    std::size_t tableOrdinal = 0;
    std::size_t unitOrdinal = 0;
    for (const auto& block : snap.document.bodyBlocks()) {
        if (block.kind == core::BodyBlockKind::paragraph) {
            ++paragraphOrdinal;
            const auto* paragraph = snap.document.findParagraph(block.id);
            if (!paragraph) continue;
            const QString containerText = fromUtf16(paragraph->text());
            for (const auto& range : searchMatchRanges(
                     containerText, needle, options)) {
                matches.push_back(DocumentSearchMatch{
                    DocumentSearchHit{
                        snap.revision,
                        previewId_,
                        BodyParagraphSearchHit{
                            paragraph->id(), range.start, range.end}},
                    containerText,
                    paragraphOrdinal,
                    unitOrdinal});
            }
            ++unitOrdinal;
            continue;
        }

        ++tableOrdinal;
        const auto* table = snap.document.findTable(block.id);
        if (!table) continue;
        for (std::size_t row = 0; row < table->rowCount(); ++row) {
            for (std::size_t column = 0; column < table->columnCount();
                 ++column) {
                const auto* cell = table->cell(row, column);
                if (!cell) continue;
                const QString containerText = fromUtf16(cell->text);
                for (const auto& range : searchMatchRanges(
                         containerText, needle, options)) {
                    matches.push_back(DocumentSearchMatch{
                        DocumentSearchHit{
                            snap.revision,
                            previewId_,
                            TableCellSearchHit{
                                table->id(), cell->id, row, column,
                                range.start, range.end}},
                        containerText,
                        tableOrdinal,
                        unitOrdinal});
                }
                ++unitOrdinal;
            }
        }
    }
    return matches;
}

std::vector<DocumentSearchHit> DocumentCanvas::searchHits(
    const QString& needle, DocumentSearchOptions options) const {
    auto matches = searchMatches(needle, options);
    std::vector<DocumentSearchHit> hits;
    hits.reserve(matches.size());
    for (auto& match : matches) {
        hits.push_back(std::move(match.hit));
    }
    return hits;
}

std::optional<QString> DocumentCanvas::searchHitContainerText(
    const DocumentSearchHit& hit) const {
    const auto snap = visibleDocumentSnapshot();
    if (hit.revision != snap.revision || hit.previewId != previewId_) {
        return std::nullopt;
    }
    return std::visit(
        [&snap](const auto& target) -> std::optional<QString> {
            using Target = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<Target,
                                         BodyParagraphSearchHit>) {
                const auto* paragraph = snap.document.findParagraph(
                    target.paragraphId);
                if (!paragraph || target.startUtf16 >= target.endUtf16 ||
                    target.endUtf16 > paragraph->text().size() ||
                    !core::isUtf16Boundary(paragraph->text(),
                                           target.startUtf16) ||
                    !core::isUtf16Boundary(paragraph->text(),
                                           target.endUtf16)) {
                    return std::nullopt;
                }
                return fromUtf16(paragraph->text());
            } else {
                const auto* table = snap.document.findTable(target.tableId);
                const auto* cell = table
                    ? table->cell(target.row, target.column)
                    : nullptr;
                if (!cell || cell->id != target.cellId ||
                    target.startUtf16 >= target.endUtf16 ||
                    target.endUtf16 > cell->text.size() ||
                    !core::isUtf16Boundary(cell->text,
                                           target.startUtf16) ||
                    !core::isUtf16Boundary(cell->text,
                                           target.endUtf16)) {
                    return std::nullopt;
                }
                return fromUtf16(cell->text);
            }
        }, hit.target);
}

std::optional<DocumentSearchHit> DocumentCanvas::currentSearchHit() const {
    const auto snap = visibleDocumentSnapshot();
    if (selectedTable_ && tableCursor_ && tableSelectionAnchor_) {
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        if (!cell) return std::nullopt;
        const auto anchor = std::min(*tableSelectionAnchor_, cell->text.size());
        const auto focus = std::min(tableCursor_->utf16Offset,
                                    cell->text.size());
        if (anchor == focus) return std::nullopt;
        return DocumentSearchHit{
            snap.revision,
            previewId_,
            TableCellSearchHit{
                table->id(), cell->id, tableCursor_->row,
                tableCursor_->column, std::min(anchor, focus),
                std::max(anchor, focus)}};
    }
    if (selectedTable_) return std::nullopt;
    const auto normalized = snap.document.normalizeRange(selection_);
    if (!normalized || normalized.value().empty() ||
        normalized.value().start.paragraph_id !=
            normalized.value().end.paragraph_id) {
        return std::nullopt;
    }
    return DocumentSearchHit{
        snap.revision,
        previewId_,
        BodyParagraphSearchHit{
            normalized.value().start.paragraph_id,
            normalized.value().start.utf16_offset,
            normalized.value().end.utf16_offset}};
}

bool DocumentCanvas::activateSearchHit(const DocumentSearchHit& hit) {
    const auto snap = visibleDocumentSnapshot();
    if (hit.revision != snap.revision || hit.previewId != previewId_) {
        emit operationFailed(tr("The search result is stale. Search again."));
        return false;
    }
    if (!searchHitContainerText(hit)) {
        emit operationFailed(tr("The search result is no longer valid."));
        return false;
    }

    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    if (const auto* paragraph =
            std::get_if<BodyParagraphSearchHit>(&hit.target)) {
        selection_ = {
            {paragraph->paragraphId, paragraph->startUtf16},
            {paragraph->paragraphId, paragraph->endUtf16}};
        tableCursor_.reset();
        tableSelectionAnchor_.reset();
        tableCellSelection_.reset();
        tableMouseSelectionAnchor_.reset();
        selectedTable_.reset();
    } else {
        const auto& cell = std::get<TableCellSearchHit>(hit.target);
        tableCursor_ = TableCursor{
            cell.tableId, cell.row, cell.column, cell.endUtf16};
        tableSelectionAnchor_ = cell.startUtf16;
        tableCellSelection_.reset();
        tableMouseSelectionAnchor_.reset();
        selectedTable_ = cell.tableId;
    }
    // A preview is read-only and may have shifted ranges relative to the live
    // document. Preserve the live insertion format until that branch is
    // accepted or discarded instead of resolving it against mismatched text.
    if (!previewId_) {
        typingFormat_ = selectedCharacterFormat();
        typingOverrideMask_ = selectedCharacterOverrideMask();
    }
    emit selectionChanged();
    if (!previewId_) {
        emitCursorFormat();
    }
    updateStatus();
    viewport()->update();
    revealCursor();
    updateMicroFocus();
    return true;
}

std::optional<DocumentSearchHit> DocumentCanvas::findNextHit(
    const QString& needle, DocumentSearchOptions options) {
    if (needle.isEmpty()) return std::nullopt;
    const auto matches = searchMatches(needle, options);
    if (matches.empty()) return std::nullopt;

    if (const auto active = currentSearchHit()) {
        const auto found = std::find_if(
            matches.begin(), matches.end(), [&active](const auto& match) {
                return match.hit == *active;
            });
        if (found != matches.end()) {
            const auto next = std::next(found) == matches.end()
                ? matches.begin()
                : std::next(found);
            if (activateSearchHit(next->hit)) return next->hit;
            return std::nullopt;
        }
    }

    const auto snap = visibleDocumentSnapshot();
    std::optional<std::variant<BodyParagraphSearchHit, TableCellSearchHit>>
        currentTarget;
    std::size_t currentOffset = 0;
    if (tableCursor_) {
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        if (cell) {
            currentOffset = std::min(tableCursor_->utf16Offset,
                                     cell->text.size());
            currentTarget = TableCellSearchHit{
                table->id(), cell->id, tableCursor_->row,
                tableCursor_->column, currentOffset, currentOffset};
        }
    } else if (tableCellSelection_ && selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        const auto* cell = table
            ? table->cell(tableCellSelection_->focusRow,
                          tableCellSelection_->focusColumn)
            : nullptr;
        if (cell) {
            currentOffset = cell->text.size();
            currentTarget = TableCellSearchHit{
                table->id(), cell->id, tableCellSelection_->focusRow,
                tableCellSelection_->focusColumn, currentOffset,
                currentOffset};
        }
    } else if (selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        if (table && table->rowCount() > 0 && table->columnCount() > 0) {
            const auto row = table->rowCount() - 1;
            const auto column = table->columnCount() - 1;
            const auto* cell = table->cell(row, column);
            if (cell) {
                currentOffset = cell->text.size();
                currentTarget = TableCellSearchHit{
                    table->id(), cell->id, row, column, currentOffset,
                    currentOffset};
            }
        }
    } else {
        currentOffset = selection_.focus.utf16_offset;
        currentTarget = BodyParagraphSearchHit{
            selection_.focus.paragraph_id, currentOffset, currentOffset};
    }

    if (currentTarget) {
        const auto currentOrdinal = searchUnitOrdinal(
            snap.document, *currentTarget);
        if (currentOrdinal) {
            for (const auto& match : matches) {
                if (match.searchUnitOrdinal > *currentOrdinal ||
                    (match.searchUnitOrdinal == *currentOrdinal &&
                     searchHitStart(match.hit) >= currentOffset)) {
                    if (activateSearchHit(match.hit)) return match.hit;
                    return std::nullopt;
                }
            }
        }
    }
    if (activateSearchHit(matches.front().hit)) return matches.front().hit;
    return std::nullopt;
}

std::optional<DocumentSearchHit> DocumentCanvas::findPreviousHit(
    const QString& needle, DocumentSearchOptions options) {
    if (needle.isEmpty()) return std::nullopt;
    const auto matches = searchMatches(needle, options);
    if (matches.empty()) return std::nullopt;

    if (const auto active = currentSearchHit()) {
        const auto found = std::find_if(
            matches.begin(), matches.end(), [&active](const auto& match) {
                return match.hit == *active;
            });
        if (found != matches.end()) {
            const auto previous = found == matches.begin()
                ? std::prev(matches.end())
                : std::prev(found);
            if (activateSearchHit(previous->hit)) return previous->hit;
            return std::nullopt;
        }
    }

    const auto snap = visibleDocumentSnapshot();
    std::optional<std::variant<BodyParagraphSearchHit, TableCellSearchHit>>
        currentTarget;
    std::size_t currentOffset = 0;
    if (tableCursor_) {
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        if (cell) {
            currentOffset = std::min(tableCursor_->utf16Offset,
                                     cell->text.size());
            currentTarget = TableCellSearchHit{
                table->id(), cell->id, tableCursor_->row,
                tableCursor_->column, currentOffset, currentOffset};
        }
    } else if (tableCellSelection_ && selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        const auto* cell = table
            ? table->cell(tableCellSelection_->focusRow,
                          tableCellSelection_->focusColumn)
            : nullptr;
        if (cell) {
            currentTarget = TableCellSearchHit{
                table->id(), cell->id, tableCellSelection_->focusRow,
                tableCellSelection_->focusColumn, 0, 0};
        }
    } else if (selectedTable_) {
        const auto* table = snap.document.findTable(*selectedTable_);
        if (table && table->rowCount() > 0 && table->columnCount() > 0) {
            const auto* cell = table->cell(0, 0);
            if (cell) {
                currentTarget = TableCellSearchHit{
                    table->id(), cell->id, 0, 0, 0, 0};
            }
        }
    } else {
        currentOffset = selection_.focus.utf16_offset;
        currentTarget = BodyParagraphSearchHit{
            selection_.focus.paragraph_id, currentOffset, currentOffset};
    }

    if (currentTarget) {
        const auto currentOrdinal = searchUnitOrdinal(
            snap.document, *currentTarget);
        if (currentOrdinal) {
            for (auto iterator = matches.rbegin(); iterator != matches.rend();
                 ++iterator) {
                if (iterator->searchUnitOrdinal < *currentOrdinal ||
                    (iterator->searchUnitOrdinal == *currentOrdinal &&
                     searchHitEnd(iterator->hit) <= currentOffset)) {
                    if (activateSearchHit(iterator->hit)) {
                        return iterator->hit;
                    }
                    return std::nullopt;
                }
            }
        }
    }
    if (activateSearchHit(matches.back().hit)) return matches.back().hit;
    return std::nullopt;
}

bool DocumentCanvas::replaceSearchHit(const DocumentSearchHit& hit,
                                      const QString& replacement) {
    if (rejectLiveEditDuringPreview()) return false;
    const auto snap = session_->snapshot();
    if (hit.revision != snap.revision || hit.previewId.has_value()) {
        emit operationFailed(tr("The search result is stale. Search again."));
        return false;
    }
    const auto replacementText = toUtf16(replacement);
    if (const auto* paragraph =
            std::get_if<BodyParagraphSearchHit>(&hit.target)) {
        const auto* source = snap.document.findParagraph(
            paragraph->paragraphId);
        if (!source || paragraph->startUtf16 >= paragraph->endUtf16 ||
            paragraph->endUtf16 > source->text().size()) {
            emit operationFailed(tr("The search result is no longer valid."));
            return false;
        }
        const auto format = source->characterFormatAt(
            paragraph->startUtf16 + 1);
        const core::Position cursor{
            paragraph->paragraphId,
            paragraph->startUtf16 +
                static_cast<std::size_t>(replacement.size())};
        return apply(
            {core::ReplaceRange{
                {{paragraph->paragraphId, paragraph->startUtf16},
                 {paragraph->paragraphId, paragraph->endUtf16}},
                replacementText,
                format.empty()
                    ? std::nullopt
                    : std::optional<core::CharacterFormat>(format)}},
            cursor,
            format.empty()
                ? std::nullopt
                : std::optional<core::CharacterFormat>(format),
            false, std::nullopt, false, true);
    }

    const auto& cellHit = std::get<TableCellSearchHit>(hit.target);
    const auto* table = snap.document.findTable(cellHit.tableId);
    const auto* cell = table
        ? table->cell(cellHit.row, cellHit.column)
        : nullptr;
    if (!cell || cell->id != cellHit.cellId ||
        cellHit.startUtf16 >= cellHit.endUtf16 ||
        cellHit.endUtf16 > cell->text.size()) {
        emit operationFailed(tr("The search result is no longer valid."));
        return false;
    }
    const auto format = cell->characterFormatAt(cellHit.startUtf16 + 1);
    const TableCursor cursor{
        cellHit.tableId, cellHit.row, cellHit.column,
        cellHit.startUtf16 + static_cast<std::size_t>(replacement.size())};
    return apply(
        {core::ReplaceTableCellRange{
            cellHit.tableId, cellHit.row, cellHit.column,
            cellHit.startUtf16, cellHit.endUtf16, replacementText,
            format.empty()
                ? std::nullopt
                : std::optional<core::CharacterFormat>(format)}},
        std::nullopt,
        format.empty()
            ? std::nullopt
            : std::optional<core::CharacterFormat>(format),
        false, std::nullopt, false, true, cursor, cellHit.tableId);
}

bool DocumentCanvas::replaceCurrent(
    const QString& needle, const QString& replacement,
    DocumentSearchOptions options) {
    if (rejectLiveEditDuringPreview()) return false;
    const auto current = currentSearchHit();
    if (!current) return false;
    const auto hits = searchHits(needle, options);
    if (std::find(hits.begin(), hits.end(), *current) == hits.end()) {
        return false;
    }
    return replaceSearchHit(*current, replacement);
}

int DocumentCanvas::replaceAllMatches(
    const QString& needle, const QString& replacement,
    DocumentSearchOptions options) {
    if (rejectLiveEditDuringPreview()) return 0;
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    const auto hits = searchHits(needle, options);
    if (hits.empty()) return 0;
    const auto snap = session_->snapshot();
    const auto replacementText = toUtf16(replacement);
    std::vector<core::Operation> operations;
    operations.reserve(hits.size());

    // Reverse document order makes every UTF-16 offset remain valid while the
    // atomic batch is evaluated, including several matches in one paragraph
    // or cell.
    for (auto iterator = hits.rbegin(); iterator != hits.rend(); ++iterator) {
        if (const auto* paragraph =
                std::get_if<BodyParagraphSearchHit>(&iterator->target)) {
            const auto* source = snap.document.findParagraph(
                paragraph->paragraphId);
            if (!source) return 0;
            const auto format = source->characterFormatAt(
                paragraph->startUtf16 + 1);
            operations.push_back(core::ReplaceRange{
                {{paragraph->paragraphId, paragraph->startUtf16},
                 {paragraph->paragraphId, paragraph->endUtf16}},
                replacementText,
                format.empty()
                    ? std::nullopt
                    : std::optional<core::CharacterFormat>(format)});
            continue;
        }
        const auto& cellHit = std::get<TableCellSearchHit>(iterator->target);
        const auto* table = snap.document.findTable(cellHit.tableId);
        const auto* cell = table
            ? table->cell(cellHit.row, cellHit.column)
            : nullptr;
        if (!cell || cell->id != cellHit.cellId) return 0;
        const auto format = cell->characterFormatAt(cellHit.startUtf16 + 1);
        operations.push_back(core::ReplaceTableCellRange{
            cellHit.tableId, cellHit.row, cellHit.column,
            cellHit.startUtf16, cellHit.endUtf16, replacementText,
            format.empty()
                ? std::nullopt
                : std::optional<core::CharacterFormat>(format)});
    }

    std::variant<BodyParagraphSearchHit, TableCellSearchHit> cursorTarget =
        hits.front().target;
    std::size_t cursorOffset = searchHitStart(hits.front());
    if (tableCursor_) {
        const auto* table = snap.document.findTable(tableCursor_->tableId);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        if (cell) {
            cursorOffset = std::min(tableCursor_->utf16Offset,
                                    cell->text.size());
            cursorTarget = TableCellSearchHit{
                table->id(), cell->id, tableCursor_->row,
                tableCursor_->column, cursorOffset, cursorOffset};
        }
    } else if (!selectedTable_) {
        cursorOffset = selection_.focus.utf16_offset;
        cursorTarget = BodyParagraphSearchHit{
            selection_.focus.paragraph_id, cursorOffset, cursorOffset};
    }

    std::vector<SearchMatchRange> localMatches;
    for (const auto& hit : hits) {
        if (!sameSearchUnit(cursorTarget, hit.target)) continue;
        localMatches.push_back({
            std::visit([](const auto& value) { return value.startUtf16; },
                       hit.target),
            std::visit([](const auto& value) { return value.endUtf16; },
                       hit.target)});
    }
    cursorOffset = mapSearchOffset(
        cursorOffset, localMatches,
        static_cast<std::size_t>(replacement.size()));

    bool applied = false;
    if (const auto* paragraph =
            std::get_if<BodyParagraphSearchHit>(&cursorTarget)) {
        applied = apply(
            std::move(operations),
            core::Position{paragraph->paragraphId, cursorOffset},
            std::nullopt, false, std::nullopt, false, true);
    } else {
        const auto& cell = std::get<TableCellSearchHit>(cursorTarget);
        applied = apply(
            std::move(operations), std::nullopt, std::nullopt, false,
            std::nullopt, false, true,
            TableCursor{cell.tableId, cell.row, cell.column, cursorOffset},
            cell.tableId);
    }
    if (!applied) return 0;
    return static_cast<int>(std::min<std::size_t>(
        hits.size(), static_cast<std::size_t>(
                         std::numeric_limits<int>::max())));
}

bool DocumentCanvas::findNext(const QString& needle, bool caseSensitive) {
    return findNextHit(
               needle, DocumentSearchOptions{caseSensitive, false})
        .has_value();
}

int DocumentCanvas::replaceAll(const QString& needle,
                               const QString& replacement,
                               bool caseSensitive) {
    return replaceAllMatches(
        needle, replacement,
        DocumentSearchOptions{caseSensitive, false});
}

bool DocumentCanvas::exportPdf(const QString& path, QString& error) {
    error.clear();
    if (previewId_) {
        error = tr("Accept or discard the Codex preview before exporting PDF.");
        return false;
    }
    ensureLayout();
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        error = tr("Could not create the PDF output: %1").arg(
            output.errorString());
        return false;
    }

    {
        QPdfWriter writer(&output);
        writer.setTitle(tr("Document"));
        writer.setCreator(QStringLiteral("Owl Docs"));
        writer.setResolution(72);
        writer.setPageSize(QPageSize(
            QSizeF(pageWidthPoints_, pageHeightPoints_), QPageSize::Point,
            QString(), QPageSize::ExactMatch));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
        QPainter painter(&writer);
        if (!painter.isActive()) {
            error = tr("Could not create the PDF output.");
            return false;
        }
        for (int page = 0; page < pageCount_; ++page) {
            if (page > 0 && !writer.newPage()) {
                painter.end();
                error = tr("Could not create PDF page %1.").arg(page + 1);
                return false;
            }
            renderPage(painter, page, QPointF(), 1.0, false);
        }
        if (!painter.end()) {
            error = tr("Could not finish the PDF output.");
            return false;
        }
    }

    if (!output.commit()) {
        error = tr("Could not replace the PDF output atomically: %1").arg(
            output.errorString());
        return false;
    }
    return true;
}

bool DocumentCanvas::configurePrinter(QPrinter& printer, QString& error) const {
    error.clear();
    const bool landscape = pageWidthPoints_ > pageHeightPoints_;
    const QSizeF portraitSize(std::min(pageWidthPoints_, pageHeightPoints_),
                              std::max(pageWidthPoints_, pageHeightPoints_));
    const QPageSize pageSize(portraitSize, QPageSize::Point, QString(),
                             QPageSize::ExactMatch);
    QPageLayout layout(
        pageSize,
        landscape ? QPageLayout::Landscape : QPageLayout::Portrait,
        QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    layout.setMode(QPageLayout::FullPageMode);
    if (!layout.isValid() || !printer.setPageLayout(layout)) {
        error = tr("The selected printer cannot use the document page size.");
        return false;
    }
    printer.setFullPage(true);

    const QRectF actual = printer.pageLayout().fullRect(QPageLayout::Point);
    constexpr double tolerancePoints = 0.75;
    if (std::abs(actual.width() - pageWidthPoints_) > tolerancePoints ||
        std::abs(actual.height() - pageHeightPoints_) > tolerancePoints) {
        error = tr("The selected printer changed the document page size.");
        return false;
    }
    return true;
}

bool DocumentCanvas::printTo(QPrinter& printer, QString& error) {
    error.clear();
    if (previewId_) {
        error = tr("Accept or discard the Codex preview before printing.");
        return false;
    }
    ensureLayout();
    std::vector<int> pages;
    switch (printer.printRange()) {
        case QPrinter::AllPages:
            pages.reserve(static_cast<std::size_t>(pageCount_));
            for (int page = 0; page < pageCount_; ++page) {
                pages.push_back(page);
            }
            break;
        case QPrinter::CurrentPage:
            pages.push_back(currentPageNumber() - 1);
            break;
        case QPrinter::PageRange: {
            const auto ranges = printer.pageRanges().toRangeList();
            if (!ranges.isEmpty()) {
                for (const auto& range : ranges) {
                    const int first = std::max(1, range.from);
                    const int last = std::min(pageCount_, range.to);
                    for (int page = first; page <= last; ++page) {
                        pages.push_back(page - 1);
                    }
                }
            } else {
                const int requestedFirst = printer.fromPage();
                const int requestedLast = printer.toPage();
                if (requestedFirst >= 1 && requestedLast >= requestedFirst &&
                    requestedFirst <= pageCount_) {
                    const int last = std::min(requestedLast, pageCount_);
                    pages.reserve(static_cast<std::size_t>(
                        last - requestedFirst + 1));
                    for (int page = requestedFirst; page <= last; ++page) {
                        pages.push_back(page - 1);
                    }
                }
            }
            break;
        }
        case QPrinter::Selection:
            error = tr("Printing only the current selection is not supported yet.");
            return false;
    }

    if (pages.empty()) {
        error = tr("The requested print range is outside this document.");
        return false;
    }
    if (printer.pageOrder() == QPrinter::LastPageFirst) {
        std::reverse(pages.begin(), pages.end());
    }

    const int requestedCopies = std::max(1, printer.copyCount());
    if (requestedCopies > 1 && !printer.supportsMultipleCopies()) {
        const auto oneCopy = pages;
        pages.clear();
        pages.reserve(oneCopy.size() * static_cast<std::size_t>(requestedCopies));
        if (printer.collateCopies()) {
            for (int copy = 0; copy < requestedCopies; ++copy) {
                pages.insert(pages.end(), oneCopy.begin(), oneCopy.end());
            }
        } else {
            for (const int page : oneCopy) {
                pages.insert(pages.end(), static_cast<std::size_t>(requestedCopies), page);
            }
        }
        // The page sequence above now owns copy expansion; do not also ask the
        // backend to duplicate it.
        printer.setCopyCount(1);
    }

    if (!configurePrinter(printer, error)) {
        return false;
    }
    QPainter painter(&printer);
    if (!painter.isActive()) {
        error = tr("Could not start the print job.");
        return false;
    }
    const double scale = static_cast<double>(printer.logicalDpiX()) / 72.0;
    for (std::size_t index = 0; index < pages.size(); ++index) {
        if (index > 0 && !printer.newPage()) {
            painter.end();
            error = tr("The printer rejected a new page.");
            return false;
        }
        renderPage(painter, pages[index], QPointF(), scale, false);
        if (printer.printerState() == QPrinter::Aborted) {
            painter.end();
            error = tr("The print job was canceled.");
            return false;
        }
        if (printer.printerState() == QPrinter::Error) {
            painter.end();
            error = tr("The printer reported an error.");
            return false;
        }
    }
    if (!painter.end() || printer.printerState() == QPrinter::Error) {
        error = tr("The print job could not be completed.");
        return false;
    }
    return true;
}

bool DocumentCanvas::createReplacementPreview(const QString& replacement,
                                              QString& summary,
                                              QString& error) {
    if (previewId_) {
        discardPreview();
    }
    const auto live = session_->snapshot();
    QString safeReplacement = replacement;
    safeReplacement.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    safeReplacement.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    safeReplacement.replace(QLatin1Char('\n'), QChar::LineSeparator);
    const auto inserted = toUtf16(safeReplacement);
    const auto normalized = live.document.normalizeRange(selection_);
    if (!normalized) {
        error = errorText(normalized.error());
        return false;
    }
    const auto before = selectedText();
    const QString label = tr("Replace %1 characters with %2 characters.\n\nBefore: %3\n\nAfter: %4")
                              .arg(before.size())
                              .arg(safeReplacement.size())
                              .arg(before.left(240), safeReplacement.left(240));
    if (normalized.value().empty() && inserted.empty()) {
        const std::vector<core::Operation> noOp{
            core::InsertText{
                normalized.value().start, {},
                insertionFormatFor(live.document, normalized.value(),
                                   typingFormat_)}};
        return createOperationsPreview(
            live.revision, noOp, label, summary, error);
    }
    auto formattingPlan = planReplacementFormatting(
        live.document, normalized.value(), typingFormat_,
        typingOverrideMask_, defaultFontFamily_, defaultFontPointSize_);
    std::vector<core::Operation> operations =
        std::move(formattingPlan.operations);
    core::Position cursor = normalized.value().start;
    const auto directTypingDelta = maskedCharacterFormatDelta(
        formattingPlan.effective_typing_format,
        formattingPlan.effective_typing_override_mask);
    std::vector<core::NodeId> emptyTypingCandidates;
    appendInitialReplacementOperations(
        operations, normalized.value(), selection_, inserted,
        formattingPlan.insertion_format, directTypingDelta, cursor,
        emptyTypingCandidates);

    // Replacing a list marker has the same semantic effect in a reviewed
    // chat edit as it does during direct typing or paste: the surviving text
    // is no longer attached to list metadata.
    const auto* startParagraph = live.document.findParagraph(
        normalized.value().start.paragraph_id);
    const auto marker = startParagraph
        ? plainTextListMarker(fromUtf16(startParagraph->text()))
        : std::nullopt;
    if (startParagraph && startParagraph->format().list_id && marker &&
        normalized.value().start.utf16_offset <
            static_cast<std::size_t>(marker->prefixLength)) {
        operations.emplace_back(core::SetParagraphFormat{
            {startParagraph->id()}, clearSemanticListDelta()});
    }

    if (!appendEmptyTypingOverrideOperations(
            live.document, operations, std::move(emptyTypingCandidates),
            directTypingDelta, error)) {
        return false;
    }
    if (!createOperationsPreview(live.revision, operations, label, summary, error)) {
        return false;
    }
    previewCursor_ = cursor;
    return true;
}

bool DocumentCanvas::createOperationsPreview(
    core::Revision expectedRevision,
    const std::vector<core::Operation>& operations,
    const QString& label,
    QString& summary,
    QString& error) {
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    if (previewId_) {
        error = tr("A preview is already awaiting review. Accept or discard it first.");
        return false;
    }
    previewHasNonTextChanges_ = false;
    if (operations.empty()) {
        error = tr("The proposed preview contains no operations.");
        return false;
    }
    const auto live = session_->snapshot();
    if (live.revision != expectedRevision) {
        error = tr("REVISION_CONFLICT: expected %1, current %2")
                    .arg(expectedRevision.value())
                    .arg(live.revision.value());
        return false;
    }
    const auto created = session_->createPreview(expectedRevision);
    if (!created) {
        error = errorText(created.error());
        return false;
    }
    const auto applied = session_->applyPreviewBatch(
        created.value().id, created.value().revision, operations);
    if (!applied) {
        static_cast<void>(session_->discardPreview(created.value().id));
        error = errorText(applied.error());
        return false;
    }
    if (!applied.value().changed) {
        static_cast<void>(session_->discardPreview(created.value().id));
        error = tr("The proposed preview does not change the document.");
        return false;
    }
    previewId_ = created.value().id;
    previewRevision_ = applied.value().revision;
    previewOperations_ = operations;
    previewCursor_.reset();
    previewHasNonTextChanges_ = operationsHaveNonTextChanges(
        live.document, operations);
    const auto proposed = session_->previewSnapshot(*previewId_);
    std::size_t changedParagraphs = 0;
    if (proposed) {
        const auto& before = live.document.paragraphs();
        const auto& after = proposed.value().document.paragraphs();
        const auto common = std::min(before.size(), after.size());
        for (std::size_t index = 0; index < common; ++index) {
            if (before[index] != after[index]) {
                ++changedParagraphs;
            }
        }
        changedParagraphs += before.size() > common ? before.size() - common
                                                    : after.size() - common;
    }
    summary = tr("%1\n\n%2 operation(s), %3 paragraph(s) affected. "
                 "The blue page is an isolated preview; the live document is unchanged.")
                  .arg(label)
                  .arg(operations.size())
                  .arg(changedParagraphs);
    invalidateLayout();
    viewport()->update();
    emit previewStateChanged(true);
    return true;
}

bool DocumentCanvas::acceptPreview(QString& error) {
    endTypingGroup();
    clearPendingSpellingWord();
    if (!previewId_) {
        error = tr("There is no active preview.");
        return false;
    }
    const auto live = session_->snapshot();
    const CursorState before = captureEditorState();
    const auto accepted = session_->acceptPreview(*previewId_, live.revision, previewRevision_);
    if (!accepted) {
        error = errorText(accepted.error());
        return false;
    }
    const bool acceptedNonTextChanges = previewHasNonTextChanges_;
    resetVerticalNavigation();
    if (previewCursor_) {
        selection_ = {*previewCursor_, *previewCursor_};
    }
    const auto acceptedDocument = session_->snapshot();
    const auto clampPosition = [&acceptedDocument](core::Position position)
        -> std::optional<core::Position> {
        const auto* paragraph = acceptedDocument.document.findParagraph(
            position.paragraph_id);
        if (!paragraph) return std::nullopt;
        position.utf16_offset = std::min(position.utf16_offset,
                                         paragraph->text().size());
        while (position.utf16_offset > 0 &&
               !core::isUtf16Boundary(paragraph->text(),
                                      position.utf16_offset)) {
            --position.utf16_offset;
        }
        return position;
    };
    const auto anchor = clampPosition(selection_.anchor);
    const auto focus = clampPosition(selection_.focus);
    if (anchor && focus) {
        selection_ = {*anchor, *focus};
    } else {
        const auto& first = acceptedDocument.document.paragraphs().front();
        selection_ = {{first.id(), 0}, {first.id(), 0}};
    }
    typingFormat_ = selection_.anchor == selection_.focus
        ? currentCharacterFormat()
        : selectedCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    previewId_.reset();
    previewRevision_ = {};
    previewCursor_.reset();
    previewOperations_.clear();
    previewHasNonTextChanges_ = false;
    if (accepted.value().changed) {
        reconcileDecodedImageCache();
        currentStateId_ = nextStateId_++;
        if (acceptedNonTextChanges) {
            currentNonTextStateId_ = nextNonTextStateId_++;
        }
        updateDirtyFlags();
        undoCursorHistory_.push_back(
            CursorHistoryEntry{before, captureEditorState(), true});
        redoCursorHistory_.clear();
        synchronizeCursorHistory();
        invalidateLayout();
        emit documentChanged(accepted.value().revision.value());
    }
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    emit previewStateChanged(false);
    return true;
}

void DocumentCanvas::discardPreview() {
    endTypingGroup();
    resetVerticalNavigation();
    clearPendingSpellingWord();
    const bool hadPreview = previewId_.has_value();
    std::optional<core::NodeId> visibleCellId;
    if (hadPreview && selectedTable_ && tableCursor_) {
        const auto visible = visibleDocumentSnapshot();
        const auto* table = visible.document.findTable(*selectedTable_);
        const auto* cell = table
            ? table->cell(tableCursor_->row, tableCursor_->column)
            : nullptr;
        if (cell) visibleCellId = cell->id;
    }
    if (previewId_) {
        static_cast<void>(session_->discardPreview(*previewId_));
        previewId_.reset();
        reconcileDecodedImageCache();
        invalidateLayout();
        viewport()->update();
    }
    previewRevision_ = {};
    previewCursor_.reset();
    previewOperations_.clear();
    previewHasNonTextChanges_ = false;
    if (!hadPreview) return;

    const auto live = session_->snapshot();
    bool hasSafeTableSelection = false;
    if (selectedTable_) {
        const auto* table = live.document.findTable(*selectedTable_);
        if (table && tableCursor_ && visibleCellId) {
            for (std::size_t row = 0;
                 row < table->rowCount() && !hasSafeTableSelection; ++row) {
                for (std::size_t column = 0;
                     column < table->columnCount(); ++column) {
                    const auto* cell = table->cell(row, column);
                    if (!cell || cell->id != *visibleCellId) continue;
                    const auto clampOffset = [&cell](std::size_t offset) {
                        offset = std::min(offset, cell->text.size());
                        while (offset > 0 &&
                               !core::isUtf16Boundary(cell->text, offset)) {
                            --offset;
                        }
                        return offset;
                    };
                    tableCursor_->row = row;
                    tableCursor_->column = column;
                    tableCursor_->utf16Offset = clampOffset(
                        tableCursor_->utf16Offset);
                    if (tableSelectionAnchor_) {
                        tableSelectionAnchor_ = clampOffset(
                            *tableSelectionAnchor_);
                    }
                    hasSafeTableSelection = true;
                    break;
                }
            }
        } else if (table && !tableCursor_ && !tableCellSelection_) {
            // A whole-table selection remains valid when the live table still
            // exists. Search-result activation always takes the cursor path
            // above, but preserve this pre-existing read-only selection too.
            hasSafeTableSelection = true;
        }
    }

    if (!hasSafeTableSelection) {
        tableCursor_.reset();
        tableSelectionAnchor_.reset();
        tableCellSelection_.reset();
        tableMouseSelectionAnchor_.reset();
        selectedTable_.reset();
        const auto clampPosition = [&live](core::Position position)
            -> std::optional<core::Position> {
            const auto* paragraph = live.document.findParagraph(
                position.paragraph_id);
            if (!paragraph) return std::nullopt;
            position.utf16_offset = std::min(
                position.utf16_offset, paragraph->text().size());
            while (position.utf16_offset > 0 &&
                   !core::isUtf16Boundary(paragraph->text(),
                                          position.utf16_offset)) {
                --position.utf16_offset;
            }
            return position;
        };
        const auto anchor = clampPosition(selection_.anchor);
        const auto focus = clampPosition(selection_.focus);
        if (anchor && focus) {
            selection_ = {*anchor, *focus};
        } else {
            const auto& first = live.document.paragraphs().front();
            selection_ = {{first.id(), 0}, {first.id(), 0}};
        }
    }
    typingFormat_ = selection_.anchor == selection_.focus
        ? currentCharacterFormat()
        : selectedCharacterFormat();
    typingOverrideMask_ = selectedCharacterOverrideMask();
    emit selectionChanged();
    emitCursorFormat();
    updateStatus();
    viewport()->update();
    revealCursor();
    updateMicroFocus();
    emit previewStateChanged(false);
}

int DocumentCanvas::paragraphIndex(core::NodeId id) const {
    return static_cast<int>(session_->snapshot().document.paragraphIndex(id).value_or(0));
}

QString DocumentCanvas::paragraphText(core::NodeId id) const {
    const auto snap = session_->snapshot();
    const auto* paragraph = snap.document.findParagraph(id);
    return paragraph ? fromUtf16(paragraph->text()) : QString();
}

QString DocumentCanvas::wordAt(const core::Position& position, core::Range* range) const {
    const auto text = paragraphText(position.paragraph_id);
    if (text.isEmpty()) return {};
    int start = std::min(static_cast<int>(position.utf16_offset),
                         static_cast<int>(text.size()));
    int end = start;
    const auto isWordCharacter = [&text](int index) {
        const auto character = text.at(index);
        return character.isLetterOrNumber() ||
               character == QLatin1Char('\'') || character == QChar(0x2019);
    };
    while (start > 0 && isWordCharacter(start - 1)) --start;
    while (end < text.size() && isWordCharacter(end)) ++end;
    if (range) *range = {{position.paragraph_id, static_cast<std::size_t>(start)},
                         {position.paragraph_id, static_cast<std::size_t>(end)}};
    return text.mid(start, end - start);
}

const DocumentCanvas::VisualLine* DocumentCanvas::visualLineForCaret() const {
    const VisualLine* fallback = nullptr;
    for (const auto& paragraph : visuals_) {
        if (paragraph->id != selection_.focus.paragraph_id) continue;
        const int layoutCaret = paragraph->layoutOffsetForCore(
            selection_.focus.utf16_offset);
        for (const auto& visualLine : paragraph->lines) {
            const int start = visualLine.line.textStart();
            const int end = start + visualLine.line.textLength();
            if (layoutCaret < start || layoutCaret > end) {
                continue;
            }
            if (!fallback) {
                fallback = &visualLine;
            }
            if (lineAffinity_ &&
                lineAffinity_->paragraphId == paragraph->id &&
                lineAffinity_->textStart == start) {
                return &visualLine;
            }
        }
    }
    return fallback;
}

QRectF DocumentCanvas::caretRectInContent() const {
    ensureLayout();
    const double scale = kScreenPointsScale * zoomPercent_ / 100.0;
    const double pagePixelWidth = pageWidthPoints_ * scale;
    const double documentWidth = std::max(pagePixelWidth + 2 * kCanvasPaddingPixels,
                                          static_cast<double>(viewport()->width()));
    const double left = (documentWidth - pagePixelWidth) / 2.0 - horizontalScrollBar()->value();
    if (tableCursor_) {
        for (const auto& tableVisual : tableVisuals_) {
            if (tableVisual->id != tableCursor_->tableId) continue;
            const auto found = std::find_if(
                tableVisual->cells.begin(), tableVisual->cells.end(),
                [this](const TableCellVisual& cell) {
                    return cell.row == tableCursor_->row &&
                           cell.column == tableCursor_->column;
                });
            if (found == tableVisual->cells.end()) return {};
            qreal x = found->rect.left() + 5.0;
            qreal y = found->rect.top() + 4.0;
            qreal height = 14.0;
            const QTextLine* caretLine = nullptr;
            for (const auto& line : found->lines) {
                const auto start = static_cast<std::size_t>(line.textStart());
                const auto end = start + static_cast<std::size_t>(line.textLength());
                if (tableCursor_->utf16Offset >= start &&
                    tableCursor_->utf16Offset <= end) {
                    caretLine = &line;
                    break;
                }
            }
            if (!caretLine && !found->lines.empty()) {
                caretLine = &found->lines.back();
            }
            if (caretLine) {
                x = caretLine->cursorToX(static_cast<int>(std::min(
                    tableCursor_->utf16Offset,
                    static_cast<std::size_t>(found->text.size()))));
                y = caretLine->y();
                height = caretLine->height();
            }
            const double top = kCanvasPaddingPixels + found->pageIndex *
                (pageHeightPoints_ * scale + kPageGapPixels) -
                verticalScrollBar()->value();
            return QRectF(left + x * scale, top + y * scale, 2.0,
                          height * scale);
        }
        return {};
    }
    const auto* visualLine = visualLineForCaret();
    if (!visualLine) return {};
    int layoutOffset = static_cast<int>(selection_.focus.utf16_offset);
    const auto owner = std::find_if(
        visuals_.begin(), visuals_.end(), [this](const auto& item) {
            return item->id == selection_.focus.paragraph_id;
        });
    if (owner != visuals_.end()) {
        layoutOffset = (*owner)->layoutOffsetForCore(
            selection_.focus.utf16_offset);
    }
    const double top = kCanvasPaddingPixels + visualLine->pageIndex *
        (pageHeightPoints_ * scale + kPageGapPixels) -
        verticalScrollBar()->value();
    return QRectF(left + visualLine->line.cursorToX(
                            layoutOffset) * scale,
                  top + visualLine->line.y() * scale, 2.0,
                  visualLine->line.height() * scale);
}

void DocumentCanvas::revealCursor() {
    const auto rect = caretRectInContent();
    if (rect.top() < 0) verticalScrollBar()->setValue(verticalScrollBar()->value() + static_cast<int>(rect.top()) - 12);
    else if (rect.bottom() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + static_cast<int>(rect.bottom() - viewport()->height()) + 12);
}

void DocumentCanvas::emitCursorFormat() {
    const auto format = activeCharacterFormat();
    emit cursorParagraphStyleChanged(currentParagraphStyleId());
    emit cursorFormatChanged(format.font_family
                                 ? QString::fromStdString(*format.font_family)
                                 : defaultFontFamily_,
                             format.font_size_half_points
                                 ? *format.font_size_half_points / 2.0
                                 : defaultFontPointSize_,
                             fromArgb(format.foreground_argb.value_or(
                                 kDefaultTextArgb)));
    emit cursorHighlightChanged(
        format.highlight_argb ? fromArgb(*format.highlight_argb) : QColor{});
    emit cursorStyleChanged(
        format.bold.value_or(false), format.italic.value_or(false),
        format.underline.value_or(core::UnderlineStyle::none) !=
            core::UnderlineStyle::none,
        format.strike.value_or(false),
        format.baseline.value_or(core::BaselinePosition::normal) ==
            core::BaselinePosition::superscript,
        format.baseline.value_or(core::BaselinePosition::normal) ==
            core::BaselinePosition::subscript);
    const bool listContextActive = hasActiveList();
    const auto marker = !listContextActive
        ? std::optional<PlainTextListMarker>{}
        : plainTextListMarker(paragraphText(selection_.focus.paragraph_id));
    emit cursorListStateChanged(
        marker && marker->kind == PlainTextListMarker::Kind::bullet,
        marker && marker->kind == PlainTextListMarker::Kind::numbered);
    int oneBasedLevel = 1;
    if (marker) {
        const auto snap = session_->snapshot();
        const auto* paragraph = snap.document.findParagraph(
            selection_.focus.paragraph_id);
        oneBasedLevel = paragraph && paragraph->format().list_level
            ? static_cast<int>(*paragraph->format().list_level) + 1
            : static_cast<int>(listLevelForMarker(*marker, tabWidthSpaces_)) + 1;
    }
    emit cursorListContextChanged(listContextActive, oneBasedLevel);
}

void DocumentCanvas::updateStatus() {
    ensureLayout();
    int words = 0;
    static const QRegularExpression word(QStringLiteral("\\b[\\p{L}\\p{N}][\\p{L}\\p{N}'’-]*\\b"));
    const auto snap = session_->snapshot();
    for (const auto& paragraph : snap.document.paragraphs()) {
        auto matches = word.globalMatch(fromUtf16(paragraph.text()));
        while (matches.hasNext()) { matches.next(); ++words; }
    }
    for (const auto& table : snap.document.tables()) {
        for (const auto& cell : table.cells()) {
            auto matches = word.globalMatch(fromUtf16(cell.text));
            while (matches.hasNext()) { matches.next(); ++words; }
        }
    }
    emit pageStatusChanged(currentPageNumber(), pageCount_, words);
}

}  // namespace docxstudio::app
