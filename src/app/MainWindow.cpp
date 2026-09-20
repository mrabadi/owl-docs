#include "docxstudio/app/MainWindow.h"

#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/CodexController.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/app/FileFingerprint.h"
#include "docxstudio/app/FontFamilyPicker.h"
#include "docxstudio/app/ListPropertiesDialog.h"
#include "docxstudio/app/NavigationDock.h"
#include "docxstudio/app/OwlDocsIcon.h"
#include "docxstudio/app/RasterDecoder.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/core/document.h"
#include "docxstudio/math/latex_parser.h"
#include "docxstudio/ooxml/docx_document.h"
#include "docxstudio/worker/client.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFont>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QInputDialog>
#include <QIcon>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtPrintSupport/QPrinter>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace docxstudio::app {
namespace {

#ifndef DOCXSTUDIO_INSTALL_PARSER_WORKER_PATH
#define DOCXSTUDIO_INSTALL_PARSER_WORKER_PATH "/usr/libexec/owl-docs/owl-docs-parser-worker"
#endif

std::filesystem::path nativePath(const QString& path) {
    return std::filesystem::path(QFile::encodeName(path).constData());
}

bool sameFileDestination(const QString& first, const QString& second) {
    if (first.isEmpty() || second.isEmpty()) return false;
    std::error_code error;
    const auto firstPath = std::filesystem::absolute(nativePath(first)).lexically_normal();
    const auto secondPath = std::filesystem::absolute(nativePath(second)).lexically_normal();
    if (std::filesystem::exists(firstPath, error) && !error &&
        std::filesystem::exists(secondPath, error) && !error) {
        const bool equivalent = std::filesystem::equivalent(firstPath, secondPath, error);
        if (!error && equivalent) return true;
    }
    return firstPath == secondPath;
}

QString normalizedAbsolutePath(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isExistingRegularFile(const QString& path) {
    const QFileInfo info(path);
    return info.exists() && info.isFile();
}

bool isOpenableDocxPath(const QString& path) {
    const QFileInfo info(path);
    return isExistingRegularFile(path) &&
           info.suffix().compare(QStringLiteral("docx"),
                                 Qt::CaseInsensitive) == 0;
}

struct DroppedDocumentPaths {
    QStringList paths;
    int rejected{};
};

DroppedDocumentPaths droppedDocumentPaths(const QMimeData* mimeData) {
    DroppedDocumentPaths selection;
    if (!mimeData || !mimeData->hasUrls()) return selection;

    for (const auto& url : mimeData->urls()) {
        if (!url.isLocalFile() || !url.host().isEmpty()) {
            ++selection.rejected;
            continue;
        }
        const QString path = normalizedAbsolutePath(url.toLocalFile());
        if (!isOpenableDocxPath(path)) {
            ++selection.rejected;
            continue;
        }
        const bool alreadySelected = std::any_of(
            selection.paths.cbegin(), selection.paths.cend(),
            [&path](const QString& existing) {
                return sameFileDestination(existing, path);
            });
        if (!alreadySelected) selection.paths.push_back(path);
    }
    return selection;
}

QString escapedMenuLabel(QString label) {
    label.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return label;
}

std::filesystem::path parserWorkerPath() {
    std::error_code ignored;
    const auto application_directory = nativePath(QCoreApplication::applicationDirPath());
    const auto build_sibling = application_directory / "owl-docs-parser-worker";
    if (std::filesystem::is_regular_file(build_sibling, ignored)) {
        return std::filesystem::absolute(build_sibling);
    }
    const auto installed_sibling =
        (application_directory / ".." / "libexec" / "owl-docs" /
         "owl-docs-parser-worker").lexically_normal();
    if (std::filesystem::is_regular_file(installed_sibling, ignored)) {
        return installed_sibling;
    }
    return std::filesystem::path(DOCXSTUDIO_INSTALL_PARSER_WORKER_PATH);
}

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

struct LiteralListMarker {
    enum class Kind { bullet, numbered };

    Kind kind{Kind::bullet};
    QString indent;
    QString marker;
    QString separator;
    qsizetype prefixLength{};
};

// WordprocessingML native numbering is limited to ilvl 0 through 8. Owl Docs
// keeps its tenth editor level, but exports that level as literal marker text
// with standard hanging-indent/tab geometry.
constexpr std::size_t kNativeOoxmlListLevelCount = 9;

std::optional<LiteralListMarker> literalListMarker(const QString& text) {
    static const QRegularExpression bulletPattern(
        QStringLiteral(R"(^([ \t]*)([\x{2022}\x{25E6}\x{25AA}\x{2023}*-])([ \t]+))"));
    // Owl Docs stores its currently supported lists as literal marker text plus
    // DOCX tab/hanging-indent geometry.  Numbered levels cycle through decimal,
    // upper-alpha, upper-Roman, lower-alpha, and lower-Roman markers, so the
    // package mapper must recognize all five forms when regenerating or
    // reopening a document.
    static const QRegularExpression numberedPattern(QStringLiteral(
        R"(^([ \t]*)([0-9]+|[A-Z]{1,3}|[a-z]{1,3}|[IVXLCDM]{4,15}|[ivxlcdm]{4,15})([.)])([ \t]+))"));

    auto match = bulletPattern.match(text);
    LiteralListMarker result;
    if (match.hasMatch()) {
        result.kind = LiteralListMarker::Kind::bullet;
        result.indent = match.captured(1);
        result.marker = match.captured(2);
        result.separator = match.captured(3);
    } else {
        match = numberedPattern.match(text);
        if (!match.hasMatch()) return std::nullopt;
        result.kind = LiteralListMarker::Kind::numbered;
        result.indent = match.captured(1);
        result.marker = match.captured(2) + match.captured(3);
        result.separator = match.captured(4);
        if (match.captured(2).front().isLetter() && result.indent.isEmpty() &&
            !result.separator.contains(QLatin1Char('\t'))) {
            return std::nullopt;
        }
    }
    result.prefixLength = match.capturedLength(0);
    return result;
}

int indentationColumns(const QString& indent, int tabWidthSpaces) {
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

QFont listMetricFont(const core::CharacterFormat& format,
                     const QString& defaultFontFamily,
                     double defaultFontPointSize) {
    QFont font(QString::fromStdString(
        format.font_family.value_or(defaultFontFamily.toStdString())));
    font.setPointSizeF(
        format.font_size_half_points
            ? static_cast<double>(*format.font_size_half_points) / 2.0
            : defaultFontPointSize);
    font.setBold(format.bold.value_or(false));
    font.setItalic(format.italic.value_or(false));
    return font;
}

ooxml::BasicRunFormat nativeListMarkerFormat(
    const ooxml::Paragraph& paragraph) {
    ooxml::BasicRunFormat result =
        paragraph.runs.empty() ? ooxml::BasicRunFormat{}
                               : paragraph.runs.front().format;
    if (!paragraph.numbering) return result;
    const auto& marker = paragraph.numbering->marker_format;
    if (marker.font_family) result.font_family = marker.font_family;
    if (marker.font_size_half_points) {
        result.font_size_half_points = marker.font_size_half_points;
    }
    if (marker.bold) result.bold = marker.bold;
    if (marker.italic) result.italic = marker.italic;
    if (marker.underline) result.underline = marker.underline;
    if (marker.strike) result.strike = marker.strike;
    if (marker.foreground_rgb) result.foreground_rgb = marker.foreground_rgb;
    if (marker.highlight_rgb) result.highlight_rgb = marker.highlight_rgb;
    if (marker.baseline) result.baseline = marker.baseline;
    return result;
}

qreal nativeListSpaceAdvance(
    const ooxml::Paragraph& paragraph, const QString& defaultFontFamily,
    double defaultFontPointSize) {
    const auto format = nativeListMarkerFormat(paragraph);
    QFont font(QString::fromStdString(
        format.font_family.value_or(defaultFontFamily.toStdString())));
    font.setPointSizeF(
        format.font_size_half_points
            ? static_cast<double>(*format.font_size_half_points) / 2.0
            : defaultFontPointSize);
    font.setBold(format.bold.value_or(false));
    font.setItalic(format.italic.value_or(false));
    return std::max<qreal>(
        0.5, QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));
}

qreal listSpaceAdvance(const core::Paragraph& paragraph,
                       std::size_t contentOffset,
                       const QString& defaultFontFamily,
                       double defaultFontPointSize) {
    const QFont font = listMetricFont(
        paragraph.characterFormatAt(contentOffset), defaultFontFamily,
        defaultFontPointSize);
    return std::max<qreal>(
        0.5, QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));
}

qreal listTextAdvance(const core::Paragraph& paragraph, std::size_t start,
                      const QString& text,
                      const QString& defaultFontFamily,
                      double defaultFontPointSize) {
    qreal result = 0.0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto offset = start + static_cast<std::size_t>(index);
        const QFont font = listMetricFont(
            paragraph.characterFormatAt(offset + 1), defaultFontFamily,
            defaultFontPointSize);
        result += QFontMetricsF(font).horizontalAdvance(text[index]);
    }
    return result;
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

core::Document documentFromText(const QStringList& paragraphs) {
    std::vector<core::Paragraph> result;
    result.reserve(static_cast<std::size_t>(std::max<qsizetype>(1, paragraphs.size())));
    for (const auto& text : paragraphs) {
        auto paragraph = core::Paragraph::create(text.toStdU16String());
        if (paragraph) result.push_back(std::move(paragraph.value()));
    }
    if (result.empty()) result.push_back(core::Paragraph::create(u"").value());
    return core::Document::create(std::move(result)).value();
}

core::CharacterFormat importedCharacterFormat(
    const ooxml::BasicRunFormat& source) {
    core::CharacterFormat result;
    result.font_family = source.font_family;
    result.font_size_half_points = source.font_size_half_points;
    if (source.bold.has_value()) result.bold = *source.bold;
    if (source.italic.has_value()) result.italic = *source.italic;
    if (source.underline.has_value()) {
        result.underline = *source.underline
            ? core::UnderlineStyle::single
            : core::UnderlineStyle::none;
    }
    if (source.strike.has_value()) result.strike = *source.strike;
    if (source.foreground_rgb) {
        result.foreground_argb = 0xff000000U | *source.foreground_rgb;
    }
    if (source.highlight_rgb) {
        result.highlight_argb = 0xff000000U | *source.highlight_rgb;
    }
    if (source.baseline) {
        switch (*source.baseline) {
            case ooxml::BasicBaseline::normal:
                result.baseline = core::BaselinePosition::normal;
                break;
            case ooxml::BasicBaseline::superscript:
                result.baseline = core::BaselinePosition::superscript;
                break;
            case ooxml::BasicBaseline::subscript:
                result.baseline = core::BaselinePosition::subscript;
                break;
        }
    }
    return result;
}

void overlayCharacterFormat(core::CharacterFormat& destination,
                            const core::CharacterFormat& source) {
    const auto overlay = [](auto& target, const auto& value) {
        if (value) target = value;
    };
    overlay(destination.font_family, source.font_family);
    overlay(destination.font_size_half_points,
            source.font_size_half_points);
    overlay(destination.bold, source.bold);
    overlay(destination.italic, source.italic);
    overlay(destination.underline, source.underline);
    overlay(destination.strike, source.strike);
    overlay(destination.foreground_argb, source.foreground_argb);
    overlay(destination.highlight_argb, source.highlight_argb);
    overlay(destination.baseline, source.baseline);
    overlay(destination.language, source.language);
}

core::CharacterFormatMask importedCharacterFormatMask(
    const ooxml::BasicRunFormat& source) {
    core::CharacterFormatMask result;
    result.font_family = source.font_family.has_value();
    result.font_size_half_points = source.font_size_half_points.has_value();
    result.bold = source.bold.has_value();
    result.italic = source.italic.has_value();
    result.underline = source.underline.has_value();
    result.strike = source.strike.has_value();
    result.foreground_argb = source.foreground_rgb.has_value();
    result.highlight_argb = source.highlight_rgb.has_value();
    result.baseline = source.baseline.has_value();
    return result;
}

std::optional<core::ParagraphAlignment> importedParagraphAlignment(
    const std::optional<ooxml::BasicParagraphAlignment>& source) {
    if (!source) return std::nullopt;
    switch (*source) {
        case ooxml::BasicParagraphAlignment::left:
            return core::ParagraphAlignment::left;
        case ooxml::BasicParagraphAlignment::center:
            return core::ParagraphAlignment::center;
        case ooxml::BasicParagraphAlignment::right:
            return core::ParagraphAlignment::right;
        case ooxml::BasicParagraphAlignment::justified:
            return core::ParagraphAlignment::justified;
    }
    return std::nullopt;
}

core::ParagraphStyleProvenance importedStyleProvenance(
    const ooxml::ParagraphStyleProvenance& source) {
    constexpr std::int64_t kEmuPerTwip = 635;
    core::ParagraphStyleProvenance result;
    result.inherited_character_format =
        importedCharacterFormat(source.inherited_character_format);
    result.inherited_paragraph_mark_character_format =
        importedCharacterFormat(source.inherited_paragraph_mark_format);
    result.inherited_paragraph_format.alignment =
        importedParagraphAlignment(source.inherited_alignment);
    const auto signedTwips = [](const std::optional<std::int32_t>& value) {
        return value ? std::optional<std::int64_t>(
                           static_cast<std::int64_t>(*value) * kEmuPerTwip)
                     : std::nullopt;
    };
    const auto unsignedTwips = [](const std::optional<std::uint32_t>& value) {
        return value ? std::optional<std::int64_t>(
                           static_cast<std::int64_t>(*value) * kEmuPerTwip)
                     : std::nullopt;
    };
    auto& inherited = result.inherited_paragraph_format;
    inherited.left_indent_emu =
        signedTwips(source.inherited_left_indent_twips);
    inherited.right_indent_emu =
        signedTwips(source.inherited_right_indent_twips);
    inherited.first_line_indent_emu =
        signedTwips(source.inherited_first_line_indent_twips);
    inherited.space_before_emu =
        unsignedTwips(source.inherited_space_before_twips);
    inherited.space_after_emu =
        unsignedTwips(source.inherited_space_after_twips);
    inherited.line_spacing_emu =
        unsignedTwips(source.inherited_line_spacing);
    if (source.inherited_line_spacing_rule) {
        switch (*source.inherited_line_spacing_rule) {
            case ooxml::BasicLineSpacingRule::automatic:
                inherited.line_spacing_rule =
                    core::LineSpacingRule::automatic;
                break;
            case ooxml::BasicLineSpacingRule::at_least:
                inherited.line_spacing_rule = core::LineSpacingRule::at_least;
                break;
            case ooxml::BasicLineSpacingRule::exact:
                inherited.line_spacing_rule = core::LineSpacingRule::exact;
                break;
        }
    }
    inherited.keep_with_next = source.inherited_keep_with_next;
    inherited.keep_lines = source.inherited_keep_lines;
    inherited.page_break_before = source.inherited_page_break_before;

    auto& direct = result.paragraph_overrides;
    direct.alignment = source.direct_alignment.has_value();
    direct.left_indent_emu = source.direct_left_indent_twips.has_value();
    direct.right_indent_emu = source.direct_right_indent_twips.has_value();
    direct.first_line_indent_emu =
        source.direct_first_line_indent_twips.has_value();
    direct.space_before_emu = source.direct_space_before_twips.has_value();
    direct.space_after_emu = source.direct_space_after_twips.has_value();
    direct.line_spacing_emu = source.direct_line_spacing.has_value();
    direct.line_spacing_rule = source.direct_line_spacing_rule.has_value();
    direct.keep_with_next = source.direct_keep_with_next.has_value();
    direct.keep_lines = source.direct_keep_lines.has_value();
    direct.page_break_before = source.direct_page_break_before.has_value();
    result.paragraph_mark_overrides = importedCharacterFormatMask(
        source.direct_paragraph_mark_format);
    return result;
}

std::optional<core::TableStyle> importedTableStyle(
    const std::optional<ooxml::BasicTableStyle>& source) {
    if (!source) return std::nullopt;
    using OoxmlStyle = ooxml::BasicTableStyle;
    using CoreStyle = core::TableStyle;
    switch (*source) {
        case OoxmlStyle::plain: return CoreStyle::plain;
        case OoxmlStyle::grid: return CoreStyle::grid;
        case OoxmlStyle::light_gray: return CoreStyle::light_gray;
        case OoxmlStyle::light_blue: return CoreStyle::light_blue;
        case OoxmlStyle::light_orange: return CoreStyle::light_orange;
        case OoxmlStyle::medium_blue: return CoreStyle::medium_blue;
        case OoxmlStyle::medium_green: return CoreStyle::medium_green;
        case OoxmlStyle::medium_orange: return CoreStyle::medium_orange;
        case OoxmlStyle::aubergine: return CoreStyle::aubergine;
        case OoxmlStyle::orange_accent: return CoreStyle::orange_accent;
        case OoxmlStyle::banded_blue: return CoreStyle::banded_blue;
        case OoxmlStyle::banded_aubergine:
            return CoreStyle::banded_aubergine;
        case OoxmlStyle::dark_header: return CoreStyle::dark_header;
    }
    return std::nullopt;
}

ooxml::BasicTableStyle toOoxmlTableStyle(core::TableStyle source) {
    using OoxmlStyle = ooxml::BasicTableStyle;
    using CoreStyle = core::TableStyle;
    switch (source) {
        case CoreStyle::plain: return OoxmlStyle::plain;
        case CoreStyle::grid: return OoxmlStyle::grid;
        case CoreStyle::light_gray: return OoxmlStyle::light_gray;
        case CoreStyle::light_blue: return OoxmlStyle::light_blue;
        case CoreStyle::light_orange: return OoxmlStyle::light_orange;
        case CoreStyle::medium_blue: return OoxmlStyle::medium_blue;
        case CoreStyle::medium_green: return OoxmlStyle::medium_green;
        case CoreStyle::medium_orange: return OoxmlStyle::medium_orange;
        case CoreStyle::aubergine: return OoxmlStyle::aubergine;
        case CoreStyle::orange_accent: return OoxmlStyle::orange_accent;
        case CoreStyle::banded_blue: return OoxmlStyle::banded_blue;
        case CoreStyle::banded_aubergine:
            return OoxmlStyle::banded_aubergine;
        case CoreStyle::dark_header: return OoxmlStyle::dark_header;
    }
    return OoxmlStyle::grid;
}

core::ParagraphFormat importedTableCellParagraphFormat(
    const ooxml::Paragraph& source) {
    constexpr std::int64_t kEmuPerTwip = 635;
    core::ParagraphFormat result;
    result.alignment = importedParagraphAlignment(source.alignment);
    const auto signedTwips = [kEmuPerTwip](
                                 const std::optional<std::int32_t>& value)
        -> std::optional<std::int64_t> {
        return value ? std::optional<std::int64_t>(
                           static_cast<std::int64_t>(*value) * kEmuPerTwip)
                     : std::nullopt;
    };
    const auto unsignedTwips = [kEmuPerTwip](
                                   const std::optional<std::uint32_t>& value)
        -> std::optional<std::int64_t> {
        return value ? std::optional<std::int64_t>(
                           static_cast<std::int64_t>(*value) * kEmuPerTwip)
                     : std::nullopt;
    };
    result.left_indent_emu = signedTwips(source.left_indent_twips);
    result.right_indent_emu = signedTwips(source.right_indent_twips);
    result.first_line_indent_emu =
        signedTwips(source.first_line_indent_twips);
    result.space_before_emu = unsignedTwips(source.space_before_twips);
    result.space_after_emu = unsignedTwips(source.space_after_twips);
    result.line_spacing_emu = unsignedTwips(source.line_spacing);
    if (source.line_spacing_rule) {
        switch (*source.line_spacing_rule) {
            case ooxml::BasicLineSpacingRule::automatic:
                result.line_spacing_rule = core::LineSpacingRule::automatic;
                break;
            case ooxml::BasicLineSpacingRule::at_least:
                result.line_spacing_rule = core::LineSpacingRule::at_least;
                break;
            case ooxml::BasicLineSpacingRule::exact:
                result.line_spacing_rule = core::LineSpacingRule::exact;
                break;
        }
    }
    result.keep_with_next = source.keep_with_next;
    result.keep_lines = source.keep_lines;
    result.page_break_before = source.page_break_before;
    return result;
}

std::optional<ImportedCellBorderPresentation> importedBorder(
    const std::optional<ooxml::ImportedTableBorder>& source) {
    if (!source || source->width_eighth_points == 0) return std::nullopt;
    return ImportedCellBorderPresentation{
        0xff000000U | source->rgb,
        static_cast<double>(source->width_eighth_points) / 8.0};
}

ImportedTableCellPresentation importedTableCellPresentation(
    const ooxml::ImportedTableCell& cell,
    const ooxml::Paragraph& paragraph) {
    ImportedTableCellPresentation result;
    result.sourceText = fromUtf8(paragraph.plainText());
    result.sourceText.replace(QStringLiteral("\r\n"),
                              QString(QChar::LineSeparator));
    result.sourceText.replace(QLatin1Char('\r'), QChar::LineSeparator);
    result.sourceText.replace(QLatin1Char('\n'), QChar::LineSeparator);
    result.alignment = importedParagraphAlignment(paragraph.alignment);
    if (paragraph.space_before_twips) {
        result.spaceBeforePoints =
            static_cast<double>(*paragraph.space_before_twips) / 20.0;
    }
    if (paragraph.space_after_twips) {
        result.spaceAfterPoints =
            static_cast<double>(*paragraph.space_after_twips) / 20.0;
    }
    result.lineSpacing = paragraph.line_spacing;
    if (paragraph.line_spacing_rule) {
        switch (*paragraph.line_spacing_rule) {
            case ooxml::BasicLineSpacingRule::automatic:
                result.lineSpacingRule = core::LineSpacingRule::automatic;
                break;
            case ooxml::BasicLineSpacingRule::at_least:
                result.lineSpacingRule = core::LineSpacingRule::at_least;
                break;
            case ooxml::BasicLineSpacingRule::exact:
                result.lineSpacingRule = core::LineSpacingRule::exact;
                break;
        }
    }
    if (cell.vertical_alignment) {
        switch (*cell.vertical_alignment) {
            case ooxml::BasicVerticalAlignment::top:
                result.verticalAlignment = ImportedCellVerticalAlignment::top;
                break;
            case ooxml::BasicVerticalAlignment::center:
                result.verticalAlignment = ImportedCellVerticalAlignment::center;
                break;
            case ooxml::BasicVerticalAlignment::bottom:
                result.verticalAlignment = ImportedCellVerticalAlignment::bottom;
                break;
        }
    }
    const auto points = [](const std::optional<std::uint32_t>& twips,
                           double fallback) {
        return twips ? static_cast<double>(*twips) / 20.0 : fallback;
    };
    result.paddingTopPoints = points(cell.margin_top_twips, 4.0);
    result.paddingRightPoints = points(cell.margin_right_twips, 5.0);
    result.paddingBottomPoints = points(cell.margin_bottom_twips, 4.0);
    result.paddingLeftPoints = points(cell.margin_left_twips, 5.0);
    if (cell.fill_rgb) result.fillArgb = 0xff000000U | *cell.fill_rgb;
    result.borderTop = importedBorder(cell.border_top);
    result.borderRight = importedBorder(cell.border_right);
    result.borderBottom = importedBorder(cell.border_bottom);
    result.borderLeft = importedBorder(cell.border_left);

    std::size_t offset = 0;
    for (const auto& run : paragraph.runs) {
        const std::size_t start = offset;
        for (const auto& fragment : run.fragments) {
            switch (fragment.kind) {
                case ooxml::FragmentKind::text:
                    offset += static_cast<std::size_t>(
                        fromUtf8(fragment.text).size());
                    break;
                case ooxml::FragmentKind::tab:
                case ooxml::FragmentKind::line_break:
                    ++offset;
                    break;
                case ooxml::FragmentKind::page_break:
                case ooxml::FragmentKind::inline_image:
                    break;
                case ooxml::FragmentKind::equation:
                    ++offset;
                    break;
            }
        }
        if (offset > start) {
            const auto format = importedCharacterFormat(run.format);
            // Retain even an empty direct format as an explicit coverage run.
            // Otherwise the first styled run can incorrectly become the
            // QTextLayout base and leak its font metrics into unformatted
            // text later in the same cell.
            result.formats.push_back({start, offset, format});
        }
    }
    return result;
}

core::Result<core::Document> documentFromOoxmlParagraphs(
    const std::vector<ooxml::Paragraph>& sourceParagraphs,
    const QString& defaultFontFamily, double defaultFontPointSize,
    int tabWidthSpaces,
    QStringList* sourceTextPrefixes = nullptr) {
    struct RunStyle {
        std::size_t paragraphIndex{};
        std::size_t start{};
        std::size_t end{};
        ooxml::BasicRunFormat format;
        ooxml::BasicRunFormat paragraphStyleOverrides;
    };
    struct PendingEquation {
        std::size_t paragraphIndex{};
        std::size_t utf16Offset{};
        ooxml::EquationPayload payload;
    };
    std::vector<core::Paragraph> paragraphs;
    std::vector<RunStyle> styles;
    std::vector<std::optional<core::ParagraphStyleProvenance>>
        styleProvenance(sourceParagraphs.size());
    std::vector<PendingEquation> equations;
    if (sourceTextPrefixes) sourceTextPrefixes->clear();
    paragraphs.reserve(std::max<std::size_t>(1, sourceParagraphs.size()));
    for (std::size_t paragraphIndex = 0;
         paragraphIndex < sourceParagraphs.size(); ++paragraphIndex) {
        const auto& source = sourceParagraphs[paragraphIndex];
        if (source.style_id && source.style_provenance) {
            styleProvenance[paragraphIndex] =
                importedStyleProvenance(*source.style_provenance);
        }
        QString text;
        QString semanticPrefix;
        if (source.numbering && !source.numbering->marker_text.empty()) {
            auto indentation =
                static_cast<qsizetype>(source.numbering->level) *
                static_cast<qsizetype>(std::max(1, tabWidthSpaces));
            if (source.left_indent_twips &&
                source.first_line_indent_twips &&
                *source.first_line_indent_twips < 0) {
                const auto bulletPosition =
                    static_cast<std::int64_t>(*source.left_indent_twips) +
                    *source.first_line_indent_twips;
                const qreal spaceAdvance = nativeListSpaceAdvance(
                    source, defaultFontFamily, defaultFontPointSize);
                const auto estimated = static_cast<int>(std::llround(
                    static_cast<qreal>(bulletPosition) /
                    (20.0 * spaceAdvance)));
                if (estimated >= 0 &&
                    estimated <= core::kMaximumListIndentSpaces) {
                    indentation = estimated;
                }
            }
            semanticPrefix = QString(indentation, QLatin1Char(' ')) +
                             fromUtf8(source.numbering->marker_text) +
                             QLatin1Char('\t');
            text = semanticPrefix;
        }
        if (sourceTextPrefixes) sourceTextPrefixes->push_back(semanticPrefix);
        std::size_t finalOffset =
            static_cast<std::size_t>(semanticPrefix.size());
        if (!semanticPrefix.isEmpty()) {
            auto markerFormat = nativeListMarkerFormat(source);
            styles.push_back(
                {paragraphIndex, 0, finalOffset, std::move(markerFormat), {}});
        }
        for (const auto& run : source.runs) {
            const std::size_t start = finalOffset;
            for (const auto& fragment : run.fragments) {
                if (fragment.kind == ooxml::FragmentKind::equation) {
                    if (fragment.equation) {
                        equations.push_back(
                            {paragraphIndex, finalOffset, *fragment.equation});
                        ++finalOffset;
                    }
                    continue;
                }
                QString fragmentText;
                switch (fragment.kind) {
                    case ooxml::FragmentKind::text:
                        fragmentText = fromUtf8(fragment.text);
                        break;
                    case ooxml::FragmentKind::tab:
                        fragmentText = QString(QLatin1Char('\t'));
                        break;
                    case ooxml::FragmentKind::line_break:
                        fragmentText = QString(QChar::LineSeparator);
                        break;
                    case ooxml::FragmentKind::page_break:
                    case ooxml::FragmentKind::inline_image:
                        break;
                    case ooxml::FragmentKind::equation:
                        break;
                }
                fragmentText.replace(QStringLiteral("\r\n"),
                                     QStringLiteral("\n"));
                fragmentText.replace(QLatin1Char('\r'), QLatin1Char('\n'));
                fragmentText.replace(QLatin1Char('\n'), QChar::LineSeparator);
                finalOffset += static_cast<std::size_t>(fragmentText.size());
                text += fragmentText;
            }
            const std::size_t end = finalOffset;
            if (end > start) {
                styles.push_back(
                    {paragraphIndex, start, end, run.format,
                     run.paragraph_style_overrides});
                if (styleProvenance[paragraphIndex]) {
                    const auto mask = importedCharacterFormatMask(
                        run.paragraph_style_overrides);
                    if (!mask.empty()) {
                        styleProvenance[paragraphIndex]
                            ->character_overrides.push_back(
                                {start, end, mask});
                    }
                }
            }
        }
        core::CharacterFormat paragraphMarkFormat;
        if (styleProvenance[paragraphIndex] && source.style_provenance) {
            paragraphMarkFormat = styleProvenance[paragraphIndex]
                                      ->inherited_paragraph_mark_character_format;
            overlayCharacterFormat(
                paragraphMarkFormat,
                importedCharacterFormat(
                    source.style_provenance
                        ->direct_paragraph_mark_format));
        } else if (source.paragraph_mark_format) {
            paragraphMarkFormat = importedCharacterFormat(
                *source.paragraph_mark_format);
        }
        auto created = core::Paragraph::create(
            text.toStdU16String(), core::NodeId::generate(),
            std::move(paragraphMarkFormat), source.style_id);
        if (!created) return created.error();
        paragraphs.push_back(std::move(created.value()));
    }
    if (paragraphs.empty()) {
        auto created = core::Paragraph::create(u"");
        if (!created) return created.error();
        paragraphs.push_back(std::move(created.value()));
    }
    auto documentResult = core::Document::create(std::move(paragraphs));
    if (!documentResult) return documentResult.error();
    auto document = std::move(documentResult.value());
    for (const auto& equation : equations) {
        if (equation.paragraphIndex >= document.paragraphs().size()) continue;
        const auto inserted = document.insertEquation(
            {document.paragraphs()[equation.paragraphIndex].id(),
             equation.utf16Offset},
            equation.payload.canonical_latex, equation.payload.display);
        if (!inserted) return inserted.error();
    }
    for (const auto& style : styles) {
        if (style.paragraphIndex >= document.paragraphs().size()) continue;
        core::CharacterFormatDelta delta;
        if (style.format.font_family)
            delta.font_family = core::PropertyDelta<std::string>::set(
                *style.format.font_family);
        if (style.format.font_size_half_points)
            delta.font_size_half_points = core::PropertyDelta<std::int32_t>::set(
                *style.format.font_size_half_points);
        if (style.format.bold.has_value())
            delta.bold = core::PropertyDelta<bool>::set(*style.format.bold);
        if (style.format.italic.has_value())
            delta.italic = core::PropertyDelta<bool>::set(*style.format.italic);
        if (style.format.underline.has_value())
            delta.underline = core::PropertyDelta<core::UnderlineStyle>::set(
                *style.format.underline ? core::UnderlineStyle::single
                                        : core::UnderlineStyle::none);
        if (style.format.strike.has_value())
            delta.strike = core::PropertyDelta<bool>::set(*style.format.strike);
        if (style.format.foreground_rgb)
            delta.foreground_argb = core::PropertyDelta<std::uint32_t>::set(
                0xff000000U | *style.format.foreground_rgb);
        if (style.format.highlight_rgb)
            delta.highlight_argb = core::PropertyDelta<std::uint32_t>::set(
                0xff000000U | *style.format.highlight_rgb);
        if (style.format.baseline) {
            const auto baseline =
                *style.format.baseline == ooxml::BasicBaseline::superscript
                    ? core::BaselinePosition::superscript
                : *style.format.baseline == ooxml::BasicBaseline::subscript
                    ? core::BaselinePosition::subscript
                    : core::BaselinePosition::normal;
            delta.baseline =
                core::PropertyDelta<core::BaselinePosition>::set(baseline);
        }
        if (!delta.empty()) {
            const auto id = document.paragraphs()[style.paragraphIndex].id();
            const auto applied = document.applyCharacterFormat(
                {{id, style.start}, {id, style.end}}, delta);
            if (!applied) return applied.error();
        }
    }

    struct ImportedListGeometry {
        core::NodeId listId;
        std::uint8_t level{};
        core::ListLayout layout;
        std::int32_t baseLeftIndentTwips{};
    };
    struct ListMetrics {
        qreal maximumMarkerWidth{};
        qreal spaceAdvance{};
    };
    struct ListCandidate {
        std::size_t group{};
        std::size_t level{};
        int bulletIndentSpaces{};
        int textIndentSpaces{};
        std::int32_t baseLeftIndentTwips{};
    };
    struct ListGroup {
        core::NodeId id{core::NodeId::generate()};
        core::ListLayout layout;
        std::array<std::optional<core::ListLevelLayout>,
                   core::kListLevelCount>
            observedLevels;
    };

    std::vector<std::optional<LiteralListMarker>> markers;
    markers.reserve(document.paragraphs().size());
    std::vector<std::optional<std::size_t>> listGroups(
        document.paragraphs().size());
    std::map<std::pair<std::size_t, std::size_t>, ListMetrics> metrics;
    std::vector<ListGroup> groups;
    std::map<std::int32_t, std::size_t> nativeNumberingGroups;
    LiteralListMarker::Kind previousKind{LiteralListMarker::Kind::bullet};
    bool hasPreviousKind{false};
    std::optional<std::size_t> currentGroup;
    tabWidthSpaces = std::max(1, tabWidthSpaces);
    for (std::size_t index = 0; index < document.paragraphs().size(); ++index) {
        const auto& paragraph = document.paragraphs()[index];
        const QString text = QString::fromUtf16(
            paragraph.text().data(),
            static_cast<qsizetype>(paragraph.text().size()));
        auto marker = literalListMarker(text);
        markers.push_back(marker);
        if (!marker) {
            hasPreviousKind = false;
            currentGroup.reset();
            continue;
        }
        if (index < sourceParagraphs.size() &&
            sourceParagraphs[index].numbering_id &&
            *sourceParagraphs[index].numbering_id > 0) {
            const auto [found, inserted] = nativeNumberingGroups.emplace(
                *sourceParagraphs[index].numbering_id, groups.size());
            if (inserted) groups.emplace_back();
            currentGroup = found->second;
        } else if (!hasPreviousKind || previousKind != marker->kind ||
                   !currentGroup) {
            groups.emplace_back();
            currentGroup = groups.size() - 1;
        }
        previousKind = marker->kind;
        hasPreviousKind = true;
        listGroups[index] = currentGroup;
        const auto level =
            index < sourceParagraphs.size() &&
                    sourceParagraphs[index].numbering
                ? std::min<std::size_t>(
                      core::kListLevelCount - 1,
                      sourceParagraphs[index].numbering->level)
                : std::min<std::size_t>(
                      core::kListLevelCount - 1,
                      static_cast<std::size_t>(
                          indentationColumns(marker->indent, tabWidthSpaces) /
                          tabWidthSpaces));
        auto& itemMetrics = metrics[{*currentGroup, level}];
        const auto markerOffset =
            static_cast<std::size_t>(marker->indent.size());
        itemMetrics.maximumMarkerWidth = std::max(
            itemMetrics.maximumMarkerWidth,
            listTextAdvance(paragraph, markerOffset, marker->marker,
                            defaultFontFamily, defaultFontPointSize));
        if (itemMetrics.spaceAdvance <= 0.0) {
            itemMetrics.spaceAdvance = listSpaceAdvance(
                paragraph,
                std::min(paragraph.text().size(),
                         static_cast<std::size_t>(marker->prefixLength)),
                defaultFontFamily, defaultFontPointSize);
        }
    }

    std::vector<std::optional<ListCandidate>> candidates(
        document.paragraphs().size());
    constexpr std::int64_t kMaximumTabStopTwips = 31'680;
    for (std::size_t index = 0; index < document.paragraphs().size(); ++index) {
        if (!markers[index] || !listGroups[index] ||
            index >= sourceParagraphs.size()) {
            continue;
        }
        const auto& marker = *markers[index];
        const auto& source = sourceParagraphs[index];
        if (!marker.separator.contains(QLatin1Char('\t')) ||
            !source.left_indent_twips || !source.first_line_indent_twips ||
            *source.first_line_indent_twips >= 0 ||
            source.left_tab_stops_twips.empty()) {
            continue;
        }
        const auto tab = std::find_if(
            source.left_tab_stops_twips.begin(),
            source.left_tab_stops_twips.end(),
            [&source](std::uint32_t position) {
                return std::abs(static_cast<std::int64_t>(position) -
                                *source.left_indent_twips) <= 2;
            });
        if (tab == source.left_tab_stops_twips.end()) continue;

        const int bulletIndentSpaces =
            indentationColumns(marker.indent, tabWidthSpaces);
        if (bulletIndentSpaces < 0 ||
            bulletIndentSpaces > core::kMaximumListIndentSpaces) {
            continue;
        }
        const auto level = source.numbering
            ? std::min<std::size_t>(
                  core::kListLevelCount - 1, source.numbering->level)
            : std::min<std::size_t>(
                  core::kListLevelCount - 1,
                  static_cast<std::size_t>(
                      bulletIndentSpaces / tabWidthSpaces));
        const auto metric = metrics.find({*listGroups[index], level});
        if (metric == metrics.end() || metric->second.spaceAdvance <= 0.0) {
            continue;
        }
        const auto& paragraph = document.paragraphs()[index];
        const qreal bulletOffset =
            static_cast<qreal>(bulletIndentSpaces) *
            metric->second.spaceAdvance;
        const qreal storedIndentWidth = listTextAdvance(
            paragraph, 0, marker.indent, defaultFontFamily,
            defaultFontPointSize);
        std::int64_t baseLeft =
            static_cast<std::int64_t>(*source.left_indent_twips) +
            static_cast<std::int64_t>(*source.first_line_indent_twips);
        if (source.numbering) {
            // A native numbering marker is generated at left - hanging; its
            // leading spaces exist only in the editor's semantic facade.
            baseLeft -= std::llround(bulletOffset * 20.0);
            if (std::abs(baseLeft) <= 2) baseLeft = 0;
        } else {
            // Literal-marker DOCX text physically contains its indentation.
            baseLeft +=
                std::llround((storedIndentWidth - bulletOffset) * 20.0);
        }
        const auto textOffsetTwips =
            static_cast<std::int64_t>(*tab) - baseLeft;
        const qreal gapWidth =
            static_cast<qreal>(textOffsetTwips) / 20.0 - bulletOffset -
            metric->second.maximumMarkerWidth;
        const int textIndentSpaces = static_cast<int>(std::llround(
            gapWidth / metric->second.spaceAdvance));
        if (textIndentSpaces < 0 ||
            textIndentSpaces > core::kMaximumListTextIndentSpaces ||
            baseLeft < std::numeric_limits<std::int32_t>::min() ||
            baseLeft > std::numeric_limits<std::int32_t>::max()) {
            continue;
        }
        const qreal expectedTextOffset =
            bulletOffset + metric->second.maximumMarkerWidth +
            static_cast<qreal>(textIndentSpaces) *
                metric->second.spaceAdvance;
        // Converting arbitrary OOXML twips to integer space equivalents
        // necessarily rounds by as much as half a space. Allow that bounded
        // quantization while still rejecting unrelated hanging-indent text.
        const qreal tolerance =
            std::max<qreal>(0.15, metric->second.spaceAdvance * 0.55);
        if (std::abs(static_cast<qreal>(textOffsetTwips) / 20.0 -
                     expectedTextOffset) > tolerance ||
            *tab == 0 || *tab > kMaximumTabStopTwips) {
            continue;
        }

        auto& group = groups[*listGroups[index]];
        const core::ListLevelLayout observed{
            bulletIndentSpaces, textIndentSpaces};
        auto& previous = group.observedLevels[level];
        if (previous && *previous != observed) continue;
        previous = observed;
        candidates[index] = ListCandidate{
            *listGroups[index], level, bulletIndentSpaces,
            textIndentSpaces, static_cast<std::int32_t>(baseLeft)};
    }
    for (auto& group : groups) {
        for (std::size_t level = 0; level < core::kListLevelCount; ++level) {
            if (group.observedLevels[level]) {
                group.layout.levels[level] = *group.observedLevels[level];
            }
        }
    }
    std::vector<std::optional<ImportedListGeometry>> importedLists(
        document.paragraphs().size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!candidates[index]) continue;
        const auto& candidate = *candidates[index];
        const auto& group = groups[candidate.group];
        importedLists[index] = ImportedListGeometry{
            group.id, static_cast<std::uint8_t>(candidate.level), group.layout,
            candidate.baseLeftIndentTwips};
    }

    constexpr std::int64_t kEmuPerTwip = 635;
    for (std::size_t index = 0; index < sourceParagraphs.size(); ++index) {
        const auto& source = sourceParagraphs[index];
        core::ParagraphFormatDelta delta;
        if (source.alignment) {
            core::ParagraphAlignment alignment = core::ParagraphAlignment::left;
            switch (*source.alignment) {
                case ooxml::BasicParagraphAlignment::left:
                    alignment = core::ParagraphAlignment::left; break;
                case ooxml::BasicParagraphAlignment::center:
                    alignment = core::ParagraphAlignment::center; break;
                case ooxml::BasicParagraphAlignment::right:
                    alignment = core::ParagraphAlignment::right; break;
                case ooxml::BasicParagraphAlignment::justified:
                    alignment = core::ParagraphAlignment::justified; break;
            }
            delta.alignment = core::PropertyDelta<core::ParagraphAlignment>::set(alignment);
        }
        const auto set_signed_twips = [](const std::optional<std::int32_t>& value,
                                         auto& target) {
            if (value) target = core::PropertyDelta<std::int64_t>::set(
                static_cast<std::int64_t>(*value) * kEmuPerTwip);
        };
        const auto set_unsigned_twips = [](const std::optional<std::uint32_t>& value,
                                           auto& target) {
            if (value) target = core::PropertyDelta<std::int64_t>::set(
                static_cast<std::int64_t>(*value) * kEmuPerTwip);
        };
        if (index < importedLists.size() && importedLists[index]) {
            if (importedLists[index]->baseLeftIndentTwips != 0) {
                delta.left_indent_emu = core::PropertyDelta<std::int64_t>::set(
                    static_cast<std::int64_t>(
                        importedLists[index]->baseLeftIndentTwips) *
                    kEmuPerTwip);
            }
        } else {
            set_signed_twips(source.left_indent_twips, delta.left_indent_emu);
        }
        set_signed_twips(source.right_indent_twips, delta.right_indent_emu);
        if (index >= importedLists.size() || !importedLists[index]) {
            set_signed_twips(source.first_line_indent_twips,
                             delta.first_line_indent_emu);
        }
        set_unsigned_twips(source.space_before_twips, delta.space_before_emu);
        set_unsigned_twips(source.space_after_twips, delta.space_after_emu);
        set_unsigned_twips(source.line_spacing, delta.line_spacing_emu);
        if (source.line_spacing_rule) {
            core::LineSpacingRule rule = core::LineSpacingRule::automatic;
            switch (*source.line_spacing_rule) {
                case ooxml::BasicLineSpacingRule::automatic:
                    rule = core::LineSpacingRule::automatic; break;
                case ooxml::BasicLineSpacingRule::at_least:
                    rule = core::LineSpacingRule::at_least; break;
                case ooxml::BasicLineSpacingRule::exact:
                    rule = core::LineSpacingRule::exact; break;
            }
            delta.line_spacing_rule = core::PropertyDelta<core::LineSpacingRule>::set(rule);
        }
        if (source.keep_with_next) {
            delta.keep_with_next = core::PropertyDelta<bool>::set(*source.keep_with_next);
        }
        if (source.keep_lines) {
            delta.keep_lines = core::PropertyDelta<bool>::set(*source.keep_lines);
        }
        if (source.page_break_before) {
            delta.page_break_before = core::PropertyDelta<bool>::set(*source.page_break_before);
        }
        if (index > 0 && sourceParagraphs[index - 1].hard_page_break_after) {
            delta.page_break_before = core::PropertyDelta<bool>::set(true);
        }
        if (index < importedLists.size() && importedLists[index]) {
            const auto& list = *importedLists[index];
            delta.list_id = core::PropertyDelta<core::NodeId>::set(list.listId);
            delta.list_level =
                core::PropertyDelta<std::uint8_t>::set(list.level);
            delta.list_layout =
                core::PropertyDelta<core::ListLayout>::set(list.layout);
        }
        if (delta.empty()) continue;
        const auto applied = document.applyParagraphFormat(
            {document.paragraphs()[index].id()}, delta);
        if (!applied) return applied.error();
    }
    for (std::size_t index = 0; index < styleProvenance.size(); ++index) {
        if (!styleProvenance[index] ||
            index >= document.paragraphs().size()) {
            continue;
        }
        const auto attached = document.setParagraphStyleProvenance(
            document.paragraphs()[index].id(),
            std::move(styleProvenance[index]));
        if (!attached) return attached.error();
    }
    return document;
}

using ImportedTableStyleSources = std::map<
    core::NodeId,
    std::pair<std::optional<core::TableStyle>, std::string>>;

struct ImportedSemanticDocument {
    core::Document document;
    // One source-package paragraph index for each semantic body paragraph.
    // A synthetic paragraph (needed because the core always retains an edit
    // surface) uses max<size_t> and therefore can never be text-patched.
    std::vector<std::size_t> sourceParagraphIndices;
    // Native OOXML numbering is presented through the editor's existing
    // marker surface. These generated prefixes are removed again before a
    // safe w:t patch so the original numPr/numbering.xml remains authoritative.
    QStringList sourceTextPrefixes;
    std::vector<ImportedInlineImagePresentation> images;
    std::vector<ImportedTablePresentation> tables;
};

core::Result<ImportedSemanticDocument> documentFromOoxml(
    const ooxml::DocxDocument& package,
    const QString& defaultFontFamily, double defaultFontPointSize,
    int tabWidthSpaces) {
    const auto& sourceParagraphs = package.paragraphs();
    const auto& sourceBlocks = package.bodyBlocks();
    std::vector<std::size_t> sourceIndices;
    std::set<std::size_t> included;
    const auto includeParagraph = [&](std::size_t sourceIndex) {
        if (sourceIndex < sourceParagraphs.size() &&
            included.insert(sourceIndex).second) {
            sourceIndices.push_back(sourceIndex);
        }
    };

    for (const auto& block : sourceBlocks) {
        std::visit(
            [&](const auto& typed) {
                using Type = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<
                                  Type, ooxml::ImportedParagraphBlock>) {
                    includeParagraph(typed.source_paragraph_index);
                } else if constexpr (std::is_same_v<
                                         Type,
                                         ooxml::ImportedUnsupportedBodyBlock>) {
                    for (const auto index : typed.fallback_paragraph_indices) {
                        includeParagraph(index);
                    }
                }
            },
            block);
    }
    // Defensive compatibility for packages parsed by an older descriptor:
    // retain direct body paragraphs, or the complete flat view if necessary.
    if (sourceIndices.empty()) {
        for (std::size_t index = 0; index < sourceParagraphs.size(); ++index) {
            if (sourceParagraphs[index].direct_body_child) {
                includeParagraph(index);
            }
        }
    }
    if (sourceIndices.empty() && sourceBlocks.empty()) {
        for (std::size_t index = 0; index < sourceParagraphs.size(); ++index) {
            includeParagraph(index);
        }
    }

    std::vector<ooxml::Paragraph> bodyParagraphs;
    bodyParagraphs.reserve(sourceIndices.size());
    for (const auto index : sourceIndices) {
        bodyParagraphs.push_back(sourceParagraphs[index]);
    }
    QStringList sourceTextPrefixes;
    auto documentResult = documentFromOoxmlParagraphs(
        bodyParagraphs, defaultFontFamily, defaultFontPointSize,
        tabWidthSpaces, &sourceTextPrefixes);
    if (!documentResult) return documentResult.error();
    auto document = std::move(documentResult.value());

    constexpr auto kSyntheticParagraph =
        std::numeric_limits<std::size_t>::max();
    const bool usesSyntheticParagraph = sourceIndices.empty();
    if (usesSyntheticParagraph) {
        sourceIndices.push_back(kSyntheticParagraph);
    }

    std::vector<std::optional<core::NodeId>> sourceToCore(
        sourceParagraphs.size());
    std::vector<std::optional<std::size_t>> sourceToSemanticIndex(
        sourceParagraphs.size());
    for (std::size_t index = 0;
         index < sourceIndices.size() && index < document.paragraphs().size();
         ++index) {
        if (sourceIndices[index] < sourceToCore.size()) {
            sourceToCore[sourceIndices[index]] =
                document.paragraphs()[index].id();
            sourceToSemanticIndex[sourceIndices[index]] = index;
        }
    }
    std::vector<ImportedInlineImagePresentation> imagePresentations;
    RasterCacheLimits imageLimits;
    imageLimits.per_image.maximum_encoded_bytes =
        core::kMaximumEncodedImageBytes;
    imageLimits.maximum_references = core::kMaximumInlineImagesPerDocument;
    imageLimits.maximum_unique_images = core::kMaximumInlineImagesPerDocument;
    BoundedRasterCache imageCache(imageLimits);
    std::map<std::string, core::EncodedImagePayload> encodedImageCache;
    std::size_t importedImageCount = 0;
    std::size_t importedEncodedImageBytes = 0;
    for (std::size_t sourceIndex = 0; sourceIndex < sourceToCore.size();
         ++sourceIndex) {
        if (!sourceToCore[sourceIndex] ||
            !sourceToSemanticIndex[sourceIndex]) {
            continue;
        }
        const auto semanticIndex = *sourceToSemanticIndex[sourceIndex];
        std::size_t semanticOffset = semanticIndex <
                static_cast<std::size_t>(sourceTextPrefixes.size())
            ? static_cast<std::size_t>(
                  sourceTextPrefixes[static_cast<qsizetype>(semanticIndex)]
                      .size())
            : 0U;
        for (const auto& run : sourceParagraphs[sourceIndex].runs) {
            for (const auto& fragment : run.fragments) {
                switch (fragment.kind) {
                    case ooxml::FragmentKind::text: {
                        QString fragmentText = fromUtf8(fragment.text);
                        fragmentText.replace(QStringLiteral("\r\n"),
                                             QStringLiteral("\n"));
                        fragmentText.replace(QLatin1Char('\r'),
                                             QLatin1Char('\n'));
                        fragmentText.replace(QLatin1Char('\n'),
                                             QChar::LineSeparator);
                        semanticOffset += static_cast<std::size_t>(
                            fragmentText.size());
                        break;
                    }
                    case ooxml::FragmentKind::tab:
                    case ooxml::FragmentKind::line_break:
                        ++semanticOffset;
                        break;
                    case ooxml::FragmentKind::equation:
                        if (fragment.equation) ++semanticOffset;
                        break;
                    case ooxml::FragmentKind::inline_image: {
                        if (!fragment.inline_image) break;
                        const auto& sourceImage = *fragment.inline_image;
                        if (!sourceImage.renderable()) break;
                        const auto inspection = raster::inspect(
                            sourceImage.bytes.view(), raster::Format::unknown,
                            imageLimits.per_image);
                        if (!inspection.ok()) break;
                        if (importedImageCount >=
                                core::kMaximumInlineImagesPerDocument ||
                            sourceImage.bytes.size() >
                                core::kMaximumDocumentEncodedImageBytes -
                                    std::min(
                                        importedEncodedImageBytes,
                                        core::kMaximumDocumentEncodedImageBytes)) {
                            return core::Error{
                                core::ErrorCode::invalid_operation,
                                "Imported document exceeds Owl Docs' bounded inline-picture budget"};
                        }
                        auto decoded = imageCache.decode(
                            sourceImage.package_member,
                            sourceImage.bytes.view());
                        if (!decoded.ok()) break;
                        auto encoded = encodedImageCache.find(
                            sourceImage.package_member);
                        if (encoded == encodedImageCache.end()) {
                            encoded = encodedImageCache.emplace(
                                sourceImage.package_member,
                                core::EncodedImagePayload(
                                    std::vector<std::uint8_t>(
                                    sourceImage.bytes.view().begin(),
                                    sourceImage.bytes.view().end())))
                                          .first;
                        }
                        const auto imageId = core::NodeId::generate();
                        core::ImageLayout imageLayout;
                        switch (sourceImage.layout.placement) {
                            case ooxml::ImagePlacement::inline_with_text:
                                imageLayout.placement =
                                    core::ImagePlacement::inline_with_text;
                                break;
                            case ooxml::ImagePlacement::square:
                                imageLayout.placement =
                                    core::ImagePlacement::square;
                                break;
                            case ooxml::ImagePlacement::top_and_bottom:
                                imageLayout.placement =
                                    core::ImagePlacement::top_and_bottom;
                                break;
                        }
                        imageLayout.distance_top_emu =
                            sourceImage.layout.distance_top_emu;
                        imageLayout.distance_right_emu =
                            sourceImage.layout.distance_right_emu;
                        imageLayout.distance_bottom_emu =
                            sourceImage.layout.distance_bottom_emu;
                        imageLayout.distance_left_emu =
                            sourceImage.layout.distance_left_emu;
                        imageLayout.move_with_text =
                            sourceImage.layout.move_with_text;
                        const auto inserted = document.insertImage(
                            {*sourceToCore[sourceIndex], semanticOffset},
                            encoded->second,
                            inspection.format == raster::Format::png
                                ? core::ImageFormat::png
                                : core::ImageFormat::jpeg,
                            sourceImage.accessible_name.empty()
                                ? (sourceImage.name.empty()
                                       ? std::string("Picture")
                                       : sourceImage.name)
                                : sourceImage.accessible_name,
                            sourceImage.width_emu, sourceImage.height_emu,
                            imageId, std::nullopt, imageLayout);
                        if (!inserted) return inserted.error();
                        imagePresentations.emplace_back(
                            imageId, std::move(decoded.image));
                        ++importedImageCount;
                        importedEncodedImageBytes += sourceImage.bytes.size();
                        ++semanticOffset;
                        break;
                    }
                    case ooxml::FragmentKind::page_break:
                        break;
                }
            }
        }
    }
    const auto nextParagraphAfter =
        [&](std::size_t blockIndex) -> std::optional<core::NodeId> {
        for (std::size_t next = blockIndex + 1; next < sourceBlocks.size();
             ++next) {
            if (const auto* paragraph =
                    std::get_if<ooxml::ImportedParagraphBlock>(
                        &sourceBlocks[next])) {
                if (paragraph->source_paragraph_index < sourceToCore.size()) {
                    const auto id =
                        sourceToCore[paragraph->source_paragraph_index];
                    if (id) return id;
                }
            } else if (const auto* unsupported =
                           std::get_if<ooxml::ImportedUnsupportedBodyBlock>(
                               &sourceBlocks[next])) {
                for (const auto sourceIndex :
                     unsupported->fallback_paragraph_indices) {
                    if (sourceIndex < sourceToCore.size() &&
                        sourceToCore[sourceIndex]) {
                        return sourceToCore[sourceIndex];
                    }
                }
            }
        }
        if (usesSyntheticParagraph && !document.paragraphs().empty()) {
            return document.paragraphs().front().id();
        }
        return std::nullopt;
    };

    std::vector<ImportedTablePresentation> tablePresentations;
    for (std::size_t blockIndex = 0; blockIndex < sourceBlocks.size();
         ++blockIndex) {
        const auto* imported = std::get_if<ooxml::ImportedTableBlock>(
            &sourceBlocks[blockIndex]);
        if (!imported) continue;
        if (imported->rows == 0 || imported->columns == 0 ||
            imported->rows > core::Table::maximum_rows ||
            imported->columns > core::Table::maximum_columns ||
            imported->rows >
                std::numeric_limits<std::size_t>::max() / imported->columns ||
            imported->cells.size() != imported->rows * imported->columns) {
            return core::Error{core::ErrorCode::invalid_operation,
                               "Imported table dimensions are invalid"};
        }
        const auto tableId = core::NodeId::generate();
        std::set<core::NodeId> tableNodeIds{tableId};
        std::vector<core::TableCell> semanticCells;
        semanticCells.reserve(imported->cells.size());
        ImportedTablePresentation tablePresentation;
        tablePresentation.tableId = tableId;
        tablePresentation.sourceSemanticStyle =
            importedTableStyle(imported->style);
        tablePresentation.sourceStyleId = imported->source_style_id;
        tablePresentation.alignment = importedParagraphAlignment(
            imported->alignment);
        tablePresentation.columnWidthsPoints.reserve(
            imported->column_widths_twips.size());
        for (const auto width : imported->column_widths_twips) {
            tablePresentation.columnWidthsPoints.push_back(
                static_cast<double>(width) / 20.0);
        }
        tablePresentation.cellIds.reserve(imported->cells.size());
        tablePresentation.cells.reserve(imported->cells.size());
        for (std::size_t cellIndex = 0;
             cellIndex < imported->cells.size(); ++cellIndex) {
            const auto sourceIndex =
                imported->cells[cellIndex].source_paragraph_index;
            if (sourceIndex >= sourceParagraphs.size()) {
                return core::Error{core::ErrorCode::invalid_operation,
                                   "Imported table cell source is invalid"};
            }
            QString text = fromUtf8(sourceParagraphs[sourceIndex].plainText());
            text.replace(QStringLiteral("\r\n"),
                         QString(QChar::LineSeparator));
            text.replace(QLatin1Char('\r'), QChar::LineSeparator);
            text.replace(QLatin1Char('\n'), QChar::LineSeparator);
            auto presentation = importedTableCellPresentation(
                imported->cells[cellIndex], sourceParagraphs[sourceIndex]);
            auto cellId = core::NodeId::generate();
            while (!tableNodeIds.insert(cellId).second) {
                cellId = core::NodeId::generate();
            }
            core::CharacterFormat defaultCellFormat;
            if (sourceParagraphs[sourceIndex]
                    .paragraph_mark_format.has_value()) {
                defaultCellFormat = importedCharacterFormat(
                    *sourceParagraphs[sourceIndex].paragraph_mark_format);
            } else if (text.isEmpty() &&
                       !sourceParagraphs[sourceIndex].runs.empty()) {
                defaultCellFormat = importedCharacterFormat(
                    sourceParagraphs[sourceIndex].runs.back().format);
            }
            semanticCells.emplace_back(
                cellId, text.toStdU16String(), presentation.formats,
                importedTableCellParagraphFormat(
                    sourceParagraphs[sourceIndex]),
                std::move(defaultCellFormat));
            tablePresentation.cellIds.push_back(cellId);
            tablePresentation.cells.push_back(std::move(presentation));
        }
        auto tableResult = core::Table::restore(
            imported->rows, imported->columns, imported->header_row,
            tableId, std::move(semanticCells),
            importedTableStyle(imported->style));
        if (!tableResult) return tableResult.error();
        const auto inserted = document.insertTable(
            nextParagraphAfter(blockIndex), std::move(tableResult.value()));
        if (!inserted) return inserted.error();
        tablePresentations.push_back(std::move(tablePresentation));
    }

    return ImportedSemanticDocument{std::move(document),
                                    std::move(sourceIndices),
                                    std::move(sourceTextPrefixes),
                                    std::move(imagePresentations),
                                    std::move(tablePresentations)};
}

QStringList currentTexts(const core::DocumentSnapshot& snapshot) {
    QStringList result;
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(QString::fromUtf16(paragraph.text().data(),
                                           static_cast<qsizetype>(paragraph.text().size())));
    }
    return result;
}

ooxml::BasicRunFormat toOoxmlSparseFormat(
    const core::CharacterFormat& format) {
    ooxml::BasicRunFormat result;
    result.font_family = format.font_family;
    result.font_size_half_points = format.font_size_half_points;
    result.bold = format.bold;
    result.italic = format.italic;
    if (format.underline.has_value()) {
        result.underline =
            *format.underline != core::UnderlineStyle::none;
    }
    result.strike = format.strike;
    if (format.foreground_argb) result.foreground_rgb = *format.foreground_argb & 0x00ffffffU;
    if (format.highlight_argb) result.highlight_rgb = *format.highlight_argb & 0x00ffffffU;
    if (format.baseline) {
        switch (*format.baseline) {
            case core::BaselinePosition::normal:
                result.baseline = ooxml::BasicBaseline::normal; break;
            case core::BaselinePosition::superscript:
                result.baseline = ooxml::BasicBaseline::superscript; break;
            case core::BaselinePosition::subscript:
                result.baseline = ooxml::BasicBaseline::subscript; break;
        }
    }
    return result;
}

ooxml::BasicRunFormat toOoxmlFormat(
    const core::CharacterFormat& format,
    const QString& defaultFontFamily = QStringLiteral("Helvetica"),
    double defaultFontPointSize = 11.0) {
    auto result = toOoxmlSparseFormat(format);
    if (!result.font_family) {
        result.font_family = defaultFontFamily.toStdString();
    }
    if (!result.font_size_half_points) {
        result.font_size_half_points = static_cast<std::int32_t>(
            std::lround(defaultFontPointSize * 2.0));
    }
    return result;
}

bool ooxmlRunFormatIsEmpty(const ooxml::BasicRunFormat& format) {
    return !format.font_family && !format.font_size_half_points &&
           !format.bold && !format.italic && !format.underline &&
           !format.strike && !format.foreground_rgb &&
           !format.highlight_rgb && !format.baseline;
}

core::CharacterFormat builtInStyleCharacterBaseline(
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

// Paragraphs carrying a native style store effective formatting in the core
// so layout never has to consult OOXML.  Serializing that effective baseline
// as direct rPr would, however, turn every inherited style property into an
// override after reopening the generated DOCX.  Compare against the built-in
// definition that this writer actually emits, not an imported style's source
// definition: a foreign built-in with the same style id may have completely
// different values, and those differences must become direct properties in a
// simplified copy to preserve its appearance.  Provenance masks still force
// exact source-direct properties (including explicit false/equal values).
// Missing effective values use the same local defaults as the renderer so a
// direct clear can still override a non-default style property.
ooxml::BasicRunFormat toOoxmlStyledFormat(
    const core::CharacterFormat& format,
    const core::CharacterFormat& inherited,
    const core::CharacterFormatMask& direct,
    const QString& defaultFontFamily, double defaultFontPointSize) {
    ooxml::BasicRunFormat result;
    const std::string defaultFamily = defaultFontFamily.toStdString();
    const auto defaultHalfPoints = static_cast<std::int32_t>(
        std::lround(defaultFontPointSize * 2.0));

    const std::string currentFamily =
        format.font_family.value_or(defaultFamily);
    const std::string inheritedFamily =
        inherited.font_family.value_or(defaultFamily);
    if (direct.font_family || currentFamily != inheritedFamily) {
        result.font_family = currentFamily;
    }

    const auto currentSize =
        format.font_size_half_points.value_or(defaultHalfPoints);
    const auto inheritedSize =
        inherited.font_size_half_points.value_or(defaultHalfPoints);
    if (direct.font_size_half_points || currentSize != inheritedSize) {
        result.font_size_half_points = currentSize;
    }

    const auto onOff = [](const std::optional<bool>& value) {
        return value.value_or(false);
    };
    const bool currentBold = onOff(format.bold);
    const bool inheritedBold = onOff(inherited.bold);
    if (direct.bold || currentBold != inheritedBold) {
        result.bold = currentBold;
    }
    const bool currentItalic = onOff(format.italic);
    const bool inheritedItalic = onOff(inherited.italic);
    if (direct.italic || currentItalic != inheritedItalic) {
        result.italic = currentItalic;
    }
    const auto currentUnderline =
        format.underline.value_or(core::UnderlineStyle::none);
    const auto inheritedUnderline =
        inherited.underline.value_or(core::UnderlineStyle::none);
    if (direct.underline || currentUnderline != inheritedUnderline) {
        result.underline = currentUnderline != core::UnderlineStyle::none;
    }
    const bool currentStrike = onOff(format.strike);
    const bool inheritedStrike = onOff(inherited.strike);
    if (direct.strike || currentStrike != inheritedStrike) {
        result.strike = currentStrike;
    }

    constexpr std::uint32_t kDefaultForeground = 0xff000000U;
    const auto currentForeground =
        format.foreground_argb.value_or(kDefaultForeground);
    const auto inheritedForeground =
        inherited.foreground_argb.value_or(kDefaultForeground);
    if (direct.foreground_argb ||
        currentForeground != inheritedForeground) {
        result.foreground_rgb = currentForeground & 0x00ffffffU;
    }

    // BasicRunFormat currently models a concrete shading fill but not an
    // explicit "no shading" token.  Concrete direct/different highlights are
    // safe to emit; an absent value remains inherited/transparent.
    if ((direct.highlight_argb ||
         format.highlight_argb != inherited.highlight_argb) &&
        format.highlight_argb) {
        result.highlight_rgb = *format.highlight_argb & 0x00ffffffU;
    }

    const auto currentBaseline =
        format.baseline.value_or(core::BaselinePosition::normal);
    const auto inheritedBaseline =
        inherited.baseline.value_or(core::BaselinePosition::normal);
    if (direct.baseline || currentBaseline != inheritedBaseline) {
        switch (currentBaseline) {
            case core::BaselinePosition::normal:
                result.baseline = ooxml::BasicBaseline::normal;
                break;
            case core::BaselinePosition::superscript:
                result.baseline = ooxml::BasicBaseline::superscript;
                break;
            case core::BaselinePosition::subscript:
                result.baseline = ooxml::BasicBaseline::subscript;
                break;
        }
    }
    // Language is retained in the semantic provenance mask, but the current
    // deliberately-small BasicRunFormat vocabulary cannot author w:lang yet.
    return result;
}

ooxml::NewParagraph toOoxmlTableCellParagraph(
    const core::TableCell& cell, QStringList& losses,
    const QString& defaultFontFamily, double defaultFontPointSize,
    bool defaultBold) {
    constexpr double kEmuPerTwip = 635.0;
    ooxml::NewParagraph output;
    const auto& format = cell.paragraph_format;
    if (format.alignment) {
        switch (*format.alignment) {
            case core::ParagraphAlignment::left:
                output.alignment = ooxml::BasicParagraphAlignment::left;
                break;
            case core::ParagraphAlignment::center:
                output.alignment = ooxml::BasicParagraphAlignment::center;
                break;
            case core::ParagraphAlignment::right:
                output.alignment = ooxml::BasicParagraphAlignment::right;
                break;
            case core::ParagraphAlignment::justified:
            case core::ParagraphAlignment::distributed:
                output.alignment = ooxml::BasicParagraphAlignment::justified;
                break;
        }
    }
    const auto signedTwips = [&losses, kEmuPerTwip](
                                 const std::optional<std::int64_t>& value,
                                 const QString& property)
        -> std::optional<std::int32_t> {
        if (!value) return std::nullopt;
        const auto rounded = std::llround(
            static_cast<double>(*value) / kEmuPerTwip);
        if (rounded < std::numeric_limits<std::int32_t>::min() ||
            rounded > std::numeric_limits<std::int32_t>::max()) {
            losses.push_back(QObject::tr(
                "A table-cell %1 exceeds the DOCX range").arg(property));
            return std::nullopt;
        }
        return static_cast<std::int32_t>(rounded);
    };
    const auto unsignedTwips = [&losses, kEmuPerTwip](
                                   const std::optional<std::int64_t>& value,
                                   const QString& property)
        -> std::optional<std::uint32_t> {
        if (!value) return std::nullopt;
        const auto rounded = std::llround(
            static_cast<double>(*value) / kEmuPerTwip);
        if (rounded < 0 ||
            static_cast<std::uint64_t>(rounded) >
                std::numeric_limits<std::uint32_t>::max()) {
            losses.push_back(QObject::tr(
                "A table-cell %1 exceeds the DOCX range").arg(property));
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(rounded);
    };
    output.left_indent_twips = signedTwips(
        format.left_indent_emu, QObject::tr("left indent"));
    output.right_indent_twips = signedTwips(
        format.right_indent_emu, QObject::tr("right indent"));
    output.first_line_indent_twips = signedTwips(
        format.first_line_indent_emu, QObject::tr("first-line indent"));
    output.space_before_twips = unsignedTwips(
        format.space_before_emu, QObject::tr("space before"));
    output.space_after_twips = unsignedTwips(
        format.space_after_emu, QObject::tr("space after"));
    output.line_spacing = unsignedTwips(
        format.line_spacing_emu, QObject::tr("line spacing"));
    if (format.line_spacing_rule && output.line_spacing) {
        switch (*format.line_spacing_rule) {
            case core::LineSpacingRule::automatic:
                output.line_spacing_rule =
                    ooxml::BasicLineSpacingRule::automatic;
                break;
            case core::LineSpacingRule::at_least:
                output.line_spacing_rule =
                    ooxml::BasicLineSpacingRule::at_least;
                break;
            case core::LineSpacingRule::exact:
                output.line_spacing_rule = ooxml::BasicLineSpacingRule::exact;
                break;
        }
    } else if (format.line_spacing_rule) {
        losses.push_back(QObject::tr(
            "A table-cell line-spacing rule has no serializable spacing value"));
    }
    output.keep_with_next = format.keep_with_next;
    output.keep_lines = format.keep_lines;
    output.page_break_before = format.page_break_before;

    const auto exportCharacterFormat = [&](core::CharacterFormat source) {
        if (defaultBold && !source.bold) source.bold = true;
        return toOoxmlFormat(
            source, defaultFontFamily, defaultFontPointSize);
    };
    if (cell.text.empty() || !cell.default_character_format.empty()) {
        output.paragraph_mark_format =
            exportCharacterFormat(cell.default_character_format);
    }
    if (cell.text.empty()) {
        return output;
    }
    const QString text = QString::fromUtf16(
        cell.text.data(), static_cast<qsizetype>(cell.text.size()));
    std::size_t cursor = 0;
    while (cursor < cell.text.size()) {
        const auto characterFormat = cell.characterFormatAt(cursor + 1U);
        const auto start = cursor;
        do {
            ++cursor;
        } while (cursor < cell.text.size() &&
                 cell.characterFormatAt(cursor + 1U) == characterFormat);
        output.runs.push_back({
            text.mid(static_cast<qsizetype>(start),
                     static_cast<qsizetype>(cursor - start))
                .toUtf8()
                .toStdString(),
            exportCharacterFormat(characterFormat),
            std::nullopt});
    }
    return output;
}

ooxml::PageSettings toOoxmlPageSettings(const DocumentCanvas& canvas) {
    const auto twips = [](double points) {
        return static_cast<std::uint32_t>(std::lround(points * 20.0));
    };
    ooxml::PageSettings page;
    page.width_twips = twips(canvas.pageWidthPoints());
    page.height_twips = twips(canvas.pageHeightPoints());
    page.margin_top_twips = twips(canvas.marginTopPoints());
    page.margin_right_twips = twips(canvas.marginRightPoints());
    page.margin_bottom_twips = twips(canvas.marginBottomPoints());
    page.margin_left_twips = twips(canvas.marginLeftPoints());
    return page;
}

ooxml::DocumentDefaults toOoxmlDocumentDefaults(
    const QString& fontFamily, double fontPointSize, int tabWidthSpaces) {
    ooxml::DocumentDefaults defaults;
    defaults.font_family = fontFamily.toStdString();
    defaults.font_size_half_points = static_cast<std::int32_t>(
        std::lround(fontPointSize * 2.0));

    QFont font(fontFamily);
    font.setPointSizeF(fontPointSize);
    const qreal tabWidthPoints =
        QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) *
        static_cast<qreal>(tabWidthSpaces);
    defaults.default_tab_stop_twips = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(std::llround(tabWidthPoints * 20.0),
                                 1, 31'680));
    return defaults;
}

ooxml::DocumentDefaults toOoxmlDocumentDefaults(
    const DocumentCanvas& canvas) {
    return toOoxmlDocumentDefaults(
        canvas.defaultFontFamily(), canvas.defaultFontPointSize(),
        canvas.tabWidthSpaces());
}

ooxml::BasicNumberFormat ooxmlNumberFormatForLevel(std::size_t level) {
    switch (level % 5U) {
        case 0: return ooxml::BasicNumberFormat::decimal;
        case 1: return ooxml::BasicNumberFormat::upper_letter;
        case 2: return ooxml::BasicNumberFormat::upper_roman;
        case 3: return ooxml::BasicNumberFormat::lower_letter;
        default: return ooxml::BasicNumberFormat::lower_roman;
    }
}

std::optional<std::int32_t> listMarkerStart(
    const QString& marker, ooxml::BasicNumberFormat format) {
    if (marker.size() < 2) return std::nullopt;
    const QString value = marker.first(marker.size() - 1);
    bool valid = false;
    qulonglong ordinal = 0;
    if (format == ooxml::BasicNumberFormat::decimal) {
        ordinal = value.toULongLong(&valid);
    } else if (format == ooxml::BasicNumberFormat::upper_letter ||
               format == ooxml::BasicNumberFormat::lower_letter) {
        valid = !value.isEmpty();
        const bool upper =
            format == ooxml::BasicNumberFormat::upper_letter;
        for (const QChar character : value) {
            const ushort first = upper ? static_cast<ushort>('A')
                                       : static_cast<ushort>('a');
            const ushort last = upper ? static_cast<ushort>('Z')
                                      : static_cast<ushort>('z');
            const ushort code = character.unicode();
            if (code < first || code > last ||
                ordinal > (std::numeric_limits<qulonglong>::max() - 26U) /
                              26U) {
                valid = false;
                break;
            }
            ordinal = ordinal * 26U +
                      static_cast<qulonglong>(code - first) + 1U;
        }
    } else if (format == ooxml::BasicNumberFormat::upper_roman ||
               format == ooxml::BasicNumberFormat::lower_roman) {
        const QString upperValue = value.toUpper();
        const auto romanValue = [](QChar character) -> unsigned int {
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
        valid = !upperValue.isEmpty();
        for (qsizetype index = 0; valid && index < upperValue.size(); ++index) {
            const unsigned int current = romanValue(upperValue.at(index));
            const unsigned int next = index + 1 < upperValue.size()
                ? romanValue(upperValue.at(index + 1))
                : 0U;
            if (current == 0U) {
                valid = false;
            } else if (current < next) {
                ordinal += static_cast<qulonglong>(next - current);
                ++index;
            } else {
                ordinal += current;
            }
        }
    }
    if (!valid || ordinal == 0 ||
        ordinal > static_cast<qulonglong>(
                      std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(ordinal);
}

std::vector<ooxml::NewParagraph> toOoxmlParagraphs(
    const core::DocumentSnapshot& snapshot, QStringList& losses,
    const QString& defaultFontFamily, double defaultFontPointSize) {
    struct ExportListMetrics {
        qreal maximumMarkerWidth{};
        qreal spaceAdvance{};
        std::int32_t start{1};
        bool hasStart{false};
    };
    using ExportListKey = std::pair<core::NodeId, std::uint8_t>;
    std::map<ExportListKey, ExportListMetrics> listMetrics;
    std::map<core::NodeId, std::int32_t> nativeListIds;
    std::int32_t nextNativeListId = 1;
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        const auto& format = paragraph.format();
        if (!format.list_id || !format.list_level || !format.list_layout) {
            continue;
        }
        if (!nativeListIds.contains(*format.list_id)) {
            nativeListIds.emplace(*format.list_id, nextNativeListId++);
        }
        const auto marker = literalListMarker(QString::fromUtf16(
            paragraph.text().data(),
            static_cast<qsizetype>(paragraph.text().size())));
        if (!marker) continue;
        auto& itemMetrics =
            listMetrics[{*format.list_id, *format.list_level}];
        if (!itemMetrics.hasStart &&
            marker->kind == LiteralListMarker::Kind::numbered) {
            const auto start = listMarkerStart(
                marker->marker,
                ooxmlNumberFormatForLevel(*format.list_level));
            if (start) itemMetrics.start = *start;
            itemMetrics.hasStart = true;
        }
        itemMetrics.maximumMarkerWidth = std::max(
            itemMetrics.maximumMarkerWidth,
            listTextAdvance(
                paragraph, static_cast<std::size_t>(marker->indent.size()),
                marker->marker, defaultFontFamily, defaultFontPointSize));
        if (itemMetrics.spaceAdvance <= 0.0) {
            itemMetrics.spaceAdvance = listSpaceAdvance(
                paragraph,
                std::min(paragraph.text().size(),
                         static_cast<std::size_t>(marker->prefixLength)),
                defaultFontFamily, defaultFontPointSize);
        }
    }

    std::vector<ooxml::NewParagraph> output;
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        ooxml::NewParagraph out;
        const core::ParagraphStyleDefinition* builtInStyle = nullptr;
        if (paragraph.styleId()) {
            builtInStyle =
                core::findBuiltInParagraphStyle(*paragraph.styleId());
            if (builtInStyle) {
                out.style_id = *paragraph.styleId();
            } else {
                losses.push_back(QObject::tr(
                    "Custom paragraph style '%1' will be flattened to its "
                    "visible formatting in this simplified copy")
                    .arg(QString::fromUtf8(
                        paragraph.styleId()->data(),
                        static_cast<qsizetype>(paragraph.styleId()->size()))));
            }
        }
        std::optional<core::CharacterFormat> styleCharacterBaseline;
        std::optional<core::ParagraphFormat> styleParagraphBaseline;
        core::CharacterFormatMask paragraphMarkOverrides;
        core::ParagraphFormatMask paragraphOverrides;
        if (builtInStyle) {
            // A regenerated styles.xml always contains Owl's deterministic
            // built-in definitions.  Use those as the sparsification target
            // even when the effective formatting was inherited from a
            // different source definition with the same style id.
            styleCharacterBaseline = builtInStyleCharacterBaseline(
                *builtInStyle, defaultFontFamily, defaultFontPointSize);
            styleParagraphBaseline = builtInStyle->paragraph_format;
            if (paragraph.styleProvenance()) {
                // Source provenance determines which properties were
                // explicitly direct.  It must not determine what may be
                // omitted, because the source style definition is not copied
                // into a simplified package.
                paragraphMarkOverrides = paragraph.styleProvenance()
                                             ->paragraph_mark_overrides;
                paragraphOverrides = paragraph.styleProvenance()
                                         ->paragraph_overrides;
            }
        }
        const auto exportCharacterFormat = [&]
            (const core::CharacterFormat& source,
             const core::CharacterFormatMask& direct) {
            return styleCharacterBaseline
                ? toOoxmlStyledFormat(
                      source, *styleCharacterBaseline, direct,
                      defaultFontFamily, defaultFontPointSize)
                : toOoxmlFormat(
                      source, defaultFontFamily, defaultFontPointSize);
        };
        const auto& format = paragraph.format();
        const auto serializeParagraphProperty = [&]
            (const auto& current, const auto& inherited,
             const auto& fallback, bool direct) {
            return !styleParagraphBaseline
                ? current.has_value()
                : direct || current.value_or(fallback) !=
                                inherited.value_or(fallback);
        };
        const auto inheritedParagraph = styleParagraphBaseline.value_or(
            core::ParagraphFormat{});
        const bool serializeAlignment = serializeParagraphProperty(
            format.alignment, inheritedParagraph.alignment,
            core::ParagraphAlignment::left, paragraphOverrides.alignment);
        if (serializeAlignment) {
            const auto alignment = format.alignment.value_or(
                core::ParagraphAlignment::left);
            switch (alignment) {
                case core::ParagraphAlignment::left: out.alignment = ooxml::BasicParagraphAlignment::left; break;
                case core::ParagraphAlignment::center: out.alignment = ooxml::BasicParagraphAlignment::center; break;
                case core::ParagraphAlignment::right: out.alignment = ooxml::BasicParagraphAlignment::right; break;
                case core::ParagraphAlignment::justified:
                case core::ParagraphAlignment::distributed:
                    out.alignment = ooxml::BasicParagraphAlignment::justified; break;
            }
        }
        constexpr double kEmuPerTwip = 635.0;
        const auto signed_twips = [&](const std::optional<std::int64_t>& value)
            -> std::optional<std::int32_t> {
            if (!value) return std::nullopt;
            const auto rounded = std::llround(static_cast<double>(*value) / kEmuPerTwip);
            if (rounded < std::numeric_limits<std::int32_t>::min() ||
                rounded > std::numeric_limits<std::int32_t>::max()) {
                losses.push_back(QObject::tr("A paragraph indent exceeds the DOCX range"));
                return std::nullopt;
            }
            return static_cast<std::int32_t>(rounded);
        };
        const auto unsigned_twips = [&](const std::optional<std::int64_t>& value)
            -> std::optional<std::uint32_t> {
            if (!value) return std::nullopt;
            const auto rounded = std::llround(static_cast<double>(*value) / kEmuPerTwip);
            if (rounded < 0 ||
                static_cast<std::uint64_t>(rounded) >
                    std::numeric_limits<std::uint32_t>::max()) {
                losses.push_back(QObject::tr("Paragraph spacing exceeds the DOCX range"));
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(rounded);
        };
        const auto effectiveLeftIndentTwips =
            signed_twips(format.left_indent_emu);
        const auto effectiveRightIndentTwips =
            signed_twips(format.right_indent_emu);
        const auto effectiveFirstLineIndentTwips =
            signed_twips(format.first_line_indent_emu);
        const auto effectiveSpaceBeforeTwips =
            unsigned_twips(format.space_before_emu);
        const auto effectiveSpaceAfterTwips =
            unsigned_twips(format.space_after_emu);
        const auto effectiveLineSpacing =
            unsigned_twips(format.line_spacing_emu);
        if (serializeParagraphProperty(
                format.left_indent_emu,
                inheritedParagraph.left_indent_emu, std::int64_t{0},
                paragraphOverrides.left_indent_emu)) {
            out.left_indent_twips = effectiveLeftIndentTwips.value_or(0);
        }
        if (serializeParagraphProperty(
                format.right_indent_emu,
                inheritedParagraph.right_indent_emu, std::int64_t{0},
                paragraphOverrides.right_indent_emu)) {
            out.right_indent_twips = effectiveRightIndentTwips.value_or(0);
        }
        if (serializeParagraphProperty(
                format.first_line_indent_emu,
                inheritedParagraph.first_line_indent_emu, std::int64_t{0},
                paragraphOverrides.first_line_indent_emu)) {
            out.first_line_indent_twips =
                effectiveFirstLineIndentTwips.value_or(0);
        }
        if (serializeParagraphProperty(
                format.space_before_emu,
                inheritedParagraph.space_before_emu, std::int64_t{0},
                paragraphOverrides.space_before_emu)) {
            out.space_before_twips = effectiveSpaceBeforeTwips.value_or(0);
        }
        if (serializeParagraphProperty(
                format.space_after_emu,
                inheritedParagraph.space_after_emu, std::int64_t{0},
                paragraphOverrides.space_after_emu)) {
            out.space_after_twips = effectiveSpaceAfterTwips.value_or(0);
        }

        constexpr std::int64_t kSingleLineSpacingEmu = 12 * 12'700;
        const bool serializeLineSpacing = serializeParagraphProperty(
            format.line_spacing_emu,
            inheritedParagraph.line_spacing_emu, kSingleLineSpacingEmu,
            paragraphOverrides.line_spacing_emu);
        const bool serializeLineSpacingRule = serializeParagraphProperty(
            format.line_spacing_rule,
            inheritedParagraph.line_spacing_rule,
            core::LineSpacingRule::automatic,
            paragraphOverrides.line_spacing_rule);
        // OOXML's lineRule qualifies one concrete line value, so emit the
        // pair together if either half is a direct/different property.
        if (serializeLineSpacing || serializeLineSpacingRule) {
            out.line_spacing = effectiveLineSpacing.value_or(240U);
            switch (format.line_spacing_rule.value_or(
                        core::LineSpacingRule::automatic)) {
                case core::LineSpacingRule::automatic:
                    out.line_spacing_rule = ooxml::BasicLineSpacingRule::automatic; break;
                case core::LineSpacingRule::at_least:
                    out.line_spacing_rule = ooxml::BasicLineSpacingRule::at_least; break;
                case core::LineSpacingRule::exact:
                    out.line_spacing_rule = ooxml::BasicLineSpacingRule::exact; break;
            }
        }
        if (serializeParagraphProperty(
                format.keep_with_next,
                inheritedParagraph.keep_with_next, false,
                paragraphOverrides.keep_with_next)) {
            out.keep_with_next = format.keep_with_next.value_or(false);
        }
        if (serializeParagraphProperty(
                format.keep_lines, inheritedParagraph.keep_lines, false,
                paragraphOverrides.keep_lines)) {
            out.keep_lines = format.keep_lines.value_or(false);
        }
        if (serializeParagraphProperty(
                format.page_break_before,
                inheritedParagraph.page_break_before, false,
                paragraphOverrides.page_break_before)) {
            out.page_break_before =
                format.page_break_before.value_or(false);
        }
        const auto paragraphMarkFormat = exportCharacterFormat(
            paragraph.paragraphMarkCharacterFormat(),
            paragraphMarkOverrides);
        if (!ooxmlRunFormatIsEmpty(paragraphMarkFormat)) {
            out.paragraph_mark_format = paragraphMarkFormat;
        }

        const auto& text = paragraph.text();
        const QString paragraphText = QString::fromUtf16(
            text.data(), static_cast<qsizetype>(text.size()));
        const auto marker = literalListMarker(paragraphText);
        std::optional<int> normalizedListIndent;
        if (marker && format.list_id && format.list_level &&
            format.list_layout) {
            const auto metric =
                listMetrics.find({*format.list_id, *format.list_level});
            if (metric != listMetrics.end() &&
                metric->second.spaceAdvance > 0.0) {
                const auto level = static_cast<std::size_t>(*format.list_level);
                const auto& levelLayout = format.list_layout->levels[level];
                const qreal bulletOffset =
                    static_cast<qreal>(levelLayout.bullet_indent_spaces) *
                    metric->second.spaceAdvance;
                const qreal textOffset =
                    bulletOffset + metric->second.maximumMarkerWidth +
                    static_cast<qreal>(levelLayout.text_indent_spaces) *
                        metric->second.spaceAdvance;
                const bool nativeNumberingLevel =
                    level < kNativeOoxmlListLevelCount;
                // A style-inherited indent is intentionally absent from pPr,
                // but list geometry is measured from the effective paragraph
                // position.  Do not let sparse style export shift list text.
                const std::int64_t baseLeft =
                    effectiveLeftIndentTwips.value_or(0);
                const std::int64_t left =
                    baseLeft + std::llround(textOffset * 20.0);
                const std::int64_t hanging = std::llround(
                    (nativeNumberingLevel
                         ? textOffset - bulletOffset
                         : textOffset) *
                    20.0);
                if (left > 0 && left <= 31'680 && hanging > 0 &&
                    hanging <= std::numeric_limits<std::int32_t>::max() &&
                    left <= std::numeric_limits<std::int32_t>::max()) {
                    out.left_indent_twips = static_cast<std::int32_t>(left);
                    out.first_line_indent_twips =
                        -static_cast<std::int32_t>(hanging);
                    out.left_tab_stops_twips = {
                        static_cast<std::uint32_t>(left)};
                    if (nativeNumberingLevel) {
                        normalizedListIndent =
                            levelLayout.bullet_indent_spaces;
                        ooxml::NewNumbering numbering;
                        numbering.num_id = nativeListIds.at(*format.list_id);
                        numbering.level = *format.list_level;
                        numbering.start = metric->second.start;
                        numbering.text_indent_twips =
                            static_cast<std::uint32_t>(left);
                        numbering.hanging_indent_twips =
                            static_cast<std::uint32_t>(hanging);
                        numbering.tab_stop_twips =
                            static_cast<std::uint32_t>(left);
                        if (marker->kind == LiteralListMarker::Kind::bullet) {
                            numbering.format =
                                ooxml::BasicNumberFormat::bullet;
                            numbering.level_text =
                                marker->marker.toUtf8().toStdString();
                        } else {
                            numbering.format =
                                ooxmlNumberFormatForLevel(level);
                            const QChar delimiter = marker->marker.isEmpty()
                                ? QLatin1Char('.')
                                : marker->marker.back();
                            numbering.level_text =
                                (QStringLiteral("%") +
                                 QString::number(level + 1U) + delimiter)
                                    .toUtf8()
                                    .toStdString();
                        }
                        out.numbering = std::move(numbering);
                        // Numbering-level geometry is authoritative in
                        // numbering.xml. Direct pPr ind/tabs would override it.
                        out.left_indent_twips.reset();
                        out.first_line_indent_twips.reset();
                        out.left_tab_stops_twips.clear();
                    }
                } else {
                    losses.push_back(QObject::tr(
                        "A list text stop exceeds the DOCX paragraph range"));
                }
            }
        }

        QString serializedText = paragraphText;
        std::vector<std::size_t> sourceOffsets;
        sourceOffsets.reserve(static_cast<std::size_t>(serializedText.size()));
        if (normalizedListIndent && marker && out.numbering) {
            serializedText.clear();
            const auto prefixLength =
                static_cast<std::size_t>(marker->prefixLength);
            serializedText.reserve(
                paragraphText.size() - marker->prefixLength);
            serializedText += paragraphText.mid(marker->prefixLength);
            for (std::size_t index = prefixLength; index < text.size();
                 ++index) {
                sourceOffsets.push_back(index);
            }
        } else {
            for (std::size_t index = 0; index < text.size(); ++index) {
                sourceOffsets.push_back(index);
            }
        }

        const auto appendImage = [&](const core::ImageAtom& image) {
            const auto bytes = image.encoded_payload.bytes();
            if (bytes.empty() || image.width_emu <= 0 ||
                image.height_emu <= 0 ||
                image.width_emu > core::kMaximumInlineImageDimensionEmu ||
                image.height_emu > core::kMaximumInlineImageDimensionEmu) {
                losses.push_back(QObject::tr(
                    "A picture lacks a valid PNG/JPEG source or display size"));
                return;
            }
            const QString accessibleName = QString::fromStdString(
                image.accessible_name);
            QString safeName;
            safeName.reserve(std::min<qsizetype>(
                accessibleName.size(), 120));
            for (const QChar character : accessibleName) {
                const ushort value = character.unicode();
                if (value < 0x20U || value == 0x7fU || value == 0xfffeU ||
                    value == 0xffffU || character == QLatin1Char('/') ||
                    character == QLatin1Char('\\')) {
                    continue;
                }
                safeName += character;
                if (safeName.size() >= 120) break;
            }
            if (safeName.trimmed().isEmpty()) {
                safeName = QObject::tr("Picture");
            }
            QByteArray encodedName = safeName.toUtf8();
            while (encodedName.size() > 240 && !safeName.isEmpty()) {
                safeName.chop(1);
                encodedName = safeName.toUtf8();
            }

            ooxml::NewInlineImage serialized;
            serialized.format = image.format == core::ImageFormat::png
                ? raster::Format::png
                : raster::Format::jpeg;
            serialized.name = encodedName.toStdString();
            serialized.width_emu = image.width_emu;
            serialized.height_emu = image.height_emu;
            serialized.bytes.assign(bytes.begin(), bytes.end());
            serialized.accessible_name = image.accessible_name;
            switch (image.layout.placement) {
                case core::ImagePlacement::inline_with_text:
                    serialized.layout.placement =
                        ooxml::ImagePlacement::inline_with_text;
                    break;
                case core::ImagePlacement::square:
                    serialized.layout.placement =
                        ooxml::ImagePlacement::square;
                    break;
                case core::ImagePlacement::top_and_bottom:
                    serialized.layout.placement =
                        ooxml::ImagePlacement::top_and_bottom;
                    break;
            }
            serialized.layout.distance_top_emu =
                image.layout.distance_top_emu;
            serialized.layout.distance_right_emu =
                image.layout.distance_right_emu;
            serialized.layout.distance_bottom_emu =
                image.layout.distance_bottom_emu;
            serialized.layout.distance_left_emu =
                image.layout.distance_left_emu;
            serialized.layout.move_with_text = image.layout.move_with_text;

            const auto formatOffset = std::min(
                paragraph.text().size(), image.utf16_offset + 1U);
            ooxml::NewRun imageRun;
            imageRun.format = exportCharacterFormat(
                paragraph.characterFormatAt(formatOffset),
                paragraph.styleOverrideMaskAt(formatOffset));
            imageRun.inline_image = std::move(serialized);
            out.runs.push_back(std::move(imageRun));
        };

        std::size_t cursor = 0;
        while (cursor < sourceOffsets.size()) {
            const std::size_t sourceOffset = sourceOffsets[cursor];
            if (const auto* equation = paragraph.equationAt(sourceOffset)) {
                out.runs.push_back({
                    {}, exportCharacterFormat(
                            paragraph.characterFormatAt(sourceOffset + 1),
                            paragraph.styleOverrideMaskAt(sourceOffset + 1)),
                    ooxml::EquationPayload{equation->canonical_latex,
                                           equation->display}});
                ++cursor;
                continue;
            }
            if (const auto* image = paragraph.imageAt(sourceOffset)) {
                appendImage(*image);
                ++cursor;
                continue;
            }
            const auto runFormat =
                paragraph.characterFormatAt(sourceOffset + 1);
            const auto serializedRunFormat = exportCharacterFormat(
                runFormat,
                paragraph.styleOverrideMaskAt(sourceOffset + 1));
            const std::size_t start = cursor;
            do {
                ++cursor;
            } while (cursor < sourceOffsets.size() &&
                     paragraph.equationAt(sourceOffsets[cursor]) == nullptr &&
                     paragraph.imageAt(sourceOffsets[cursor]) == nullptr &&
                     exportCharacterFormat(
                         paragraph.characterFormatAt(
                             sourceOffsets[cursor] + 1),
                         paragraph.styleOverrideMaskAt(
                             sourceOffsets[cursor] + 1)) ==
                         serializedRunFormat);
            out.runs.push_back({
                serializedText
                    .mid(static_cast<qsizetype>(start),
                         static_cast<qsizetype>(cursor - start))
                    .toUtf8().toStdString(),
                serializedRunFormat,
                std::nullopt});
        }
        if (out.runs.empty()) {
            out.runs.push_back({
                {}, styleCharacterBaseline
                        ? ooxml::BasicRunFormat{}
                        : toOoxmlFormat(
                              {}, defaultFontFamily,
                              defaultFontPointSize),
                std::nullopt});
        }
        output.push_back(std::move(out));
    }
    losses.removeDuplicates();
    return output;
}

ooxml::NewDocumentBody toOoxmlBody(
    const core::DocumentSnapshot& snapshot, QStringList& losses,
    const QString& defaultFontFamily, double defaultFontPointSize,
    const ImportedTableStyleSources& importedTableStyleSources) {
    auto paragraphs = toOoxmlParagraphs(
        snapshot, losses, defaultFontFamily, defaultFontPointSize);
    ooxml::NewDocumentBody output;
    output.blocks.reserve(snapshot.document.bodyBlocks().size());
    for (const auto& block : snapshot.document.bodyBlocks()) {
        if (block.kind == core::BodyBlockKind::paragraph) {
            const auto index = snapshot.document.paragraphIndex(block.id);
            if (!index || *index >= paragraphs.size()) {
                losses.push_back(QObject::tr(
                    "A body paragraph could not be serialized"));
                continue;
            }
            output.blocks.emplace_back(std::move(paragraphs[*index]));
            continue;
        }
        const auto* table = snapshot.document.findTable(block.id);
        if (!table) {
            losses.push_back(QObject::tr(
                "A body table could not be serialized"));
            continue;
        }
        ooxml::NewTable serialized;
        serialized.rows = table->rowCount();
        serialized.columns = table->columnCount();
        serialized.header_row = table->hasHeaderRow();
        serialized.style = table->style()
            ? std::optional<ooxml::BasicTableStyle>(
                  toOoxmlTableStyle(*table->style()))
            : std::nullopt;
        const auto sourceStyle = importedTableStyleSources.find(table->id());
        if (sourceStyle != importedTableStyleSources.end() &&
            !sourceStyle->second.first.has_value()) {
            const QString action = table->style().has_value()
                ? QObject::tr("will be replaced by the selected built-in style")
                : QObject::tr("will be omitted");
            losses.push_back(QObject::tr(
                "Custom table style \"%1\" is preserved in the original file, but its definition cannot be embedded in a regenerated DOCX and %2")
                .arg(fromUtf8(sourceStyle->second.second), action));
        }
        serialized.cell_paragraphs.reserve(table->cells().size());
        std::size_t cellIndex = 0;
        for (const auto& cell : table->cells()) {
            serialized.cell_paragraphs.push_back(
                toOoxmlTableCellParagraph(
                    cell, losses, defaultFontFamily,
                    defaultFontPointSize,
                    table->hasHeaderRow() &&
                        cellIndex / table->columnCount() == 0U));
            ++cellIndex;
        }
        output.blocks.emplace_back(std::move(serialized));
    }
    losses.removeDuplicates();
    return output;
}

std::optional<std::array<double, 4>> requestCustomMargins(
    QWidget* parent, const DocumentCanvas& canvas) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("customMarginsDialog"));
    dialog.setWindowTitle(QObject::tr("Custom Margins"));
    dialog.setModal(true);

    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    const auto addField = [&dialog, form](const QString& label,
                                         const QString& objectName,
                                         double points) {
        auto* field = new QDoubleSpinBox(&dialog);
        field->setObjectName(objectName);
        field->setRange(0.0, 10.0);
        field->setDecimals(2);
        field->setSingleStep(0.1);
        field->setSuffix(QObject::tr(" in"));
        field->setValue(points / 72.0);
        form->addRow(label, field);
        return field;
    };
    auto* top = addField(QObject::tr("Top:"),
                         QStringLiteral("customMargins.top"),
                         canvas.marginTopPoints());
    auto* bottom = addField(QObject::tr("Bottom:"),
                            QStringLiteral("customMargins.bottom"),
                            canvas.marginBottomPoints());
    auto* left = addField(QObject::tr("Left:"),
                          QStringLiteral("customMargins.left"),
                          canvas.marginLeftPoints());
    auto* right = addField(QObject::tr("Right:"),
                           QStringLiteral("customMargins.right"),
                           canvas.marginRightPoints());
    outer->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("customMargins.buttons"));
    outer->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const double vertical = (top->value() + bottom->value()) * 72.0;
        const double horizontal = (left->value() + right->value()) * 72.0;
        if (vertical >= canvas.pageHeightPoints() - 36.0 ||
            horizontal >= canvas.pageWidthPoints() - 36.0) {
            QMessageBox::warning(
                &dialog, QObject::tr("Margins are too large"),
                QObject::tr("Leave at least half an inch of printable page "
                            "between opposing margins."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return std::array<double, 4>{top->value() * 72.0,
                                 right->value() * 72.0,
                                 bottom->value() * 72.0,
                                 left->value() * 72.0};
}

std::optional<EditorPreferences> requestEditorPreferences(
    QWidget* parent, const EditorPreferences& current) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("editorOptionsDialog"));
    dialog.setWindowTitle(QObject::tr("Editor Options"));
    dialog.setModal(true);

    auto* outer = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        QObject::tr("These defaults apply to unformatted text in every open "
                    "document and to documents you create next."),
        &dialog);
    explanation->setWordWrap(true);
    outer->addWidget(explanation);

    auto* form = new QFormLayout;
    auto* fontFamily = new FontFamilyPicker(&dialog);
    fontFamily->setObjectName(QStringLiteral("editorOptions.fontFamily"));
    fontFamily->setAccessibleName(QObject::tr("Default font"));
    fontFamily->setCurrentText(current.defaultFontFamily());
    form->addRow(QObject::tr("Default font:"), fontFamily);

    auto* fontSize = new QDoubleSpinBox(&dialog);
    fontSize->setObjectName(QStringLiteral("editorOptions.fontSize"));
    fontSize->setAccessibleName(QObject::tr("Default font size"));
    fontSize->setRange(EditorPreferences::kMinimumFontPointSize,
                       EditorPreferences::kMaximumFontPointSize);
    fontSize->setDecimals(1);
    fontSize->setSingleStep(0.5);
    fontSize->setSuffix(QObject::tr(" pt"));
    fontSize->setValue(current.defaultFontPointSize());
    form->addRow(QObject::tr("Default size:"), fontSize);

    auto* tabWidth = new QSpinBox(&dialog);
    tabWidth->setObjectName(QStringLiteral("editorOptions.tabWidth"));
    tabWidth->setAccessibleName(QObject::tr("Tab size in spaces"));
    tabWidth->setRange(EditorPreferences::kMinimumTabWidthSpaces,
                       EditorPreferences::kMaximumTabWidthSpaces);
    tabWidth->setSuffix(QObject::tr(" spaces"));
    tabWidth->setValue(current.tabWidthSpaces());
    form->addRow(QObject::tr("Tab size:"), tabWidth);
    outer->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("editorOptions.buttons"));
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    outer->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return std::nullopt;

    EditorPreferences result = current;
    if (!result.setDefaultFontFamily(fontFamily->currentText()) ||
        !result.setDefaultFontPointSize(fontSize->value()) ||
        !result.setTabWidthSpaces(tabWidth->value())) {
        return std::nullopt;
    }
    return result;
}

ListProperties listPropertiesFromLayout(const core::ListLayout& layout) {
    ListProperties properties{};
    for (std::size_t level = 0; level < core::kListLevelCount; ++level) {
        properties[level].bulletPositionSpaces =
            layout.levels[level].bullet_indent_spaces;
        properties[level].textGapAfterBulletSpaces =
            layout.levels[level].text_indent_spaces;
    }
    return properties;
}

core::ListLayout listLayoutFromProperties(const ListProperties& properties) {
    core::ListLayout layout;
    for (std::size_t level = 0; level < core::kListLevelCount; ++level) {
        layout.levels[level].bullet_indent_spaces =
            properties[level].bulletPositionSpaces;
        layout.levels[level].text_indent_spaces =
            properties[level].textGapAfterBulletSpaces;
    }
    return layout;
}

QString saveError(const std::optional<ooxml::Error>& error) {
    return error ? fromUtf8(error->message) : QObject::tr("Unknown DOCX error");
}

QString compatibilityClassName(ooxml::CompatibilityClass value) {
    switch (value) {
        case ooxml::CompatibilityClass::exact_round_trip_only: return QObject::tr("Preserved, view-only");
        case ooxml::CompatibilityClass::safe_text_patch: return QObject::tr("Safe text editing");
        case ooxml::CompatibilityClass::basic_body_text_patch: return QObject::tr("Basic body text editing");
        case ooxml::CompatibilityClass::invalid: return QObject::tr("Invalid");
    }
    return QObject::tr("Unknown");
}

}  // namespace

struct MainWindow::TabState {
    QString documentKey;
    QString path;
    QString recoverySourcePath;
    QString recoveryDisplayName;
    QString journalKey;
    std::unique_ptr<ooxml::DocxDocument> package;
    std::optional<FileFingerprint> diskFingerprint;
    QStringList baselineTexts;
    std::vector<std::size_t> sourceParagraphIndices;
    QStringList sourceTextPrefixes;
    ImportedTableStyleSources importedTableStyleSources;
    bool regeneratable{true};
    // When an existing package is eligible for full regeneration, retain the
    // document defaults that were actually serialized into that package.
    // Application preference changes update canvas insertion defaults, but
    // must not silently rewrite an already-open document's Normal style or
    // default tab stop on the next formatting/structural save.
    std::optional<ooxml::DocumentDefaults> regenerationDefaults;
    QString navigationQuery;
    QString navigationReplacement;
    bool navigationMatchCase{false};
    bool navigationWholeWords{false};
    bool navigationReplaceMode{false};
    std::vector<DocumentSearchHit> navigationHits;
    bool recovered{false};
    bool replaceOnSuccessfulOpen{false};
    bool simplificationWarningAcknowledged{false};
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), commands_(this) {
    setWindowTitle(tr("Owl Docs"));
    setWindowIcon(owlDocsApplicationIcon());
    resize(1280, 860);
    setMinimumSize(820, 600);
    setAcceptDrops(true);
    {
        const QSettings settings;
        editorPreferences_ = EditorPreferences::load(settings);
    }
    figureEditor_ = std::make_unique<ExcalidrawFigureEditor>(this);
    registerCommands();
    buildMenus();

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    ribbon_ = new RibbonWidget(commands_, central);
    tabs_ = new QTabWidget(central);
    tabs_->setObjectName(QStringLiteral("documentTabs"));
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    layout->addWidget(ribbon_);
    layout->addWidget(tabs_, 1);
    setCentralWidget(central);
    connectRibbon();

    navigationSearchDebounce_ = new QTimer(this);
    navigationSearchDebounce_->setSingleShot(true);
    navigationSearchDebounce_->setInterval(120);
    connect(navigationSearchDebounce_, &QTimer::timeout, this,
            &MainWindow::refreshNavigationResults);

    navigation_ = new NavigationDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, navigation_);
    navigation_->hide();
    auto* navigationAction = commands_.action(QStringLiteral("view.navigation"));
    connect(navigation_, &QDockWidget::visibilityChanged, navigationAction,
            [navigationAction](bool visible) {
        const QSignalBlocker blocker(navigationAction);
        navigationAction->setChecked(visible);
    });
    connect(navigation_, &NavigationDock::queryChanged, this,
            [this](const QString& query, bool matchCase, bool wholeWords) {
        if (auto* state = activeState()) {
            state->navigationQuery = query;
            state->navigationMatchCase = matchCase;
            state->navigationWholeWords = wholeWords;
            state->navigationHits.clear();
        }
        navigationSearchDebounce_->start();
    });
    connect(navigation_, &NavigationDock::replacementChanged, this,
            [this](const QString& replacement) {
        if (auto* state = activeState()) {
            state->navigationReplacement = replacement;
        }
    });
    connect(navigation_, &NavigationDock::modeChanged, this,
            [this](NavigationDock::Mode mode) {
        if (auto* state = activeState()) {
            state->navigationReplaceMode = mode == NavigationDock::Mode::replace;
        }
    });
    connect(navigation_, &NavigationDock::nextRequested, this,
            [this] { navigateSearchResult(true); });
    connect(navigation_, &NavigationDock::previousRequested, this,
            [this] { navigateSearchResult(false); });
    connect(navigation_, &NavigationDock::resultActivated, this,
            &MainWindow::activateNavigationResult);
    connect(navigation_, &NavigationDock::replaceRequested, this,
            [this](const QString&, const QString& replacement, bool, bool) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (canvas->hasPreview()) {
            statusBar()->showMessage(
                tr("Accept or discard the Codex preview before replacing text."),
                5000);
            return;
        }
        const int index = navigation_->currentResultIndex();
        const auto options = DocumentSearchOptions{
            navigation_->matchCase(), navigation_->wholeWords()};
        const auto hits = canvas->searchHits(navigation_->query(), options);
        if (index < 0 || static_cast<std::size_t>(index) >= hits.size() ||
            !canvas->replaceSearchHit(
                hits[static_cast<std::size_t>(index)], replacement)) {
            statusBar()->showMessage(tr("The selected match is no longer available."),
                                     3500);
            refreshNavigationResults();
            return;
        }
        // Search from the resulting caret, not from the old presentation row:
        // replacements that still contain the query must advance, and
        // replacing the final match must wrap to the first remaining match.
        static_cast<void>(canvas->findNextHit(
            navigation_->query(), options));
        refreshNavigationResults();
    });
    connect(navigation_, &NavigationDock::replaceAllRequested, this,
            [this](const QString& query, const QString& replacement,
                   bool matchCase, bool wholeWords) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (canvas->hasPreview()) {
            statusBar()->showMessage(
                tr("Accept or discard the Codex preview before replacing text."),
                5000);
            return;
        }
        const int count = canvas->replaceAllMatches(
            query, replacement,
            DocumentSearchOptions{matchCase, wholeWords});
        statusBar()->showMessage(
            count == 1 ? tr("Replaced 1 match")
                       : tr("Replaced %1 matches").arg(count),
            4000);
        refreshNavigationResults();
    });
    connect(navigation_, &NavigationDock::dismissRequested, this, [this] {
        if (auto* canvas = activeCanvas()) {
            canvas->setFocus(Qt::ShortcutFocusReason);
        }
    });

    chat_ = new ChatDock(this);
    addDockWidget(Qt::RightDockWidgetArea, chat_);
    chat_->hide();
    codex_ = new CodexController(this);
    codex_->setEditorToolHandler(
        [this](const QString& key, const QString& tool,
               const codex::Json& arguments, QString& error) {
            return handleEditorTool(key, tool, arguments, error);
        });
    connect(chat_, &ChatDock::enableRequested, codex_, &CodexController::enable);
    connect(chat_, &ChatDock::modelChanged, codex_, &CodexController::selectModel);
    connect(chat_, &ChatDock::sendRequested, this,
            [this](const QString& text, const QString& model, const QString& effort, const QString& tier) {
                const auto* canvas = activeCanvas();
                if (!canvas) return;
                const auto key = documentKey();
                std::string ignored;
                static_cast<void>(chatStore_.appendMessage(
                    key.toStdString(), "user", text.toStdString(), ignored));
                if (const auto remembered = chatStore_.threadForDocument(key.toStdString())) {
                    codex_->restoreDocumentThread(key,
                        QString::fromStdString(*remembered));
                }
                codex_->sendMessage(text, model, effort, tier, key,
                                    canvas->selectedText(), canvas->outlineText());
            });
    connect(codex_, &CodexController::statusChanged, chat_, &ChatDock::setConnected);
    connect(codex_, &CodexController::modelsChanged, chat_, &ChatDock::setModels);
    connect(codex_, &CodexController::effortsChanged, chat_, &ChatDock::setEfforts);
    connect(codex_, &CodexController::serviceTiersChanged, chat_, &ChatDock::setServiceTiers);
    connect(codex_, &CodexController::busyChanged, chat_, &ChatDock::setBusy);
    connect(codex_, &CodexController::assistantStarted, this, [this](const QString& key) {
        assistantDocumentKey_ = key;
        assistantForStore_.clear();
        if (key == documentKey()) chat_->beginAssistantMessage();
    });
    connect(codex_, &CodexController::assistantDelta, this,
            [this](const QString& key, const QString& delta) {
        if (key != assistantDocumentKey_) return;
        assistantForStore_ += delta;
        if (key == documentKey()) chat_->appendAssistantDelta(delta);
    });
    connect(codex_, &CodexController::assistantFinished, this, [this](const QString& key) {
        if (key == documentKey()) chat_->finishAssistantMessage();
        std::string ignored;
        chatStore_.appendMessage(key.toStdString(), "assistant",
                                 assistantForStore_.toStdString(), ignored);
        assistantDocumentKey_.clear();
    });
    connect(codex_, &CodexController::errorOccurred, chat_, &ChatDock::showError);
    connect(codex_, &CodexController::threadCreated, this,
            [this](const QString& key, const QString& thread) {
        std::string ignored;
        chatStore_.bindThread(key.toStdString(), thread.toStdString(), ignored);
    });
    connect(chat_, &ChatDock::previewLastResponseRequested, this, [this](const QString& response) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        const QString key = documentKeyFor(canvas);
        if (!previewDocumentKey_.isEmpty() && previewDocumentKey_ != key) {
            QMessageBox::information(
                this, tr("Preview already awaiting review"),
                tr("Accept or discard the preview in the other document before "
                   "creating another one."));
            return;
        }
        QString summary;
        QString error;
        if (!canvas->createReplacementPreview(response, summary, error)) {
            QMessageBox::warning(this, tr("Could not create preview"), error);
            return;
        }
        previewDocumentKey_ = key;
        previewLabel_ = tr("Codex replacement preview");
        previewSummary_ = summary;
        chat_->showPreview(previewLabel_, previewSummary_);
    });
    connect(chat_, &ChatDock::acceptPreviewRequested, this, [this] {
        auto* canvas = canvasForDocumentKey(previewDocumentKey_);
        if (!canvas) return;
        QString error;
        if (!canvas->acceptPreview(error)) {
            QMessageBox::warning(this, tr("Preview conflict"), error);
            return;
        }
        previewDocumentKey_.clear();
        previewLabel_.clear();
        previewSummary_.clear();
        chat_->hidePreview();
    });
    connect(chat_, &ChatDock::rejectPreviewRequested, this, [this] {
        if (auto* canvas = canvasForDocumentKey(previewDocumentKey_))
            canvas->discardPreview();
        previewDocumentKey_.clear();
        previewLabel_.clear();
        previewSummary_.clear();
        chat_->hidePreview();
    });

    pageStatus_ = new QLabel(this);
    saveStatus_ = new QLabel(tr("Ready"), this);
    statusBar()->addPermanentWidget(saveStatus_);
    statusBar()->addPermanentWidget(pageStatus_);

    auto* zoomControls = new QWidget(this);
    zoomControls->setObjectName(QStringLiteral("status.zoomControls"));
    auto* zoomLayout = new QHBoxLayout(zoomControls);
    zoomLayout->setContentsMargins(6, 0, 0, 0);
    zoomLayout->setSpacing(4);

    zoomOut_ = new QToolButton(zoomControls);
    zoomOut_->setObjectName(QStringLiteral("status.zoomOut"));
    zoomOut_->setAutoRaise(true);
    zoomOut_->setFocusPolicy(Qt::NoFocus);
    zoomOut_->setAccessibleName(tr("Zoom out"));
    zoomOut_->setToolTip(tr("Zoom out"));
    zoomOut_->setIcon(QIcon::fromTheme(QStringLiteral("zoom-out")));
    if (zoomOut_->icon().isNull()) zoomOut_->setText(QStringLiteral("−"));

    zoomSlider_ = new QSlider(Qt::Horizontal, zoomControls);
    zoomSlider_->setObjectName(QStringLiteral("status.zoomSlider"));
    zoomSlider_->setAccessibleName(tr("Document zoom"));
    zoomSlider_->setAccessibleDescription(
        tr("Zoom from 25 to 400 percent; hold Control and scroll over the document"));
    zoomSlider_->setToolTip(zoomSlider_->accessibleDescription());
    zoomSlider_->setRange(DocumentCanvas::kMinimumZoomPercent,
                          DocumentCanvas::kMaximumZoomPercent);
    zoomSlider_->setSingleStep(5);
    zoomSlider_->setPageStep(10);
    zoomSlider_->setTracking(false);
    zoomSlider_->setFixedWidth(140);
    zoomSlider_->setValue(100);

    zoomIn_ = new QToolButton(zoomControls);
    zoomIn_->setObjectName(QStringLiteral("status.zoomIn"));
    zoomIn_->setAutoRaise(true);
    zoomIn_->setFocusPolicy(Qt::NoFocus);
    zoomIn_->setAccessibleName(tr("Zoom in"));
    zoomIn_->setToolTip(tr("Zoom in"));
    zoomIn_->setIcon(QIcon::fromTheme(QStringLiteral("zoom-in")));
    if (zoomIn_->icon().isNull()) zoomIn_->setText(QStringLiteral("+"));

    zoomStatus_ = new QLabel(QStringLiteral("100%"), zoomControls);
    zoomStatus_->setObjectName(QStringLiteral("status.zoomPercent"));
    zoomStatus_->setAccessibleName(tr("Current document zoom"));
    zoomStatus_->setMinimumWidth(44);
    zoomStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    zoomLayout->addWidget(zoomOut_);
    zoomLayout->addWidget(zoomSlider_);
    zoomLayout->addWidget(zoomIn_);
    zoomLayout->addWidget(zoomStatus_);
    statusBar()->addPermanentWidget(zoomControls);

    connect(zoomOut_, &QToolButton::clicked, this, [this] {
        if (auto* canvas = activeCanvas()) {
            setActiveZoomPercent(canvas->zoomPercent() - 10);
        }
    });
    connect(zoomIn_, &QToolButton::clicked, this, [this] {
        if (auto* canvas = activeCanvas()) {
            setActiveZoomPercent(canvas->zoomPercent() + 10);
        }
    });
    connect(zoomSlider_, &QSlider::sliderMoved, this, [this](int percent) {
        zoomStatus_->setText(tr("%1%").arg(percent));
    });
    connect(zoomSlider_, &QSlider::valueChanged, this,
            [this](int percent) { setActiveZoomPercent(percent); });

    const auto stateDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDir);
    QFile::setPermissions(
        stateDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                      QFileDevice::ExeOwner);
    instanceLock_ = std::make_unique<QLockFile>(
        stateDir + QStringLiteral("/instance.lock"));
    recoveryOwner_ = instanceLock_->tryLock(0);
    std::string storeError;
    if (!chatStore_.open(QFile::encodeName(stateDir + QStringLiteral("/state.sqlite3")).toStdString(), storeError)) {
        statusBar()->showMessage(tr("Local chat history disabled: %1").arg(fromUtf8(storeError)), 8000);
    } else if (!recoveryOwner_) {
        statusBar()->showMessage(
            tr("Autosave recovery is owned by another running Owl Docs instance."),
            8000);
    }

    recoveryDebounce_ = new QTimer(this);
    recoveryDebounce_->setSingleShot(true);
    recoveryDebounce_->setInterval(1500);
    connect(recoveryDebounce_, &QTimer::timeout, this,
            &MainWindow::checkpointModifiedDocuments);
    recoveryPeriodic_ = new QTimer(this);
    recoveryPeriodic_->setInterval(30000);
    connect(recoveryPeriodic_, &QTimer::timeout, this,
            &MainWindow::checkpointModifiedDocuments);
    recoveryPeriodic_->start();

    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        // A modeless color adjustment belongs to the document that opened it.
        // Close it before changing the active tab so a later color click can
        // never format a different document behind the user's back.
        if (textColorPicker_) delete textColorPicker_.data();
        if (highlightColorPicker_) delete highlightColorPicker_.data();
        updateWindowTitle();
        loadChatForActiveDocument();
        loadNavigationForActiveDocument();
        if (auto* canvas = activeCanvas()) {
            synchronizeZoomControls(canvas);
            ribbon_->setParagraphStyle(canvas->currentParagraphStyleId());
            synchronizeParagraphStyleAvailability(canvas);
            ribbon_->setFontFamily(canvas->currentFontFamily());
            ribbon_->setFontPointSize(canvas->currentFontPointSize());
            ribbon_->setTextColor(canvas->currentTextColor());
            ribbon_->setTableContext(canvas->selectedTableId().has_value());
            const auto pictureLayout = canvas->selectedImageLayout();
            ribbon_->setPictureContext(
                pictureLayout.has_value(),
                pictureLayout.value_or(core::ImageLayout{}).placement);
            canvas->refreshCursorFormat();
        } else {
            synchronizeZoomControls(nullptr);
            ribbon_->setParagraphStyle({});
            synchronizeParagraphStyleAvailability(nullptr);
            ribbon_->setListContext(false, 1);
            ribbon_->setTableContext(false);
            ribbon_->setPictureContext(false);
        }
    });
    newDocument(true);
    if (recoveryOwner_)
        QTimer::singleShot(0, this, &MainWindow::restoreRecoveryJournals);
}

MainWindow::~MainWindow() {
    // QColorDialog is a window child, so Qt may otherwise destroy it after the
    // central widget and tab bar. Its cleanup callback needs those objects
    // while ending the originating canvas's color adjustment.
    if (textColorPicker_) delete textColorPicker_.data();
    if (highlightColorPicker_) delete highlightColorPicker_.data();
}

void MainWindow::registerCommands() {
    auto add = [this](const QString& id, const QString& text, const QKeySequence& shortcut,
                      std::function<void()> handler, bool checkable = false) {
        return commands_.add(id, text, shortcut, std::move(handler), checkable);
    };
    add("file.new", tr("New"), QKeySequence::New, [this] { newDocument(); });
    add("file.open", tr("Open…"), QKeySequence::Open, [this] { openDocument(); });
    add("file.save", tr("Save"), QKeySequence::Save, [this] { saveDocument(false); });
    add("file.saveAs", tr("Save As…"), QKeySequence::SaveAs, [this] { saveDocument(true); });
    add("file.exportPdf", tr("Export PDF…"), QKeySequence(), [this] { exportPdf(); });
    add("file.printPreview", tr("Print Preview…"),
        QKeySequence(QStringLiteral("Ctrl+F2")),
        [this] { printPreview(); });
    add("file.print", tr("Print…"), QKeySequence::Print, [this] { printDocument(); });
    add("file.options", tr("Options…"), QKeySequence(),
        [this] { showEditorOptions(); });
    add("edit.undo", tr("Undo"), QKeySequence::Undo, [this] { if (activeCanvas()) activeCanvas()->undo(); });
    add("edit.redo", tr("Redo"), QKeySequence::Redo, [this] { if (activeCanvas()) activeCanvas()->redo(); });
    add("edit.cut", tr("Cut"), QKeySequence::Cut, [this] { if (activeCanvas()) activeCanvas()->cut(); });
    add("edit.copy", tr("Copy"), QKeySequence::Copy, [this] { if (activeCanvas()) activeCanvas()->copy(); });
    add("edit.paste", tr("Paste"), QKeySequence::Paste, [this] { if (activeCanvas()) activeCanvas()->paste(); });
    add("edit.pasteTextOnly", tr("Paste as Text Only"),
        QKeySequence(QStringLiteral("Ctrl+Shift+V")),
        [this] {
            if (activeCanvas()) activeCanvas()->pasteTextOnly();
        });
    add("edit.find", tr("Find"), QKeySequence::Find,
        [this] { showFindReplace(false); });
    add("edit.replace", tr("Replace"), QKeySequence::Replace,
        [this] { showFindReplace(true); });
    add("edit.findNext", tr("Find Next"),
        QKeySequence(QStringLiteral("F3")),
        [this] { navigateSearchResult(true); });
    add("edit.findPrevious", tr("Find Previous"),
        QKeySequence(QStringLiteral("Shift+F3")),
        [this] { navigateSearchResult(false); });
    add("edit.commandPalette", tr("Command Palette…"), QKeySequence(QStringLiteral("Ctrl+Shift+P")),
        [this] { showCommandPalette(); });
    add("format.bold", tr("Bold"), QKeySequence::Bold, [this] { if (activeCanvas()) activeCanvas()->toggleBold(); }, true);
    add("format.italic", tr("Italic"), QKeySequence::Italic, [this] { if (activeCanvas()) activeCanvas()->toggleItalic(); }, true);
    add("format.underline", tr("Underline"), QKeySequence::Underline, [this] { if (activeCanvas()) activeCanvas()->toggleUnderline(); }, true);
    add("format.strike", tr("Strike"), QKeySequence(), [this] { if (activeCanvas()) activeCanvas()->toggleStrike(); }, true);
    add("format.superscript", tr("Superscript"), QKeySequence(QStringLiteral("Ctrl+Shift+=")),
        [this] { if (activeCanvas()) activeCanvas()->toggleBaseline(core::BaselinePosition::superscript); }, true);
    add("format.subscript", tr("Subscript"), QKeySequence(QStringLiteral("Ctrl+=")),
        [this] { if (activeCanvas()) activeCanvas()->toggleBaseline(core::BaselinePosition::subscript); }, true);
    const auto applyParagraphStyle = [this](const char* styleId) {
        if (auto* canvas = activeCanvas()) {
            canvas->applyParagraphStyle(QString::fromLatin1(styleId));
        }
    };
    add("style.normal", tr("Normal"),
        QKeySequence(QStringLiteral("Ctrl+Shift+N")),
        [applyParagraphStyle] { applyParagraphStyle("Normal"); });
    add("style.noSpacing", tr("No Spacing"), QKeySequence(),
        [applyParagraphStyle] { applyParagraphStyle("NoSpacing"); });
    add("style.title", tr("Title"), QKeySequence(),
        [applyParagraphStyle] { applyParagraphStyle("Title"); });
    add("style.subtitle", tr("Subtitle"), QKeySequence(),
        [applyParagraphStyle] { applyParagraphStyle("Subtitle"); });
    add("style.quote", tr("Quote"), QKeySequence(),
        [applyParagraphStyle] { applyParagraphStyle("Quote"); });
    for (int level = 1; level <= 9; ++level) {
        const QString id = QStringLiteral("style.heading%1").arg(level);
        const QString name = tr("Heading %1").arg(level);
        const QKeySequence shortcut = level <= 3
            ? QKeySequence(QStringLiteral("Ctrl+Alt+%1").arg(level))
            : QKeySequence{};
        add(id, name, shortcut, [this, level] {
            if (auto* canvas = activeCanvas()) {
                canvas->applyParagraphStyle(
                    QStringLiteral("Heading%1").arg(level));
            }
        });
    }
    add("paragraph.alignLeft", tr("Align Left"), QKeySequence(QStringLiteral("Ctrl+L")),
        [this] { if (activeCanvas()) activeCanvas()->setAlignment(core::ParagraphAlignment::left); });
    add("paragraph.alignCenter", tr("Center"), QKeySequence(QStringLiteral("Ctrl+E")),
        [this] { if (activeCanvas()) activeCanvas()->setAlignment(core::ParagraphAlignment::center); });
    add("paragraph.alignRight", tr("Align Right"), QKeySequence(QStringLiteral("Ctrl+R")),
        [this] { if (activeCanvas()) activeCanvas()->setAlignment(core::ParagraphAlignment::right); });
    add("paragraph.justify", tr("Justify"), QKeySequence(QStringLiteral("Ctrl+J")),
        [this] { if (activeCanvas()) activeCanvas()->setAlignment(core::ParagraphAlignment::justified); });
    add("paragraph.bullets", tr("Bullets"), QKeySequence(QStringLiteral("Ctrl+Shift+L")),
        [this] {
            if (auto* canvas = activeCanvas()) {
                canvas->toggleBullets();
                canvas->window()->activateWindow();
                canvas->setFocus(Qt::ShortcutFocusReason);
            }
        }, true);
    add("paragraph.numbering", tr("Numbering"), QKeySequence(),
        [this] {
            if (auto* canvas = activeCanvas()) {
                canvas->toggleNumbering();
                canvas->window()->activateWindow();
                canvas->setFocus(Qt::ShortcutFocusReason);
            }
        }, true);
    add("paragraph.listProperties", tr("List Properties…"), QKeySequence(),
        [this] { showListProperties(); });
    add("paragraph.decreaseIndent", tr("Decrease Indent"), QKeySequence(), [this] {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (canvas->hasActiveList()) {
            static_cast<void>(canvas->changeListLevel(true));
            return;
        }
        core::ParagraphFormatDelta d;
        d.left_indent_emu = core::PropertyDelta<std::int64_t>::set(0);
        canvas->applyParagraphFormat(d);
    });
    add("paragraph.increaseIndent", tr("Increase Indent"), QKeySequence(), [this] {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (canvas->hasActiveList()) {
            static_cast<void>(canvas->changeListLevel(false));
            return;
        }
        core::ParagraphFormatDelta d;
        d.left_indent_emu = core::PropertyDelta<std::int64_t>::set(457200);
        canvas->applyParagraphFormat(d);
    });
    add("insert.pageBreak", tr("Page Break"), QKeySequence(QStringLiteral("Ctrl+Return")),
        [this] { if (activeCanvas()) activeCanvas()->insertPageBreak(); });
    add("insert.table", tr("Table"), QKeySequence(), [this] { insertTable(); });
    add("table.insertRowAbove", tr("Insert Row Above"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->insertTableRow(false));
        }
    });
    add("table.insertRowBelow", tr("Insert Row Below"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->insertTableRow(true));
        }
    });
    add("table.insertColumnLeft", tr("Insert Column Left"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->insertTableColumn(false));
        }
    });
    add("table.insertColumnRight", tr("Insert Column Right"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->insertTableColumn(true));
        }
    });
    add("table.deleteRows", tr("Delete Rows"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->deleteSelectedTableRows());
        }
    });
    add("table.deleteColumns", tr("Delete Columns"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            static_cast<void>(canvas->deleteSelectedTableColumns());
        }
    });
    add("insert.image", tr("Picture"), QKeySequence(), [this] { insertImage(); });
    add("insert.excalidraw", tr("Excalidraw Figure"), QKeySequence(),
        [this] { insertExcalidrawFigure(); });
    add("picture.size", tr("Picture Size…"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            canvas->showSelectedImageSizeDialog();
        }
    });
    add("picture.editExcalidraw", tr("Edit Figure…"), QKeySequence(),
        [this] {
            editSelectedExcalidrawFigure(activeCanvas());
        });
    add("picture.altText", tr("Alt Text…"), QKeySequence(), [this] {
        if (auto* canvas = activeCanvas()) {
            canvas->showSelectedImageAltTextDialog();
        }
    });
    add("picture.layoutOptions", tr("Layout Options…"), QKeySequence(),
        [this] {
            if (auto* canvas = activeCanvas()) {
                canvas->showSelectedImageLayoutDialog();
            }
        });
    const auto setPicturePlacement = [this](core::ImagePlacement placement) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        auto layout = canvas->selectedImageLayout();
        if (!layout) return;
        layout->placement = placement;
        if (placement == core::ImagePlacement::inline_with_text) {
            layout->move_with_text = true;
        }
        static_cast<void>(canvas->setSelectedImageLayout(*layout));
    };
    auto* wrapInline = add(
        "picture.wrapInline", tr("In Line with Text"), QKeySequence(),
        [setPicturePlacement] {
            setPicturePlacement(core::ImagePlacement::inline_with_text);
        }, true);
    auto* wrapSquare = add(
        "picture.wrapSquare", tr("Square"), QKeySequence(),
        [setPicturePlacement] {
            setPicturePlacement(core::ImagePlacement::square);
        }, true);
    auto* wrapTopBottom = add(
        "picture.wrapTopBottom", tr("Top and Bottom"), QKeySequence(),
        [setPicturePlacement] {
            setPicturePlacement(core::ImagePlacement::top_and_bottom);
        }, true);
    auto* pictureWrapGroup = new QActionGroup(this);
    pictureWrapGroup->setExclusive(true);
    pictureWrapGroup->addAction(wrapInline);
    pictureWrapGroup->addAction(wrapSquare);
    pictureWrapGroup->addAction(wrapTopBottom);
    add("picture.delete", tr("Delete Picture"), QKeySequence(),
        [this] {
            if (auto* canvas = activeCanvas()) {
                static_cast<void>(canvas->deleteSelectedInlineImage());
            }
        });
    add("insert.equation", tr("Equation"), QKeySequence(QStringLiteral("Alt+=")), [this] { insertEquation(); });
    add("insert.textBox", tr("Text Box"), QKeySequence(), [this] { statusBar()->showMessage(tr("Text boxes are preserved on import; editing is not in this build."), 5000); });
    add("insert.comment", tr("Comment"), QKeySequence(QStringLiteral("Ctrl+Alt+M")), [this] { statusBar()->showMessage(tr("Comments are scheduled for the business/legal milestone."), 5000); });
    add("layout.orientation", tr("Orientation"), QKeySequence(), [this] { if (activeCanvas()) activeCanvas()->toggleOrientation(); });
    add("layout.columns", tr("Columns"), QKeySequence(), [this] { statusBar()->showMessage(tr("Multi-column authoring is not in this build."), 5000); });
    add("layout.lineSpacing", tr("Line Spacing"), QKeySequence(), [this] {
        if (!activeCanvas()) return;
        bool ok = false;
        const double value = QInputDialog::getDouble(this, tr("Line spacing"), tr("Multiple:"), 1.0, 0.5, 5.0, 2, &ok);
        if (ok) {
            core::ParagraphFormatDelta d;
            d.line_spacing_emu = core::PropertyDelta<std::int64_t>::set(
                static_cast<std::int64_t>(value * 12.0 * 12700.0));
            d.line_spacing_rule =
                core::PropertyDelta<core::LineSpacingRule>::set(
                    core::LineSpacingRule::automatic);
            activeCanvas()->applyParagraphFormat(d);
        }
    });
    add("layout.paragraphSpacing", tr("Paragraph Spacing"), QKeySequence(), [this] {
        if (!activeCanvas()) return;
        bool ok = false;
        const double value = QInputDialog::getDouble(this, tr("Paragraph spacing"), tr("After (pt):"), 8, 0, 144, 1, &ok);
        if (ok) { core::ParagraphFormatDelta d; d.space_after_emu = core::PropertyDelta<std::int64_t>::set(static_cast<std::int64_t>(value * 12700.0)); activeCanvas()->applyParagraphFormat(d); }
    });
    add("review.spelling", tr("Spelling"), QKeySequence(QStringLiteral("F7")), [this] { statusBar()->showMessage(tr("Misspellings are underlined; right-click one for suggestions."), 5000); });
    add("review.comment", tr("New Comment"), QKeySequence(), [this] { statusBar()->showMessage(tr("Comments are not editable in this build."), 5000); });
    add("review.trackChanges", tr("Track Changes"), QKeySequence(), [this] { statusBar()->showMessage(tr("Tracked changes are preserved but not editable in this build."), 5000); }, true);
    add("review.acceptChange", tr("Accept"), QKeySequence(), [] {});
    add("review.rejectChange", tr("Reject"), QKeySequence(), [] {});
    add("review.compatibility", tr("Compatibility Report"), QKeySequence(), [this] { showCompatibilityReport(); });
    add("view.navigation", tr("Navigation"), QKeySequence(), [this] {
        if (!navigation_) return;
        const auto* action = commands_.action(QStringLiteral("view.navigation"));
        navigation_->setVisible(action && action->isChecked());
        if (navigation_->isVisible()) {
            refreshNavigationResults();
            navigation_->focusQuery(false);
        }
    }, true);
    add("view.ruler", tr("Ruler"), QKeySequence(), [] {}, true);
    add("view.pageWidth", tr("Page Width"), QKeySequence(), [this] { if (activeCanvas()) activeCanvas()->setZoomPercent(125); });
    add("codex.toggle", tr("Codex Chat"), QKeySequence(QStringLiteral("Ctrl+Alt+C")), [this] { chat_->setVisible(!chat_->isVisible()); }, true);

    // Commands that mutate or inspect the document should only own their
    // shortcuts while a document canvas (or one of its children) has focus.
    // Keeping them window-wide makes shortcuts such as Ctrl+B fire against the
    // document while the user is editing a ribbon or dialog text field.
    for (const auto* id : {
             "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste",
             "edit.pasteTextOnly",
             "format.bold", "format.italic", "format.underline",
             "format.superscript", "format.subscript",
             "style.normal", "style.heading1", "style.heading2",
             "style.heading3",
             "paragraph.alignLeft", "paragraph.alignCenter",
             "paragraph.alignRight", "paragraph.justify", "paragraph.bullets",
             "paragraph.listProperties",
             "insert.pageBreak", "insert.equation"}) {
        if (auto* action = commands_.action(QString::fromLatin1(id))) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        }
    }

    auto setThemedIcon = [this](const char* id, const char* themeName,
                                QStyle::StandardPixmap fallback) {
        auto* action = commands_.action(QString::fromLatin1(id));
        if (!action) return;
        action->setIcon(QIcon::fromTheme(
            QString::fromLatin1(themeName), style()->standardIcon(fallback)));
        action->setToolTip(action->text());
    };
    setThemedIcon("file.new", "document-new", QStyle::SP_FileIcon);
    setThemedIcon("file.open", "document-open", QStyle::SP_DialogOpenButton);
    setThemedIcon("file.save", "document-save", QStyle::SP_DialogSaveButton);
    setThemedIcon("file.saveAs", "document-save-as", QStyle::SP_DialogSaveAllButton);
    setThemedIcon("file.exportPdf", "application-pdf", QStyle::SP_FileIcon);
    setThemedIcon("file.printPreview", "document-print-preview",
                  QStyle::SP_FileDialogContentsView);
    setThemedIcon("file.print", "document-print", QStyle::SP_FileDialogDetailedView);
    setThemedIcon("edit.undo", "edit-undo", QStyle::SP_ArrowBack);
    setThemedIcon("edit.redo", "edit-redo", QStyle::SP_ArrowForward);

    // Ribbon document commands must return keyboard ownership to the page.
    // This matters when the user previously typed in an editable combo box or
    // closed a picker: otherwise the command works, but the next text/Enter key
    // is delivered to the old ribbon control instead of the document.
    for (const auto* id : {
             "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste",
             "edit.pasteTextOnly",
             "format.bold", "format.italic", "format.underline", "format.strike",
             "format.superscript", "format.subscript",
             "style.normal", "style.noSpacing", "style.title",
             "style.subtitle", "style.quote", "style.heading1",
             "style.heading2", "style.heading3", "style.heading4",
             "style.heading5", "style.heading6", "style.heading7",
             "style.heading8", "style.heading9",
             "paragraph.bullets", "paragraph.numbering",
             "paragraph.listProperties",
             "paragraph.alignLeft", "paragraph.alignCenter",
             "paragraph.alignRight", "paragraph.justify",
             "paragraph.decreaseIndent", "paragraph.increaseIndent",
             "insert.pageBreak", "insert.table", "insert.image",
             "insert.excalidraw",
             "insert.equation", "insert.textBox", "insert.comment",
             "table.insertRowAbove", "table.insertRowBelow",
             "table.insertColumnLeft", "table.insertColumnRight",
             "table.deleteRows", "table.deleteColumns",
             "picture.size", "picture.altText", "picture.layoutOptions",
             "picture.editExcalidraw",
             "picture.wrapInline", "picture.wrapSquare",
             "picture.wrapTopBottom", "picture.delete",
             "layout.orientation", "layout.columns", "layout.lineSpacing",
             "layout.paragraphSpacing", "review.spelling", "review.comment",
             "review.trackChanges", "review.acceptChange", "review.rejectChange",
             "review.compatibility", "view.ruler",
             "view.pageWidth"}) {
        auto* action = commands_.action(QString::fromLatin1(id));
        if (!action) continue;
        connect(action, &QAction::triggered, this, [this] {
            auto* canvas = activeCanvas();
            if (!canvas) return;
            QTimer::singleShot(0, canvas, [canvas] { canvas->setFocus(); });
        });
    }
}

void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(commands_.action("file.new"));
    file->addAction(commands_.action("file.open"));
    recentMenu_ = file->addMenu(tr("Open Recent"));
    recentMenu_->setObjectName(QStringLiteral("file.openRecent"));
    recentMenu_->setToolTipsVisible(true);
    connect(recentMenu_, &QMenu::aboutToShow, this,
            &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();
    file->addSeparator();
    file->addAction(commands_.action("file.save"));
    file->addAction(commands_.action("file.saveAs"));
    file->addAction(commands_.action("file.exportPdf"));
    file->addAction(commands_.action("file.printPreview"));
    file->addAction(commands_.action("file.print"));
    file->addSeparator();
    file->addAction(commands_.action("file.options"));
    file->addSeparator();
    file->addAction(tr("Quit"), qApp, &QApplication::closeAllWindows, QKeySequence::Quit);
    auto* edit = menuBar()->addMenu(tr("&Edit"));
    for (const auto* id : {"edit.undo", "edit.redo", "edit.cut", "edit.copy",
                           "edit.paste", "edit.pasteTextOnly", "edit.find",
                           "edit.replace", "edit.findNext", "edit.findPrevious",
                           "edit.commandPalette"})
        edit->addAction(commands_.action(QString::fromLatin1(id)));
    auto* insert = menuBar()->addMenu(tr("&Insert"));
    for (const auto* id : {"insert.table", "insert.image",
                           "insert.excalidraw", "insert.equation",
                           "insert.pageBreak"})
        insert->addAction(commands_.action(QString::fromLatin1(id)));
    auto* review = menuBar()->addMenu(tr("&Review"));
    review->addAction(commands_.action("review.spelling"));
    review->addAction(commands_.action("review.compatibility"));
    auto* view = menuBar()->addMenu(tr("&View"));
    view->addAction(commands_.action("view.navigation"));
    view->addAction(commands_.action("view.pageWidth"));
    view->addSeparator();
    view->addAction(commands_.action("codex.toggle"));
}

void MainWindow::connectRibbon() {
    connect(ribbon_, &RibbonWidget::paragraphStyleRequested, this,
            [this](const QString& styleId) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        canvas->applyParagraphStyle(styleId);
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::fontFamilyRequested, this,
            [this](const QString& family) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        canvas->setFontFamily(family);
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::fontPointSizeRequested, this,
            [this](double points) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        canvas->setFontPointSize(points);
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::textColorSelected,
            this, &MainWindow::applyTextColor);
    connect(ribbon_, &RibbonWidget::highlightColorSelected,
            this, &MainWindow::applyHighlightColor);
    connect(ribbon_, &RibbonWidget::clearHighlightRequested,
            this, &MainWindow::clearHighlight);
    connect(ribbon_, &RibbonWidget::textColorRequested, this,
            [this] { showLiveColorPicker(false); });
    connect(ribbon_, &RibbonWidget::highlightColorRequested, this,
            [this] { showLiveColorPicker(true); });
    connect(ribbon_, &RibbonWidget::marginPresetRequested, this, [this](const QString& preset) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (preset == QStringLiteral("custom")) {
            const auto margins = requestCustomMargins(this, *canvas);
            if (margins) {
                canvas->setMarginsPoints((*margins)[0], (*margins)[1],
                                         (*margins)[2], (*margins)[3]);
            }
        } else if (preset == QStringLiteral("narrow")) {
            canvas->setMarginsPoints(36, 36, 36, 36);
        } else if (preset == QStringLiteral("moderate")) {
            canvas->setMarginsPoints(72, 54, 72, 54);
        } else if (preset == QStringLiteral("wide")) {
            canvas->setMarginsPoints(72, 144, 72, 144);
        } else if (preset == QStringLiteral("office2003")) {
            canvas->setMarginsPoints(72, 90, 72, 90);
        } else {
            canvas->setMarginsPoints(72, 72, 72, 72);
        }
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::pageSizeRequested, this, [this](const QString& preset) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        if (preset == tr("A4")) canvas->setPageSizePoints(595.28, 841.89);
        else if (preset == tr("Legal")) canvas->setPageSizePoints(612, 1008);
        else canvas->setPageSizePoints(612, 792);
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::zoomRequested, this, [this](int zoom) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        setActiveZoomPercent(zoom);
        restoreCanvasFocus(canvas);
    });
    connect(ribbon_, &RibbonWidget::listPropertiesRequested,
            this, &MainWindow::showListProperties);
    connect(ribbon_, &RibbonWidget::tableStyleRequested, this,
            [this](const QString& styleKey) {
        auto* canvas = activeCanvas();
        if (!canvas) return;
        static_cast<void>(canvas->setTableStyle(styleKey));
        restoreCanvasFocus(canvas);
    });
}

void MainWindow::restoreCanvasFocus(DocumentCanvas* canvas) {
    if (!canvas) return;
    canvas->setFocus();
    QTimer::singleShot(0, canvas, [canvas] { canvas->setFocus(); });
}

void MainWindow::setActiveZoomPercent(int percent) {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    canvas->setZoomPercent(percent);
    // setZoomPercent deliberately emits only for a real change. Always sync
    // so an attempted move beyond either bound snaps every control back.
    synchronizeZoomControls(canvas);
}

void MainWindow::synchronizeZoomControls(DocumentCanvas* canvas) {
    const bool available = canvas != nullptr;
    const int percent = available ? canvas->zoomPercent() : 100;
    if (zoomSlider_) {
        const QSignalBlocker blocker(zoomSlider_);
        zoomSlider_->setValue(percent);
        zoomSlider_->setEnabled(available);
    }
    if (zoomStatus_) {
        zoomStatus_->setText(tr("%1%").arg(percent));
        zoomStatus_->setEnabled(available);
    }
    if (zoomOut_) {
        zoomOut_->setEnabled(
            available && percent > DocumentCanvas::kMinimumZoomPercent);
    }
    if (zoomIn_) {
        zoomIn_->setEnabled(
            available && percent < DocumentCanvas::kMaximumZoomPercent);
    }
    if (ribbon_) ribbon_->setZoomPercent(percent);
}

void MainWindow::synchronizeParagraphStyleAvailability(
    DocumentCanvas* canvas) {
    if (!ribbon_) return;
    if (!canvas) {
        ribbon_->setParagraphStyleAvailable(
            false, tr("Paragraph styles unavailable without a document"));
    } else if (canvas->hasPreview()) {
        ribbon_->setParagraphStyleAvailable(
            false, tr("Paragraph styles unavailable during preview"));
    } else if (!canvas->paragraphStylesAvailable()) {
        ribbon_->setParagraphStyleAvailable(
            false, tr("Paragraph styles unavailable in tables"));
    } else {
        ribbon_->setParagraphStyleAvailable(true);
    }
}

void MainWindow::applyTextColor(const QColor& color) {
    auto* canvas = activeCanvas();
    if (!canvas || !color.isValid()) return;
    if (textColorPicker_) delete textColorPicker_.data();
    if (highlightColorPicker_) delete highlightColorPicker_.data();
    canvas->setForeground(color);
    ribbon_->setTextColor(color);
    restoreCanvasFocus(canvas);
}

void MainWindow::applyHighlightColor(const QColor& color) {
    auto* canvas = activeCanvas();
    if (!canvas || !color.isValid()) return;
    if (textColorPicker_) delete textColorPicker_.data();
    if (highlightColorPicker_) delete highlightColorPicker_.data();
    canvas->setHighlight(color);
    ribbon_->setHighlightColor(color);
    restoreCanvasFocus(canvas);
}

void MainWindow::clearHighlight() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    if (textColorPicker_) delete textColorPicker_.data();
    if (highlightColorPicker_) delete highlightColorPicker_.data();
    canvas->clearHighlight();
    ribbon_->setHighlightColor({});
    restoreCanvasFocus(canvas);
}

void MainWindow::showLiveColorPicker(bool highlight) {
    auto& existing = highlight ? highlightColorPicker_ : textColorPicker_;
    if (existing) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    auto& other = highlight ? textColorPicker_ : highlightColorPicker_;
    if (other) delete other.data();

    auto* canvas = activeCanvas();
    if (!canvas) return;
    QPointer<DocumentCanvas> target(canvas);
    target->beginColorAdjustment();
    QColor initial = highlight ? canvas->currentHighlightColor()
                               : canvas->currentTextColor();
    if (!initial.isValid()) initial = QColor(QStringLiteral("#FFF200"));

    auto* picker = new QColorDialog(initial, this);
    picker->setObjectName(highlight
                              ? QStringLiteral("highlightColorPicker")
                              : QStringLiteral("textColorPicker"));
    picker->setWindowTitle(highlight ? tr("Highlight color") : tr("Text color"));
    picker->setOption(QColorDialog::DontUseNativeDialog, true);
    picker->setOption(QColorDialog::NoButtons, true);
    picker->setWindowModality(Qt::NonModal);
    picker->setAttribute(Qt::WA_DeleteOnClose, true);
    existing = picker;

    connect(picker, &QColorDialog::currentColorChanged, this,
            [this, highlight, target](const QColor& color) {
        if (!target || !color.isValid()) return;
        if (highlight) {
            target->setHighlight(color);
            ribbon_->setHighlightColor(color);
        } else {
            target->setForeground(color);
            ribbon_->setTextColor(color);
        }
    });
    connect(picker, &QObject::destroyed, this, [this, highlight, target] {
        if (target) target->endColorAdjustment();
        if (highlight) highlightColorPicker_.clear();
        else textColorPicker_.clear();
        if (auto* active = activeCanvas()) {
            QTimer::singleShot(0, active, [active] { active->setFocus(); });
        }
    });
    picker->show();
    picker->raise();
    picker->activateWindow();
}

DocumentCanvas* MainWindow::createDocumentTab(core::Document document,
                                               std::unique_ptr<TabState> state,
                                               const QString& title) {
    if (state->journalKey.isEmpty())
        state->journalKey = QString::fromStdString(core::NodeId::generate().toString());
    if (state->documentKey.isEmpty())
        state->documentKey = QStringLiteral("document:%1").arg(state->journalKey);
    auto* canvas = new DocumentCanvas(spelling_, tabs_);
    canvas->setEditorDefaults(editorPreferences_.defaultFontFamily(),
                              editorPreferences_.defaultFontPointSize(),
                              editorPreferences_.tabWidthSpaces());
    canvas->setDefaultListLayout(editorPreferences_.defaultListLayout());
    canvas->setDocument(std::move(document));
    for (auto* action : commands_.actions()) {
        if (action->shortcutContext() == Qt::WidgetWithChildrenShortcut) {
            canvas->addAction(action);
        }
    }
    const int index = tabs_->addTab(canvas, title);
    states_.emplace(canvas, std::move(state));
    tabs_->setCurrentIndex(index);
    connect(canvas, &DocumentCanvas::documentChanged, this, [this, canvas] {
        updateTabTitle(canvas);
        recoveryDebounce_->start();
        if (canvas == activeCanvas() && navigation_->isVisible()) {
            if (auto* tabState = stateFor(canvas)) {
                tabState->navigationHits.clear();
            }
            navigation_->clearSearchResults();
            navigationSearchDebounce_->start();
        }
    });
    connect(canvas, &DocumentCanvas::previewStateChanged, this,
            [this, canvas](bool active) {
        if (canvas != activeCanvas()) return;
        synchronizeParagraphStyleAvailability(canvas);
        navigation_->setReplacementLocked(active);
        if (!navigation_->isVisible()) return;
        if (auto* tabState = stateFor(canvas)) {
            tabState->navigationHits.clear();
        }
        navigation_->clearSearchResults();
        // Preview creation and dismissal replace the complete visible search
        // corpus. Refresh immediately so no live-branch row remains actionable
        // against a preview (or vice versa).
        refreshNavigationResults();
    });
    connect(canvas, &DocumentCanvas::cursorFormatChanged, this,
            [this](const QString& family, double points, const QColor& textColor) {
        ribbon_->setFontFamily(family);
        ribbon_->setFontPointSize(points);
        ribbon_->setTextColor(textColor);
    });
    connect(canvas, &DocumentCanvas::cursorHighlightChanged, this,
            [this](const QColor& highlightColor) {
        ribbon_->setHighlightColor(highlightColor);
    });
    connect(canvas, &DocumentCanvas::cursorStyleChanged, this,
            [this](bool bold, bool italic, bool underline, bool strike,
                   bool superscript, bool subscript) {
        commands_.action(QStringLiteral("format.bold"))->setChecked(bold);
        commands_.action(QStringLiteral("format.italic"))->setChecked(italic);
        commands_.action(QStringLiteral("format.underline"))->setChecked(underline);
        commands_.action(QStringLiteral("format.strike"))->setChecked(strike);
        commands_.action(QStringLiteral("format.superscript"))->setChecked(superscript);
        commands_.action(QStringLiteral("format.subscript"))->setChecked(subscript);
    });
    connect(canvas, &DocumentCanvas::cursorParagraphStyleChanged, this,
            [this, canvas](const QString& styleId) {
        if (canvas == activeCanvas()) ribbon_->setParagraphStyle(styleId);
    });
    connect(canvas, &DocumentCanvas::cursorListStateChanged, this,
            [this](bool bullets, bool numbering) {
        commands_.action(QStringLiteral("paragraph.bullets"))->setChecked(bullets);
        commands_.action(QStringLiteral("paragraph.numbering"))->setChecked(numbering);
    });
    connect(canvas, &DocumentCanvas::cursorListContextChanged, this,
            [this](bool active, int level) {
        ribbon_->setListContext(active, level);
    });
    connect(canvas, &DocumentCanvas::selectionChanged, this, [this, canvas] {
        if (canvas == activeCanvas()) {
            ribbon_->setTableContext(canvas->selectedTableId().has_value());
            synchronizeParagraphStyleAvailability(canvas);
            const auto pictureLayout = canvas->selectedImageLayout();
            ribbon_->setPictureContext(
                pictureLayout.has_value(),
                pictureLayout.value_or(core::ImageLayout{}).placement,
                canvas->selectedExcalidrawScene().has_value());
            synchronizeNavigationSelection();
        }
    });
    connect(canvas, &DocumentCanvas::listPropertiesRequested,
            this, &MainWindow::showListProperties);
    connect(canvas, &DocumentCanvas::editExcalidrawFigureRequested,
            this, [this, canvas] {
                editSelectedExcalidrawFigure(canvas);
            });
    connect(canvas, &DocumentCanvas::pageStatusChanged, this, [this](int page, int count, int words) {
        pageStatus_->setText(tr("Page %1 of %2   %3 words").arg(page).arg(count).arg(words));
    });
    connect(canvas, &DocumentCanvas::zoomChanged, this,
            [this, canvas](int) {
        if (canvas == activeCanvas()) synchronizeZoomControls(canvas);
    });
    connect(canvas, &DocumentCanvas::operationFailed, this, [this](const QString& message) {
        QMessageBox::warning(this, tr("Edit could not be applied"), message);
    });
    ribbon_->setParagraphStyle(canvas->currentParagraphStyleId());
    synchronizeParagraphStyleAvailability(canvas);
    ribbon_->setFontFamily(canvas->currentFontFamily());
    ribbon_->setFontPointSize(canvas->currentFontPointSize());
    ribbon_->setTextColor(canvas->currentTextColor());
    ribbon_->setHighlightColor(canvas->currentHighlightColor());
    ribbon_->setTableContext(canvas->selectedTableId().has_value());
    const auto pictureLayout = canvas->selectedImageLayout();
    ribbon_->setPictureContext(
        pictureLayout.has_value(),
        pictureLayout.value_or(core::ImageLayout{}).placement);
    synchronizeZoomControls(canvas);
    loadNavigationForActiveDocument();
    canvas->refreshCursorFormat();
    canvas->setFocus();
    updateWindowTitle();
    return canvas;
}

DocumentCanvas* MainWindow::activeCanvas() const { return qobject_cast<DocumentCanvas*>(tabs_->currentWidget()); }
MainWindow::TabState* MainWindow::stateFor(DocumentCanvas* canvas) const {
    const auto found = states_.find(canvas); return found == states_.end() ? nullptr : found->second.get();
}
MainWindow::TabState* MainWindow::activeState() const { return stateFor(activeCanvas()); }

void MainWindow::newDocument(bool replaceOnSuccessfulOpen) {
    auto state = std::make_unique<TabState>();
    state->baselineTexts = {QString()}; state->regeneratable = true;
    state->replaceOnSuccessfulOpen = replaceOnSuccessfulOpen;
    createDocumentTab(documentFromText({QString()}), std::move(state), tr("Untitled"));
}

void MainWindow::openDocument() {
    const auto paths = QFileDialog::getOpenFileNames(
        this, tr("Open DOCX"), {}, tr("Word documents (*.docx)"));
    openPaths(paths);
}

bool MainWindow::openPath(const QString& path) {
    const QString absolutePath = normalizedAbsolutePath(path);
    if (auto* existing = canvasForPath(absolutePath)) {
        tabs_->setCurrentWidget(existing);
        if (const auto* state = stateFor(existing)) addRecentFile(state->path);
        statusBar()->showMessage(tr("Document is already open"), 3000);
        return true;
    }

    QFile source(absolutePath);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(
            this, tr("Could not open document"),
            tr("The file could not be opened for reading: %1")
                .arg(source.errorString()));
        return false;
    }
    const QString descriptorPath =
        QStringLiteral("/proc/self/fd/%1").arg(source.handle());
    QString documentPath = QFileInfo(descriptorPath).symLinkTarget();
    if (!isExistingRegularFile(documentPath)) {
        documentPath = QFileInfo(absolutePath).canonicalFilePath();
    }
    if (!isExistingRegularFile(documentPath)) documentPath = absolutePath;
    documentPath = normalizedAbsolutePath(documentPath);
    if (auto* existing = canvasForPath(documentPath)) {
        tabs_->setCurrentWidget(existing);
        if (const auto* state = stateFor(existing)) addRecentFile(state->path);
        statusBar()->showMessage(tr("Document is already open"), 3000);
        return true;
    }

    auto* replaceableUntitled = replaceableUntitledCanvas();
    QString fingerprintError;
    auto diskFingerprint = fingerprintOpenFile(source, fingerprintError);
    if (!diskFingerprint) {
        QMessageBox::critical(
            this, tr("Could not monitor document"),
            tr("Owl Docs could not establish a safe on-disk fingerprint for "
               "this file: %1")
                .arg(fingerprintError));
        return false;
    }
    worker::ParserClientOptions workerOptions;
    workerOptions.helper_executable = parserWorkerPath();
    const auto preflight = worker::parseDocxDescriptorWithWorker(
        source.handle(), workerOptions);
    if (!preflight.transportOk()) {
        QMessageBox::critical(
            this, tr("Could not inspect document safely"),
            tr("The restricted parser worker failed: %1")
                .arg(fromUtf8(preflight.error)));
        return false;
    }
    if (!preflight.response.ok()) {
        QMessageBox::critical(
            this, tr("Could not open document"),
            tr("The restricted parser rejected this file: %1")
                .arg(fromUtf8(preflight.response.error)));
        return false;
    }
    ooxml::Error error;
    // Reopen the exact descriptor that the restricted worker inspected. Using
    // /proc/self/fd here prevents a path replacement between preflight and the
    // preservation-model import. The GUI parser surface is still a known
    // hardening gap, but it cannot be handed different bytes by a pathname
    // race.
    auto package = ooxml::DocxDocument::open(nativePath(descriptorPath), &error);
    if (!package) {
        QMessageBox::critical(this, tr("Could not open document"), fromUtf8(error.message)); return false;
    }
    auto semantic = documentFromOoxml(
        *package, editorPreferences_.defaultFontFamily(),
        editorPreferences_.defaultFontPointSize(),
        editorPreferences_.tabWidthSpaces());
    if (!semantic) {
        QMessageBox::critical(this, tr("Could not map document"),
                              fromUtf8(semantic.error().message));
        return false;
    }
    const QStringList paragraphs = currentTexts(
        {core::Revision{}, semantic.value().document});
    auto state = std::make_unique<TabState>();
    state->path = documentPath;
    state->diskFingerprint = std::move(diskFingerprint);
    state->baselineTexts = paragraphs;
    state->sourceParagraphIndices =
        semantic.value().sourceParagraphIndices;
    state->sourceTextPrefixes =
        semantic.value().sourceTextPrefixes;
    for (const auto& table : semantic.value().tables) {
        if (table.sourceStyleId) {
            state->importedTableStyleSources.emplace(
                table.tableId,
                std::pair{table.sourceSemanticStyle,
                          *table.sourceStyleId});
        }
    }
    state->package = std::move(package);
    const auto importedDefaults = toOoxmlDocumentDefaults(
        editorPreferences_.defaultFontFamily(),
        editorPreferences_.defaultFontPointSize(),
        editorPreferences_.tabWidthSpaces());
    state->regeneratable =
        state->package->isCanonicalRegeneratableSimplePackage(
            importedDefaults);
    if (state->regeneratable) {
        state->regenerationDefaults = importedDefaults;
    }
    const auto page = state->package->bodyPageSettings();
    auto importedImages = std::move(semantic.value().images);
    auto importedTables = std::move(semantic.value().tables);
    auto* canvas = createDocumentTab(
                                     std::move(semantic.value().document),
                                     std::move(state),
                                     QFileInfo(documentPath).fileName());
    canvas->setImportedPresentation(
        std::move(importedImages), std::move(importedTables));
    if (page) {
        canvas->setImportedPageLayout(
            page->width_twips / 20.0, page->height_twips / 20.0,
            page->margin_top_twips / 20.0, page->margin_right_twips / 20.0,
            page->margin_bottom_twips / 20.0, page->margin_left_twips / 20.0);
    }
    if (replaceableUntitled) {
        const int placeholderIndex = tabs_->indexOf(replaceableUntitled);
        const auto placeholderState = states_.find(replaceableUntitled);
        if (placeholderIndex >= 0 && placeholderState != states_.end() &&
            placeholderState->second->replaceOnSuccessfulOpen &&
            !replaceableUntitled->isModified()) {
            tabs_->removeTab(placeholderIndex);
            states_.erase(placeholderState);
            delete replaceableUntitled;
        }
    }
    addRecentFile(documentPath);
    return true;
}

int MainWindow::openPaths(const QStringList& paths) {
    int opened = 0;
    int skipped = 0;
    for (const auto& path : paths) {
        if (path.isEmpty() || !isOpenableDocxPath(path)) {
            ++skipped;
            continue;
        }
        if (openPath(path)) ++opened;
    }
    if (skipped > 0) {
        statusBar()->showMessage(
            skipped == 1
                ? tr("Skipped 1 unavailable or unsupported document")
                : tr("Skipped %1 unavailable or unsupported documents")
                      .arg(skipped),
            5000);
    }
    return opened;
}

bool MainWindow::saveDocument(bool saveAs) { return activeCanvas() && saveCanvas(activeCanvas(), saveAs); }

bool MainWindow::saveCanvas(DocumentCanvas* canvas, bool saveAs) {
    auto* state = stateFor(canvas);
    if (!state) return false;
    if (canvas->hasPreview()) {
        QMessageBox::information(
            this, tr("Preview awaiting review"),
            tr("Accept or discard the Codex preview before saving the document."));
        return false;
    }
    QString target = state->path;
    const bool destinationMustBeChosen = saveAs || target.isEmpty();
    if (destinationMustBeChosen) {
        target = QFileDialog::getSaveFileName(this, tr("Save DOCX"), target.isEmpty() ? tr("Untitled.docx") : target,
                                              tr("Word documents (*.docx)"));
        if (target.isEmpty()) return false;
        if (!target.endsWith(QStringLiteral(".docx"), Qt::CaseInsensitive)) target += QStringLiteral(".docx");
    }
    if (!canvas->isModified() && !saveAs && !state->path.isEmpty()) return true;

    if (!state->path.isEmpty() && sameFileDestination(target, state->path)) {
        QString fingerprintError;
        const auto comparison = state->diskFingerprint
                                    ? compareFileFingerprint(
                                          target, *state->diskFingerprint,
                                          fingerprintError)
                                    : FileFingerprintComparison::unavailable;
        if (comparison != FileFingerprintComparison::matches) {
            QMessageBox box(
                QMessageBox::Warning, tr("Save As required"),
                comparison == FileFingerprintComparison::differs
                    ? tr("The document on disk has changed since Owl Docs "
                         "opened or last saved it. Owl Docs did not overwrite "
                         "those changes. Use Save As with a different name.")
                    : tr("Owl Docs could not verify that the document on disk "
                         "is still the version that was opened or last saved. "
                         "It was not overwritten. Use Save As with a different "
                         "name."),
                QMessageBox::Ok, this);
            if (!fingerprintError.isEmpty()) {
                box.setDetailedText(fingerprintError);
            }
            box.exec();
            return false;
        }
    }

    const auto snapshot = canvas->snapshot();
    const auto texts = currentTexts(snapshot);
    // An unchanged Save As must retain the original package byte-for-byte,
    // including for documents first created by this application. Regeneration
    // is reserved for an unsaved document or an actual semantic/layout edit.
    const auto currentDefaults = toOoxmlDocumentDefaults(*canvas);
    const bool regenerationDefaultsStillMatch =
        !state->regenerationDefaults ||
        *state->regenerationDefaults == currentDefaults;
    bool regenerate = state->regeneratable &&
                      regenerationDefaultsStillMatch &&
                      (canvas->isModified() || !state->package);
    bool simplificationConfirmed =
        state->simplificationWarningAcknowledged;
    const auto confirmSimplification = [&]() {
        if (state->simplificationWarningAcknowledged) {
            simplificationConfirmed = true;
            return true;
        }
        const QString protectedSource = !state->path.isEmpty()
                                            ? state->path
                                            : state->recoverySourcePath;
        const bool overwritingOriginal =
            sameFileDestination(target, protectedSource);
        QMessageBox box(
            QMessageBox::Warning, tr("Compatibility warning"),
            tr("This imported document contains content that this build "
               "preserves but cannot safely rewrite after structural or "
               "formatting changes. Saving anyway will rebuild the DOCX with "
               "Owl Docs' supported content."),
            QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        if (auto* saveAnyway = box.button(QMessageBox::Yes)) {
            saveAnyway->setText(tr("Save Anyway"));
        }
        if (auto* cancel = box.button(QMessageBox::No)) {
            cancel->setText(tr("Cancel"));
        }
        QString details = tr(
            "Potential losses include headers/footers, comments, tracked revisions, "
            "fields, charts, SmartArt, text boxes, floating drawings, and custom XML. "
            "Editable body text and supported formatting will be retained.");
        details += QLatin1Char('\n');
        details += overwritingOriginal
            ? tr("Continuing will atomically replace the original file. Cancel if "
                 "you want to use Save As and retain that original separately.")
            : tr("Continuing will write the selected destination. The imported "
                 "original will remain unchanged.");
        box.setDetailedText(details);
        if (box.exec() != QMessageBox::Yes) return false;
        state->simplificationWarningAcknowledged = true;
        simplificationConfirmed = true;
        return true;
    };
    if (!regenerate && canvas->isModified()) {
        regenerate = canvas->hasNonTextChanges() ||
                     texts.size() != state->baselineTexts.size() ||
                     static_cast<std::size_t>(texts.size()) !=
                         state->sourceParagraphIndices.size();
        if (regenerate && !confirmSimplification()) return false;
    }

    bool savedWithPackage = false;
    if (!regenerate && state->package) {
        QString patchFailure;
        for (int index = 0; index < texts.size(); ++index) {
            if (texts[index] == state->baselineTexts[index]) continue;
            const auto semanticIndex = static_cast<std::size_t>(index);
            if (semanticIndex >= state->sourceParagraphIndices.size() ||
                state->sourceParagraphIndices[semanticIndex] >=
                    state->package->paragraphs().size()) {
                patchFailure = tr(
                    "This paragraph has no safe source mapping in the imported DOCX.");
                break;
            }
            QString patchText = texts[index];
            if (index < state->sourceTextPrefixes.size() &&
                !state->sourceTextPrefixes[index].isEmpty()) {
                const QString& prefix = state->sourceTextPrefixes[index];
                if (!patchText.startsWith(prefix)) {
                    patchFailure = tr(
                        "The displayed native list marker was edited. "
                        "That structural list change requires a simplified Save As.");
                    break;
                }
                patchText.remove(0, prefix.size());
            }
            const auto edit = state->package->replaceParagraphText(
                state->sourceParagraphIndices[semanticIndex],
                patchText.toUtf8().toStdString());
            if (!edit) {
                state->package->discardEdits();
                patchFailure = edit.error ? fromUtf8(edit.error->message)
                                          : tr("This paragraph cannot be safely patched.");
                break;
            }
        }
        if (!patchFailure.isEmpty()) {
            regenerate = true;
            if (!simplificationConfirmed && !confirmSimplification()) return false;
        } else {
            const auto result = state->package->saveAs(nativePath(target));
            if (!result || result.error.has_value()) {
                state->package->discardEdits();
                QMessageBox::critical(this, tr("Save validation failed"),
                                      saveError(result.error));
                return false;
            }
            savedWithPackage = true;
        }
    }
    // Recovery intentionally stores only the bounded semantic snapshot, never
    // an imported package's opaque ZIP members. A restored imported document
    // therefore cannot take the normal package-preserving save path and must
    // be treated as an explicitly warned simplified save.
    if (!savedWithPackage && !state->package && !state->regeneratable) {
        regenerate = true;
        if (!simplificationConfirmed && !confirmSimplification()) return false;
    }
    if (!savedWithPackage) {
        QStringList losses;
        auto output = toOoxmlBody(
            snapshot, losses, canvas->defaultFontFamily(),
            canvas->defaultFontPointSize(),
            state->importedTableStyleSources);
        losses.removeDuplicates();
        if (!losses.isEmpty() && !simplificationConfirmed) {
            QMessageBox box(QMessageBox::Warning, tr("Some formatting is not serializable yet"),
                            tr("Save the DOCX with the supported subset?"),
                            QMessageBox::Yes | QMessageBox::No, this);
            box.setDefaultButton(QMessageBox::No);
            box.setDetailedText(losses.join(QLatin1Char('\n')));
            if (box.exec() != QMessageBox::Yes) return false;
        }
        const auto result = ooxml::DocxDocument::writeNew(
            nativePath(target), output, toOoxmlPageSettings(*canvas),
            currentDefaults);
        if (!result) { QMessageBox::critical(this, tr("Save failed"), saveError(result.error)); return false; }
        ooxml::Error reopenError;
        state->package = ooxml::DocxDocument::open(nativePath(target), &reopenError);
        if (!state->package) { QMessageBox::critical(this, tr("Save validation failed"), fromUtf8(reopenError.message)); return false; }
        state->regeneratable = true;
        state->regenerationDefaults = currentDefaults;
        state->sourceParagraphIndices.clear();
        state->sourceTextPrefixes.clear();
    }
    QString savedFingerprintError;
    auto savedFingerprint = fingerprintFile(target, savedFingerprintError);
    state->path = QFileInfo(target).absoluteFilePath();
    state->diskFingerprint = std::move(savedFingerprint);
    state->recoverySourcePath.clear();
    state->recoveryDisplayName.clear();
    state->recovered = false;
    state->baselineTexts = texts;
    canvas->markSaved();
    deleteRecoveryFor(canvas);
    addRecentFile(target);
    updateTabTitle(canvas);
    saveStatus_->setText(state->diskFingerprint
                             ? tr("Saved")
                             : tr("Saved — use Save As for the next change"));
    if (!state->diskFingerprint) {
        QMessageBox::warning(
            this, tr("Saved, but monitoring is unavailable"),
            tr("The document was saved, but Owl Docs could not fingerprint the "
               "saved file (%1). To avoid overwriting another program's "
               "changes, the next edit must be saved under a different name.")
                .arg(savedFingerprintError));
    }
    QTimer::singleShot(1800, this, [this] { saveStatus_->setText(tr("Ready")); });
    return true;
}

bool MainWindow::maybeCloseTab(int index) {
    auto* canvas = qobject_cast<DocumentCanvas*>(tabs_->widget(index));
    if (!canvas || !canvas->isModified()) return true;
    const auto answer = QMessageBox::question(this, tr("Save changes?"),
        tr("Save changes to %1?").arg(tabs_->tabText(index)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return saveCanvas(canvas, false);
    return true;
}

void MainWindow::closeTab(int index) {
    if (!maybeCloseTab(index)) return;
    auto* canvas = qobject_cast<DocumentCanvas*>(tabs_->widget(index));
    if (canvas && documentKeyFor(canvas) == previewDocumentKey_) {
        canvas->discardPreview();
        previewDocumentKey_.clear();
        previewLabel_.clear();
        previewSummary_.clear();
        chat_->hidePreview();
    }
    deleteRecoveryFor(canvas);
    tabs_->removeTab(index); states_.erase(canvas); delete canvas;
    if (tabs_->count() == 0) newDocument(true);
}

void MainWindow::exportPdf() {
    auto* canvas = activeCanvas(); if (!canvas) return;
    if (canvas->hasPreview()) {
        QMessageBox::information(
            this, tr("Preview awaiting review"),
            tr("Accept or discard the Codex preview before exporting PDF."));
        return;
    }
    QString suggested = activeState() && !activeState()->path.isEmpty()
                            ? QFileInfo(activeState()->path).completeBaseName() + QStringLiteral(".pdf")
                            : tr("Untitled.pdf");
    auto path = QFileDialog::getSaveFileName(this, tr("Export PDF"), suggested, tr("PDF files (*.pdf)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        path += QStringLiteral(".pdf");
    QString error; if (!canvas->exportPdf(path, error)) QMessageBox::critical(this, tr("PDF export failed"), error);
    else statusBar()->showMessage(tr("Exported %1").arg(path), 5000);
}

void MainWindow::printPreview() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    if (canvas->hasPreview()) {
        QMessageBox::information(
            this, tr("Preview awaiting review"),
            tr("Accept or discard the Codex preview before printing."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QString setupError;
    if (!canvas->configurePrinter(printer, setupError)) {
        QMessageBox::critical(this, tr("Print preview failed"), setupError);
        return;
    }
    const QString documentName =
        activeState() && !activeState()->path.isEmpty()
            ? QFileInfo(activeState()->path).fileName()
            : tabs_->tabText(tabs_->currentIndex());
    printer.setDocName(documentName);
    printer.setCreator(QStringLiteral("Owl Docs"));

    QPrintPreviewDialog dialog(&printer, this);
    dialog.setObjectName(QStringLiteral("printPreviewDialog"));
    dialog.setWindowTitle(tr("Print Preview — %1").arg(documentName));
    dialog.resize(1050, 760);
    QString renderError;
    connect(&dialog, &QPrintPreviewDialog::paintRequested, this,
            [canvas, &dialog, &renderError](QPrinter* target) {
        QString currentError;
        if (!canvas->printTo(*target, currentError)) {
            if (renderError.isEmpty()) renderError = currentError;
            target->abort();
            QTimer::singleShot(0, &dialog, &QDialog::reject);
        }
    });
    dialog.exec();
    if (!renderError.isEmpty()) {
        QMessageBox::critical(this, tr("Print preview failed"), renderError);
    }
}

void MainWindow::printDocument() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    if (canvas->hasPreview()) {
        QMessageBox::information(
            this, tr("Preview awaiting review"),
            tr("Accept or discard the Codex preview before printing."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QString setupError;
    if (!canvas->configurePrinter(printer, setupError)) {
        QMessageBox::critical(this, tr("Printing failed"), setupError);
        return;
    }
    const QString documentName =
        activeState() && !activeState()->path.isEmpty()
            ? QFileInfo(activeState()->path).fileName()
            : tabs_->tabText(tabs_->currentIndex());
    printer.setDocName(documentName);
    printer.setCreator(QStringLiteral("Owl Docs"));

    QPrintDialog dialog(&printer, this);
    dialog.setObjectName(QStringLiteral("printDialog"));
    dialog.setWindowTitle(tr("Print — %1").arg(documentName));
    dialog.setMinMax(1, canvas->pageCount());
    dialog.setFromTo(1, canvas->pageCount());
    dialog.setOption(QAbstractPrintDialog::PrintToFile);
    dialog.setOption(QAbstractPrintDialog::PrintPageRange);
    dialog.setOption(QAbstractPrintDialog::PrintCurrentPage);
    dialog.setOption(QAbstractPrintDialog::PrintShowPageSize);
    dialog.setOption(QAbstractPrintDialog::PrintCollateCopies);
    if (dialog.exec() != QDialog::Accepted) return;
    QString error;
    if (!canvas->printTo(printer, error)) {
        QMessageBox::critical(this, tr("Printing failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Print job sent"), 5000);
}

void MainWindow::showFindReplace(bool replaceMode) {
    if (!activeCanvas() || !navigation_) return;
    if (auto* state = activeState()) {
        state->navigationReplaceMode = replaceMode;
    }
    navigation_->setMode(replaceMode ? NavigationDock::Mode::replace
                                     : NavigationDock::Mode::find);
    navigation_->show();
    navigation_->raise();
    refreshNavigationResults();
    navigation_->focusQuery();
}

void MainWindow::loadNavigationForActiveDocument() {
    if (!navigation_) return;
    const QSignalBlocker blocker(navigation_);
    if (const auto* state = activeState()) {
        navigation_->setMode(
            state->navigationReplaceMode ? NavigationDock::Mode::replace
                                         : NavigationDock::Mode::find);
        navigation_->setQuery(state->navigationQuery);
        navigation_->setReplacement(state->navigationReplacement);
        navigation_->setMatchCase(state->navigationMatchCase);
        navigation_->setWholeWords(state->navigationWholeWords);
    } else {
        navigation_->setMode(NavigationDock::Mode::find);
        navigation_->setQuery({});
        navigation_->setReplacement({});
        navigation_->setMatchCase(false);
        navigation_->setWholeWords(false);
    }
    refreshNavigationResults();
}

void MainWindow::refreshNavigationResults() {
    if (navigationSearchDebounce_) navigationSearchDebounce_->stop();
    auto* canvas = activeCanvas();
    if (!navigation_ || !canvas) {
        if (navigation_) navigation_->clearSearchResults();
        return;
    }
    navigation_->setReplacementLocked(canvas->hasPreview());
    // A remembered per-document query must not turn every keystroke into a
    // whole-document scan after the user closes the modeless pane. Reopening
    // it triggers a fresh search through visibilityChanged.
    if (!navigation_->isVisible()) return;
    const QString query = navigation_->query();
    if (query.isEmpty()) {
        if (auto* state = activeState()) state->navigationHits.clear();
        navigation_->clearSearchResults();
        return;
    }

    const auto matches = canvas->searchMatches(
        query, DocumentSearchOptions{navigation_->matchCase(),
                                     navigation_->wholeWords()});
    auto* state = activeState();
    if (state) {
        state->navigationHits.clear();
        state->navigationHits.reserve(matches.size());
        for (const auto& match : matches) {
            state->navigationHits.push_back(match.hit);
        }
    }
    const auto current = canvas->currentSearchHit();
    QList<NavigationResult> results;
    results.reserve(static_cast<qsizetype>(matches.size()));
    int currentIndex = -1;

    const auto snippetFor = [](const QString& text, std::size_t startOffset,
                               std::size_t endOffset) {
        constexpr qsizetype context = 36;
        const auto start = static_cast<qsizetype>(startOffset);
        const auto end = static_cast<qsizetype>(endOffset);
        const qsizetype snippetStart = std::max<qsizetype>(0, start - context);
        const qsizetype snippetEnd = std::min<qsizetype>(
            text.size(), end + context);
        QString snippet = text.mid(snippetStart, snippetEnd - snippetStart);
        snippet.replace(QLatin1Char('\n'), QLatin1Char(' '));
        snippet.replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (snippetStart > 0) snippet.prepend(QChar(0x2026));
        if (snippetEnd < text.size()) snippet.append(QChar(0x2026));
        return snippet;
    };

    for (const auto& match : matches) {
        const auto& hit = match.hit;
        NavigationResult result;
        if (const auto* paragraph =
                std::get_if<BodyParagraphSearchHit>(&hit.target)) {
            result.title = tr("Paragraph %1").arg(
                static_cast<qulonglong>(match.containerOrdinal));
            result.snippet = snippetFor(
                match.containerText, paragraph->startUtf16,
                paragraph->endUtf16);
        } else {
            const auto& cell = std::get<TableCellSearchHit>(hit.target);
            result.title = tr("Table %1, row %2, column %3")
                               .arg(static_cast<qulonglong>(
                                   match.containerOrdinal))
                               .arg(static_cast<qulonglong>(cell.row + 1))
                               .arg(static_cast<qulonglong>(cell.column + 1));
            result.snippet = snippetFor(
                match.containerText, cell.startUtf16, cell.endUtf16);
        }
        result.accessibleText = result.title + QStringLiteral(": ") +
                                result.snippet;
        if (current && *current == hit) {
            currentIndex = static_cast<int>(results.size());
        }
        results.push_back(std::move(result));
    }
    navigation_->setSearchResults(results, currentIndex);
}

void MainWindow::synchronizeNavigationSelection() {
    if (!navigation_ || !navigation_->isVisible()) return;
    const auto* state = activeState();
    const auto* canvas = activeCanvas();
    if (!state || !canvas || state->navigationHits.empty()) {
        navigation_->setCurrentResultIndex(-1);
        return;
    }
    const auto current = canvas->currentSearchHit();
    int currentIndex = -1;
    if (current) {
        const auto found = std::find(
            state->navigationHits.begin(), state->navigationHits.end(),
            *current);
        if (found != state->navigationHits.end()) {
            currentIndex = static_cast<int>(std::distance(
                state->navigationHits.begin(), found));
        }
    }
    navigation_->setCurrentResultIndex(currentIndex);
}

void MainWindow::activateNavigationResult(int index) {
    auto* canvas = activeCanvas();
    const auto* state = activeState();
    if (!navigation_ || !canvas || !state || index < 0) return;
    if (static_cast<std::size_t>(index) >= state->navigationHits.size() ||
        !canvas->activateSearchHit(
            state->navigationHits[static_cast<std::size_t>(index)])) {
        refreshNavigationResults();
        return;
    }
    navigation_->setCurrentResultIndex(index);
}

void MainWindow::navigateSearchResult(bool forward) {
    auto* canvas = activeCanvas();
    if (!navigation_ || !canvas) return;
    const auto options = DocumentSearchOptions{
        navigation_->matchCase(), navigation_->wholeWords()};
    const auto activated = forward
        ? canvas->findNextHit(navigation_->query(), options)
        : canvas->findPreviousHit(navigation_->query(), options);
    if (!activated) {
        statusBar()->showMessage(tr("No match"), 2500);
        refreshNavigationResults();
        return;
    }
    refreshNavigationResults();
}

void MainWindow::showCommandPalette() {
    QDialog dialog(this); dialog.setWindowTitle(tr("Command Palette")); dialog.resize(480, 420);
    QVBoxLayout layout(&dialog); QLineEdit search; QListWidget list;
    search.setPlaceholderText(tr("Type a command…")); layout.addWidget(&search); layout.addWidget(&list);
    auto refill = [&] {
        list.clear(); for (const auto& id : commands_.search(search.text())) {
            auto* action = commands_.action(id); auto* item = new QListWidgetItem(action->text(), &list); item->setData(Qt::UserRole, id);
        }
        if (list.count()) list.setCurrentRow(0);
    };
    connect(&search, &QLineEdit::textChanged, &dialog, refill);
    connect(&list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem* item) {
        if (auto* action = commands_.action(item->data(Qt::UserRole).toString()))
            action->trigger();
        dialog.accept();
    });
    refill(); search.setFocus(); dialog.exec();
}

void MainWindow::showEditorOptions() {
    const auto requested = requestEditorPreferences(this, editorPreferences_);
    if (!requested) return;

    QSettings settings;
    QString error;
    if (!requested->save(settings, &error)) {
        QMessageBox::warning(
            this, tr("Options could not be saved"),
            error.isEmpty()
                ? tr("Owl Docs could not save the editor options.")
                : error);
        return;
    }

    editorPreferences_ = *requested;
    applyEditorPreferences();
    if (auto* canvas = activeCanvas()) {
        restoreCanvasFocus(canvas);
    }
}

void MainWindow::applyEditorPreferences() {
    for (const auto& entry : states_) {
        auto* canvas = entry.first;
        if (!canvas) continue;
        canvas->setEditorDefaults(editorPreferences_.defaultFontFamily(),
                                  editorPreferences_.defaultFontPointSize(),
                                  editorPreferences_.tabWidthSpaces());
        canvas->setDefaultListLayout(editorPreferences_.defaultListLayout());
    }
}

void MainWindow::showListProperties() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    const auto current = canvas->currentListLayout();
    if (!current) {
        statusBar()->showMessage(
            tr("Place the cursor in a bulleted or numbered list first."), 4000);
        restoreCanvasFocus(canvas);
        return;
    }

    ListPropertiesDialog dialog(this);
    dialog.setDefaultProperties(
        listPropertiesFromLayout(editorPreferences_.defaultListLayout()));
    dialog.setProperties(listPropertiesFromLayout(*current));
    if (dialog.exec() == QDialog::Accepted) {
        const core::ListLayout layout =
            listLayoutFromProperties(dialog.properties());
        static_cast<void>(canvas->setCurrentListLayout(layout));

        if (dialog.useAsDefaultsRequested()) {
            EditorPreferences requestedPreferences = editorPreferences_;
            static_cast<void>(requestedPreferences.setDefaultListLayout(layout));
            QSettings settings;
            QString error;
            if (!requestedPreferences.save(settings, &error)) {
                QMessageBox::warning(
                    this, tr("Defaults could not be saved"),
                    error.isEmpty()
                        ? tr("Owl Docs could not save the list defaults.")
                        : error);
            } else {
                editorPreferences_ = std::move(requestedPreferences);
                applyEditorPreferences();
                statusBar()->showMessage(
                    tr("List defaults updated for new lists"), 4000);
            }
        }
    }
    restoreCanvasFocus(canvas);
}

void MainWindow::insertTable() {
    auto* canvas = activeCanvas();
    if (!canvas) return;

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("insertTableDialog"));
    dialog.setWindowTitle(tr("Insert Table"));
    dialog.setModal(true);
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;

    auto* rows = new QSpinBox(&dialog);
    rows->setObjectName(QStringLiteral("insertTable.rows"));
    rows->setRange(1, static_cast<int>(core::Table::maximum_rows));
    rows->setValue(2);
    rows->setAccessibleName(tr("Table rows"));
    form->addRow(tr("Rows:"), rows);

    auto* columns = new QSpinBox(&dialog);
    columns->setObjectName(QStringLiteral("insertTable.columns"));
    columns->setRange(1, static_cast<int>(core::Table::maximum_columns));
    columns->setValue(2);
    columns->setAccessibleName(tr("Table columns"));
    form->addRow(tr("Columns:"), columns);

    auto* header = new QCheckBox(tr("Header row"), &dialog);
    header->setObjectName(QStringLiteral("insertTable.header"));
    header->setChecked(true);
    form->addRow(QString(), header);
    outer->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("insertTable.buttons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted,
            &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            &dialog, &QDialog::reject);
    rows->setFocus();
    rows->selectAll();
    if (dialog.exec() == QDialog::Accepted) {
        static_cast<void>(canvas->insertTable(
            static_cast<std::size_t>(rows->value()),
            static_cast<std::size_t>(columns->value()),
            header->isChecked()));
    }
    canvas->setFocus();
}

void MainWindow::insertEquation() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("insertEquationDialog"));
    dialog.setWindowTitle(tr("Insert LaTeX Equation"));
    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* latexInput = new QLineEdit(QStringLiteral("\\frac{a}{b}"), &dialog);
    latexInput->setObjectName(QStringLiteral("insertEquation.latex"));
    latexInput->setAccessibleName(tr("LaTeX equation"));
    form->addRow(tr("Safe LaTeX subset:"), latexInput);
    auto* display = new QCheckBox(tr("Display equation"), &dialog);
    display->setObjectName(QStringLiteral("insertEquation.display"));
    form->addRow(QString(), display);
    outer->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("insertEquation.buttons"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    latexInput->setFocus();
    latexInput->selectAll();
    if (dialog.exec() != QDialog::Accepted || latexInput->text().isEmpty()) {
        canvas->setFocus();
        return;
    }
    const auto latex = latexInput->text();
    const QByteArray encoded = latex.toUtf8();
    const auto parsed = math::parseLatex(std::string_view(
        encoded.constData(), static_cast<std::size_t>(encoded.size())),
        editorEquationLimits());
    if (!parsed) {
        QMessageBox::warning(
            this, tr("Invalid equation"),
            tr("%1 (at byte %2)\n\nOnly the built-in, non-executing LaTeX math subset is accepted.")
                .arg(fromUtf8(parsed.error().message))
                .arg(static_cast<qulonglong>(parsed.error().byte_offset)));
        canvas->setFocus();
        return;
    }
    static_cast<void>(canvas->insertEquation(
        fromUtf8(math::toCanonicalLatex(parsed.value())),
        display->isChecked()));
    canvas->setFocus();
}

void MainWindow::insertImage() {
    auto* canvas = activeCanvas();
    if (!canvas) return;
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Insert Picture"), {},
        tr("PNG and JPEG pictures (*.png *.jpg *.jpeg)"));
    if (path.isEmpty()) {
        canvas->setFocus();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(
            this, tr("Picture could not be inserted"),
            tr("The selected file could not be opened for reading."));
        canvas->setFocus();
        return;
    }
    const qint64 maximum = static_cast<qint64>(
        core::kMaximumEncodedImageBytes);
    const QByteArray encoded = file.read(maximum + 1);
    if (encoded.isEmpty() || encoded.size() > maximum || !file.atEnd() ||
        file.error() != QFileDevice::NoError) {
        QMessageBox::warning(
            this, tr("Picture could not be inserted"),
            tr("The selected file is empty, could not be read completely, or exceeds the 16 MiB encoded-picture limit."));
        canvas->setFocus();
        return;
    }
    std::vector<std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size());
    if (canvas->insertInlineImage(
            std::move(bytes), QFileInfo(path).fileName())) {
        statusBar()->showMessage(
            tr("Picture inserted in line with text"), 4000);
    }
    canvas->setFocus();
}

void MainWindow::insertExcalidrawFigure() {
    auto* canvas = activeCanvas();
    if (!canvas || !figureEditor_) return;
    QPointer<DocumentCanvas> guardedCanvas(canvas);
    figureEditor_->open(
        std::nullopt,
        [this, guardedCanvas](
            std::optional<ExcalidrawFigureEditor::Result> result,
            const QString& error) {
            if (!error.isEmpty()) {
                QMessageBox::warning(
                    this, tr("Figure editor"), error);
                return;
            }
            if (!result || !guardedCanvas) return;
            std::vector<std::uint8_t> png(
                reinterpret_cast<const std::uint8_t*>(
                    result->png.constData()),
                reinterpret_cast<const std::uint8_t*>(
                    result->png.constData()) + result->png.size());
            if (guardedCanvas->insertInlineImage(
                    std::move(png), tr("Excalidraw figure"))) {
                statusBar()->showMessage(
                    tr("Editable Excalidraw figure inserted"), 4000);
            }
            guardedCanvas->setFocus();
        });
}

void MainWindow::editSelectedExcalidrawFigure(DocumentCanvas* canvas) {
    if (!canvas || !figureEditor_) return;
    const auto scene = canvas->selectedExcalidrawScene();
    if (!scene) return;
    const auto imageId = canvas->selectedInlineImageId();
    QPointer<DocumentCanvas> guardedCanvas(canvas);
    figureEditor_->open(
        *scene,
        [this, guardedCanvas, imageId](
            std::optional<ExcalidrawFigureEditor::Result> result,
            const QString& error) {
            if (!error.isEmpty()) {
                QMessageBox::warning(
                    this, tr("Figure editor"), error);
                return;
            }
            if (!result || !guardedCanvas || !imageId ||
                !guardedCanvas->selectInlineImage(*imageId)) {
                return;
            }
            std::vector<std::uint8_t> png(
                reinterpret_cast<const std::uint8_t*>(
                    result->png.constData()),
                reinterpret_cast<const std::uint8_t*>(
                    result->png.constData()) + result->png.size());
            if (guardedCanvas->replaceSelectedExcalidrawFigure(
                    std::move(png))) {
                statusBar()->showMessage(
                    tr("Excalidraw figure updated"), 4000);
            }
            guardedCanvas->setFocus();
        });
}

void MainWindow::showCompatibilityReport() {
    const auto* state = activeState();
    if (!state || !state->package) { QMessageBox::information(this, tr("Compatibility Report"), tr("New Owl Docs document: supported body-text subset.")); return; }
    const auto& report = state->package->compatibility();
    QString detail;
    for (const auto& issue : report.issues) detail += fromUtf8(issue.part_name + ": " + issue.detail) + QLatin1Char('\n');
    QMessageBox box(QMessageBox::Information, tr("Compatibility Report"),
                    tr("Classification: %1\nUnchanged Save As is byte-identical: %2")
                        .arg(compatibilityClassName(report.classification), report.byte_identical_unchanged_save ? tr("Yes") : tr("No")),
                    QMessageBox::Ok, this);
    box.setDetailedText(detail.isEmpty() ? tr("No compatibility issues were reported for the supported mapper.") : detail); box.exec();
}

void MainWindow::updateTabTitle(DocumentCanvas* canvas) {
    const int index = tabs_->indexOf(canvas); if (index < 0) return;
    const auto* state = stateFor(canvas);
    QString base = tr("Untitled");
    if (state && !state->path.isEmpty()) {
        base = QFileInfo(state->path).fileName();
    } else if (state && state->recovered) {
        base = tr("Recovered — %1").arg(
            state->recoveryDisplayName.isEmpty() ? tr("Untitled")
                                                 : state->recoveryDisplayName);
    }
    tabs_->setTabText(index, canvas->isModified() ? base + QStringLiteral(" *") : base); updateWindowTitle();
}

void MainWindow::updateWindowTitle() {
    const auto* state = activeState();
    QString base = tr("Untitled");
    if (state && !state->path.isEmpty()) {
        base = QFileInfo(state->path).fileName();
    } else if (state && state->recovered) {
        base = tr("Recovered — %1").arg(
            state->recoveryDisplayName.isEmpty() ? tr("Untitled")
                                                 : state->recoveryDisplayName);
    }
    setWindowTitle(QStringLiteral("%1 — Owl Docs").arg(base));
}

void MainWindow::addRecentFile(const QString& path) {
    const QString normalized = normalizedAbsolutePath(path);
    QSettings settings;
    auto recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&normalized](const QString& existing) {
                                    return sameFileDestination(existing,
                                                               normalized);
                                }),
                 recent.end());
    recent.push_front(normalized);
    while (recent.size() > 10) recent.removeLast();
    settings.setValue(QStringLiteral("recentFiles"), recent);
    settings.sync();
    QTimer::singleShot(0, this, &MainWindow::rebuildRecentMenu);
}

void MainWindow::removeRecentFile(const QString& path) {
    QSettings settings;
    auto recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [&path](const QString& existing) {
                                    return sameFileDestination(existing, path);
                                }),
                 recent.end());
    settings.setValue(QStringLiteral("recentFiles"), recent);
    settings.sync();
    QTimer::singleShot(0, this, &MainWindow::rebuildRecentMenu);
}

void MainWindow::removeMissingRecentFiles() {
    QSettings settings;
    auto recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    const auto previousSize = recent.size();
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [](const QString& path) {
                                    return !isExistingRegularFile(path);
                                }),
                 recent.end());
    settings.setValue(QStringLiteral("recentFiles"), recent);
    settings.sync();
    QTimer::singleShot(0, this, &MainWindow::rebuildRecentMenu);
    const qsizetype removed = previousSize - recent.size();
    statusBar()->showMessage(
        removed == 1 ? tr("Removed 1 unavailable recent document")
                     : tr("Removed %1 unavailable recent documents")
                           .arg(removed),
        4000);
}

void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    QSettings settings;
    const auto stored = settings.value(QStringLiteral("recentFiles")).toStringList();
    QStringList recent;
    for (const auto& storedPath : stored) {
        if (storedPath.isEmpty()) continue;
        const QString path = normalizedAbsolutePath(storedPath);
        const bool duplicate = std::any_of(
            recent.cbegin(), recent.cend(), [&path](const QString& existing) {
                return sameFileDestination(existing, path);
            });
        if (!duplicate) recent.push_back(path);
        if (recent.size() == 10) break;
    }
    if (recent != stored) {
        settings.setValue(QStringLiteral("recentFiles"), recent);
        settings.sync();
    }

    if (recent.isEmpty()) {
        auto* empty = recentMenu_->addAction(tr("No recent documents"));
        empty->setObjectName(QStringLiteral("recent.empty"));
        empty->setEnabled(false);
        return;
    }

    std::map<QString, int> basenameCounts;
    for (const auto& path : recent) {
        ++basenameCounts[QFileInfo(path).fileName().toCaseFolded()];
    }
    bool hasUnavailable = false;
    for (qsizetype index = 0; index < recent.size(); ++index) {
        const QString path = recent[index];
        const QFileInfo info(path);
        QString visible = info.fileName();
        if (basenameCounts[visible.toCaseFolded()] > 1) {
            visible += tr(" — %1").arg(
                QDir::toNativeSeparators(info.absolutePath()));
        }
        const QString ordinal = index < 9
                                    ? QStringLiteral("&%1").arg(index + 1)
                                    : QStringLiteral("%1").arg(index + 1);
        auto* action = recentMenu_->addAction(
            QStringLiteral("%1 %2")
                .arg(ordinal, escapedMenuLabel(visible)));
        action->setObjectName(
            QStringLiteral("recent.open.%1").arg(index + 1));
        action->setData(path);
        action->setToolTip(QDir::toNativeSeparators(path));
        connect(action, &QAction::triggered, this, [this, path] {
            if (!isExistingRegularFile(path)) {
                removeRecentFile(path);
                statusBar()->showMessage(
                    tr("Removed unavailable recent document: %1")
                        .arg(QDir::toNativeSeparators(path)),
                    5000);
                return;
            }
            openPath(path);
        });
        hasUnavailable = hasUnavailable || !isExistingRegularFile(path);
    }

    recentMenu_->addSeparator();
    auto* removeMissing = recentMenu_->addAction(
        tr("Remove Missing Documents"), this,
        &MainWindow::removeMissingRecentFiles);
    removeMissing->setObjectName(QStringLiteral("recent.removeMissing"));
    removeMissing->setEnabled(hasUnavailable);

    auto* clear = recentMenu_->addAction(tr("Clear Recent Documents"));
    clear->setObjectName(QStringLiteral("recent.clear"));
    connect(clear, &QAction::triggered, this, [this] {
        QSettings recentSettings;
        recentSettings.remove(QStringLiteral("recentFiles"));
        recentSettings.sync();
        QTimer::singleShot(0, this, &MainWindow::rebuildRecentMenu);
        statusBar()->showMessage(tr("Recent documents cleared"), 3000);
    });
}

DocumentCanvas* MainWindow::canvasForPath(const QString& path) const {
    for (const auto& [canvas, state] : states_) {
        if (state && !state->path.isEmpty() &&
            sameFileDestination(state->path, path)) {
            return canvas;
        }
    }
    return nullptr;
}

DocumentCanvas* MainWindow::replaceableUntitledCanvas() const {
    for (const auto& [canvas, state] : states_) {
        if (!state || !state->replaceOnSuccessfulOpen || state->recovered ||
            state->package || !state->path.isEmpty() || canvas->isModified() ||
            canvas->hasPreview()) {
            continue;
        }
        const auto snapshot = canvas->snapshot();
        const auto& paragraphs = snapshot.document.paragraphs();
        if (paragraphs.size() == 1 && paragraphs.front().text().empty() &&
            paragraphs.front().equations().empty() &&
            paragraphs.front().images().empty() &&
            snapshot.document.tables().empty()) {
            return canvas;
        }
    }
    return nullptr;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if ((event->possibleActions() & Qt::CopyAction) &&
        !droppedDocumentPaths(event->mimeData()).paths.isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto selection = droppedDocumentPaths(event->mimeData());
    if (!(event->possibleActions() & Qt::CopyAction) ||
        selection.paths.isEmpty()) {
        event->ignore();
        return;
    }
    const int opened = openPaths(selection.paths);
    if (opened == 0) {
        event->ignore();
        statusBar()->showMessage(tr("No documents were opened"), 5000);
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    const int skipped = selection.rejected +
                        static_cast<int>(selection.paths.size()) - opened;
    if (skipped > 0) {
        const QString openedSummary =
            opened == 1 ? tr("Opened 1 document")
                        : tr("Opened %1 documents").arg(opened);
        const QString skippedSummary =
            skipped == 1 ? tr("skipped 1 unsupported item")
                         : tr("skipped %1 unsupported items").arg(skipped);
        statusBar()->showMessage(
            tr("%1; %2").arg(openedSummary, skippedSummary),
            5000);
    } else {
        statusBar()->showMessage(
            opened == 1 ? tr("Opened 1 document")
                        : tr("Opened %1 documents").arg(opened),
            3500);
    }
}

QString MainWindow::documentKey() const {
    return documentKeyFor(activeCanvas());
}

QString MainWindow::documentKeyFor(DocumentCanvas* canvas) const {
    const auto* state = stateFor(canvas);
    return state ? state->documentKey : QString();
}

DocumentCanvas* MainWindow::canvasForDocumentKey(const QString& key) const {
    if (key.isEmpty()) return nullptr;
    for (const auto& [canvas, state] : states_) {
        Q_UNUSED(state);
        if (documentKeyFor(canvas) == key) return canvas;
    }
    return nullptr;
}

codex::Json MainWindow::handleEditorTool(const QString& key,
                                         const QString& tool,
                                         const codex::Json& arguments,
                                         QString& error) {
    auto* canvas = canvasForDocumentKey(key);
    if (!canvas) {
        error = tr("The document for this Codex thread is no longer open.");
        return {};
    }
    const QString previewTool = QString::fromLatin1(
        codex::kEditorPreviewTool.data(),
        static_cast<qsizetype>(codex::kEditorPreviewTool.size()));
    if (tool == previewTool && !previewDocumentKey_.isEmpty() &&
        previewDocumentKey_ != key) {
        error = tr("A preview in another document is awaiting review. Accept or "
                   "discard it before creating another preview.");
        return {};
    }
    QString previewSummary;
    auto result = invokeEditorTool(*canvas, key, tool, arguments,
                                   previewSummary, error);
    if (error.isEmpty() && !previewSummary.isEmpty()) {
        previewDocumentKey_ = key;
        previewLabel_ = tr("Codex semantic edit preview");
        previewSummary_ = previewSummary;
        if (key == documentKey())
            chat_->showPreview(previewLabel_, previewSummary_);
        if (canvas != activeCanvas()) {
            statusBar()->showMessage(
                tr("Codex prepared a preview for %1; review it in the chat panel.")
                    .arg(QFileInfo(key).fileName()),
                7000);
        }
    }
    return result;
}

void MainWindow::checkpointCanvas(DocumentCanvas* canvas) {
    auto* state = stateFor(canvas);
    if (!recoveryOwner_ || !state || !canvas->isModified() || !chatStore_.isOpen()) return;

    const auto snapshot = canvas->snapshot();
    RecoveryDocument recovery{
        snapshot.document,
        {canvas->pageWidthPoints(), canvas->pageHeightPoints(),
         canvas->marginTopPoints(), canvas->marginRightPoints(),
         canvas->marginBottomPoints(), canvas->marginLeftPoints()}};
    std::string error;
    const auto payload = RecoveryCodec::encode(recovery, error);
    if (!payload) {
        statusBar()->showMessage(
            tr("Recovery checkpoint paused: %1").arg(fromUtf8(error)), 7000);
        return;
    }

    QString sourcePath = state->path;
    if (sourcePath.isEmpty()) sourcePath = state->recoverySourcePath;
    QString displayName;
    if (!state->path.isEmpty()) {
        displayName = QFileInfo(state->path).fileName();
    } else if (!state->recoveryDisplayName.isEmpty()) {
        displayName = state->recoveryDisplayName;
    } else {
        displayName = tr("Untitled document");
    }
    const RecoveryRecord record{
        state->journalKey.toStdString(), sourcePath.toUtf8().toStdString(),
        displayName.toUtf8().toStdString(), *payload, 0};
    if (!chatStore_.upsertRecovery(record, error)) {
        statusBar()->showMessage(
            tr("Recovery checkpoint paused: %1").arg(fromUtf8(error)), 7000);
    }
}

void MainWindow::checkpointModifiedDocuments() {
    for (const auto& [canvas, state] : states_) {
        Q_UNUSED(state);
        checkpointCanvas(canvas);
    }
}

void MainWindow::deleteRecoveryFor(DocumentCanvas* canvas) {
    auto* state = stateFor(canvas);
    if (!recoveryOwner_ || !state || state->journalKey.isEmpty() || !chatStore_.isOpen()) return;
    std::string error;
    if (!chatStore_.deleteRecovery(state->journalKey.toStdString(), error)) {
        statusBar()->showMessage(
            tr("Could not clear the recovery checkpoint: %1").arg(fromUtf8(error)),
            7000);
    }
}

void MainWindow::restoreRecoveryJournals() {
    if (!recoveryOwner_ || !chatStore_.isOpen()) return;
    const auto records = chatStore_.recoveryRecords();
    if (records.empty()) return;

    QStringList names;
    for (const auto& record : records) {
        const auto name = fromUtf8(record.displayName);
        names.push_back(name.isEmpty() ? tr("Untitled document") : name);
    }
    QMessageBox prompt(QMessageBox::Warning, tr("Recover documents?"),
                       tr("Owl Docs found %1 autosaved document(s) from an "
                          "earlier session.")
                           .arg(records.size()),
                       QMessageBox::NoButton, this);
    prompt.setInformativeText(
        tr("Recovered documents open as unsaved copies and must be saved with "
           "Save As. Original DOCX files will not be overwritten."));
    prompt.setDetailedText(names.join(QLatin1Char('\n')));
    auto* restoreButton = prompt.addButton(tr("Restore"), QMessageBox::AcceptRole);
    auto* laterButton = prompt.addButton(tr("Later"), QMessageBox::RejectRole);
    auto* discardButton = prompt.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    prompt.setDefaultButton(restoreButton);
    prompt.exec();
    if (prompt.clickedButton() == laterButton || prompt.clickedButton() == nullptr) return;
    if (prompt.clickedButton() == discardButton) {
        std::string error;
        for (const auto& record : records)
            static_cast<void>(chatStore_.deleteRecovery(record.journalKey, error));
        return;
    }
    if (prompt.clickedButton() != restoreButton) return;

    DocumentCanvas* placeholder = nullptr;
    if (tabs_->count() == 1) {
        auto* candidate = qobject_cast<DocumentCanvas*>(tabs_->widget(0));
        const auto* candidateState = stateFor(candidate);
        if (candidate && candidateState && candidateState->path.isEmpty() &&
            !candidateState->recovered && !candidate->isModified()) {
            const auto snapshot = candidate->snapshot();
            if (snapshot.document.paragraphs().size() == 1 &&
                snapshot.document.paragraphs().front().text().empty())
                placeholder = candidate;
        }
    }

    int restoredCount = 0;
    QStringList failures;
    for (const auto& record : records) {
        std::string error;
        auto recovery = RecoveryCodec::decode(record.payload, error);
        if (!recovery) {
            failures.push_back(
                tr("%1: %2").arg(fromUtf8(record.displayName), fromUtf8(error)));
            continue;
        }
        auto state = std::make_unique<TabState>();
        state->recoverySourcePath = fromUtf8(record.sourcePath);
        state->recoveryDisplayName = fromUtf8(record.displayName);
        state->journalKey = fromUtf8(record.journalKey);
        state->baselineTexts = currentTexts(
            {core::Revision{}, recovery->document});
        // A recovery snapshot contains the editable semantic projection, not
        // opaque package parts. Recovered imported documents therefore retain
        // the guarded/simplified Save As policy instead of silently claiming a
        // lossless native save.
        state->regeneratable = state->recoverySourcePath.isEmpty();
        state->recovered = true;
        const auto page = recovery->page;
        const auto title = tr("Recovered — %1").arg(
            state->recoveryDisplayName.isEmpty() ? tr("Untitled")
                                                 : state->recoveryDisplayName);
        auto* canvas = createDocumentTab(std::move(recovery->document),
                                         std::move(state), title);
        // Raster payloads live in semantic image atoms. They are decoded lazily
        // by DocumentCanvas under the same bounded presentation-cache policy
        // used for newly inserted and imported pictures.
        canvas->setImportedPresentation({}, {});
        canvas->setImportedPageLayout(
            page.width_points, page.height_points, page.margin_top_points,
            page.margin_right_points, page.margin_bottom_points,
            page.margin_left_points);
        canvas->markRecovered();
        ++restoredCount;
    }
    if (restoredCount > 0 && placeholder) {
        const int index = tabs_->indexOf(placeholder);
        if (index >= 0) tabs_->removeTab(index);
        states_.erase(placeholder);
        delete placeholder;
    }
    if (!failures.isEmpty()) {
        QMessageBox warning(QMessageBox::Warning, tr("Some recovery data was unreadable"),
                            tr("Unreadable checkpoints were left in local storage."),
                            QMessageBox::Ok, this);
        warning.setDetailedText(failures.join(QLatin1Char('\n')));
        warning.exec();
    }
}

void MainWindow::loadChatForActiveDocument() {
    if (!chat_) return;
    const QString key = documentKey();
    chat_->clearConversation();
    if (!key.isEmpty()) {
        for (const auto& message : chatStore_.messages(key.toStdString())) {
            chat_->appendStoredMessage(QString::fromStdString(message.role),
                                       QString::fromStdString(message.text));
        }
    }
    if (key == assistantDocumentKey_ && !assistantForStore_.isEmpty()) {
        chat_->beginAssistantMessage();
        chat_->appendAssistantDelta(assistantForStore_);
    }
    if (key == previewDocumentKey_ && activeCanvas() && activeCanvas()->hasPreview()) {
        chat_->showPreview(previewLabel_, previewSummary_);
    } else {
        chat_->hidePreview();
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (int index = tabs_->count() - 1; index >= 0; --index) {
        if (!maybeCloseTab(index)) { event->ignore(); return; }
    }
    // Defer deleting checkpoints for discarded tabs until every close prompt
    // has succeeded. A later Cancel must leave the remaining recovery data
    // intact.
    for (const auto& [canvas, state] : states_) {
        Q_UNUSED(state);
        deleteRecoveryFor(canvas);
    }
    codex_->shutdown(); event->accept();
}

}  // namespace docxstudio::app
