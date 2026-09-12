#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>

#include <zip.h>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString textOf(const docxstudio::app::DocumentCanvas& canvas) {
    const auto snapshot = canvas.snapshot();
    const auto& text = snapshot.document.paragraphs().front().text();
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "could not read DOCX fixture");
    return file.readAll();
}

std::vector<std::uint8_t> solidPng(const QColor& color) {
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not encode synthetic inline image");
    return {reinterpret_cast<const std::uint8_t*>(encoded.constData()),
            reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
                encoded.size()};
}

QRect saturatedColorBounds(const QImage& image, bool red) {
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const bool matches = red
                ? color.red() > 220 && color.green() < 40 &&
                      color.blue() < 40
                : color.blue() > 220 && color.red() < 40 &&
                      color.green() < 40;
            if (!matches) continue;
            result = result.isValid() ? result.united(QRect(x, y, 1, 1))
                                      : QRect(x, y, 1, 1);
        }
    }
    return result;
}

QRect cursorRect(docxstudio::app::DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRect();
}

void replacePackageMembers(
    const QString& path,
    const std::vector<std::pair<std::string, std::string>>& members) {
    int error = 0;
    const QByteArray encodedPath = QFile::encodeName(path);
    zip_t* archive = zip_open(encodedPath.constData(), 0, &error);
    check(archive != nullptr, "could not update synthetic DOCX fixture");
    for (const auto& [name, contents] : members) {
        void* owned = nullptr;
        if (!contents.empty()) {
            owned = std::malloc(contents.size());
            check(owned != nullptr, "could not allocate synthetic ZIP member");
            std::memcpy(owned, contents.data(), contents.size());
        }
        zip_source_t* source = zip_source_buffer(
            archive, owned, static_cast<zip_uint64_t>(contents.size()), 1);
        if (!source) std::free(owned);
        check(source != nullptr, "could not create synthetic ZIP source");
        if (zip_file_add(
                archive, name.c_str(), source,
                ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(source);
            zip_discard(archive);
            check(false, "could not replace synthetic ZIP member");
        }
    }
    check(zip_close(archive) == 0,
          "could not finish synthetic DOCX fixture");
}

void createNativeStyleAndNumberingFixture(const QString& path) {
    docxstudio::ooxml::NewParagraph seed;
    seed.runs.push_back({"seed", {}});
    const auto initial = docxstudio::ooxml::DocxDocument::writeNew(
        std::filesystem::path(QFile::encodeName(path).constData()), {seed});
    check(static_cast<bool>(initial),
          "could not create base synthetic DOCX fixture");

    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"SampleHeading\"/></w:pPr>"
        "<w:r><w:t>Heading</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"4\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Alpha</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"4\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Beta</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    const std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:asciiTheme=\"minorHAnsi\" w:hAnsiTheme=\"minorHAnsi\"/>"
        "<w:sz w:val=\"20\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:rPr><w:b w:val=\"0\"/></w:rPr>"
        "</w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"SampleHeading\">"
        "<w:name w:val=\"Sample Heading\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:jc w:val=\"center\"/></w:pPr>"
        "<w:rPr><w:rFonts w:ascii=\"Fallback Serif\" "
        "w:hAnsi=\"Fallback Serif\" w:asciiTheme=\"majorHAnsi\" "
        "w:hAnsiTheme=\"majorHAnsi\"/><w:b/></w:rPr></w:style>"
        "</w:styles>";
    const std::string numbering =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"2\"><w:lvl w:ilvl=\"0\">"
        "<w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
        "<w:lvlText w:val=\"%1.\"/><w:pPr><w:tabs>"
        "<w:tab w:val=\"num\" w:pos=\"720\"/></w:tabs>"
        "<w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
        "</w:lvl></w:abstractNum><w:num w:numId=\"4\">"
        "<w:abstractNumId w:val=\"2\"/></w:num></w:numbering>";
    const std::string theme =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<a:themeElements><a:clrScheme name=\"Synthetic\">"
        "<a:dk1><a:srgbClr val=\"000000\"/></a:dk1>"
        "</a:clrScheme><a:fontScheme name=\"Synthetic\">"
        "<a:majorFont><a:latin typeface=\"Theme Serif\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Theme Sans\"/></a:minorFont>"
        "</a:fontScheme></a:themeElements></a:theme>";
    const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>"
        "</Relationships>";
    replacePackageMembers(
        path,
        {{"word/document.xml", document}, {"word/styles.xml", styles},
         {"word/numbering.xml", numbering},
         {"word/theme/theme1.xml", theme},
         {"word/_rels/document.xml.rels", relationships}});
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory failed");
    const QString path = temporary.filePath(QStringLiteral("styled.docx"));

    docxstudio::ooxml::BasicRunFormat format;
    format.font_family = "Carlito";
    format.font_size_half_points = 28;
    format.bold = true;
    format.italic = true;
    format.underline = true;
    format.foreground_rgb = 0x00336699U;
    format.highlight_rgb = 0x00ffee88U;
    format.baseline = docxstudio::ooxml::BasicBaseline::subscript;
    docxstudio::ooxml::NewParagraph paragraph;
    paragraph.alignment = docxstudio::ooxml::BasicParagraphAlignment::center;
    paragraph.left_indent_twips = 720;
    paragraph.right_indent_twips = 360;
    paragraph.first_line_indent_twips = -240;
    paragraph.space_before_twips = 120;
    paragraph.space_after_twips = 180;
    paragraph.line_spacing = 360;
    paragraph.line_spacing_rule = docxstudio::ooxml::BasicLineSpacingRule::exact;
    paragraph.keep_with_next = true;
    paragraph.keep_lines = false;
    paragraph.page_break_before = true;
    paragraph.runs.push_back({"Styled text", format});
    docxstudio::ooxml::PageSettings page;
    page.width_twips = 11906;
    page.height_twips = 16838;
    page.margin_top_twips = 1000;
    page.margin_right_twips = 1100;
    page.margin_bottom_twips = 1200;
    page.margin_left_twips = 1300;
    const auto saved = docxstudio::ooxml::DocxDocument::writeNew(
        std::filesystem::path(QFile::encodeName(path).constData()),
        {paragraph}, page);
    check(static_cast<bool>(saved), "styled DOCX fixture could not be written");

    docxstudio::app::MainWindow window;
    check(window.openPath(path), "desktop shell could not open a valid DOCX");
    docxstudio::app::DocumentCanvas* imported = nullptr;
    for (auto* canvas : window.findChildren<docxstudio::app::DocumentCanvas*>()) {
        if (textOf(*canvas) == QStringLiteral("Styled text")) {
            imported = canvas;
            break;
        }
    }
    check(imported != nullptr, "imported paragraph was not mapped to the custom engine");
    const auto snapshot = imported->snapshot();
    const auto& mapped = snapshot.document.paragraphs().front();
    const auto mappedFormat = mapped.characterFormatAt(1);
    check(mappedFormat.bold.value_or(false), "bold was flattened during import");
    check(mappedFormat.italic.value_or(false), "italic was flattened during import");
    check(mappedFormat.underline.value_or(docxstudio::core::UnderlineStyle::none) ==
              docxstudio::core::UnderlineStyle::single,
          "underline was flattened during import");
    check(mappedFormat.font_size_half_points == 28,
          "font size was flattened during import");
    check(mappedFormat.foreground_argb == 0xff336699U,
          "font color was flattened during import");
    check(mappedFormat.highlight_argb == 0xffffee88U,
          "highlight color was flattened during import");
    check(mappedFormat.baseline == docxstudio::core::BaselinePosition::subscript,
          "subscript was flattened during import");
    check(mapped.format().alignment == docxstudio::core::ParagraphAlignment::center,
          "paragraph alignment was flattened during import");
    check(mapped.format().left_indent_emu == 720LL * 635LL,
          "left indent was flattened during import");
    check(mapped.format().right_indent_emu == 360LL * 635LL,
          "right indent was flattened during import");
    check(mapped.format().first_line_indent_emu == -240LL * 635LL,
          "hanging indent was flattened during import");
    check(mapped.format().space_before_emu == 120LL * 635LL,
          "space-before was flattened during import");
    check(mapped.format().space_after_emu == 180LL * 635LL,
          "space-after was flattened during import");
    check(mapped.format().line_spacing_emu == 360LL * 635LL,
          "line spacing was flattened during import");
    check(mapped.format().line_spacing_rule == docxstudio::core::LineSpacingRule::exact,
          "line spacing rule was flattened during import");
    check(mapped.format().keep_with_next == true,
          "keep-with-next was flattened during import");
    check(mapped.format().keep_lines == false,
          "explicit keep-lines false was flattened during import");
    check(mapped.format().page_break_before == true,
          "page-break-before was flattened during import");
    check(!imported->isModified(), "opening a DOCX marked it modified");
    check(std::abs(imported->pageWidthPoints() - 595.3) < 0.01,
          "page width was not imported");
    check(std::abs(imported->pageHeightPoints() - 841.9) < 0.01,
          "page height was not imported");
    check(std::abs(imported->marginLeftPoints() - 65.0) < 0.01,
          "page margins were not imported");

    imported->selectAll();
    imported->insertText(QStringLiteral("Local unsaved edit"));
    check(imported->isModified(), "local conflict fixture was not modified");

    docxstudio::ooxml::NewParagraph externalParagraph;
    externalParagraph.runs.push_back({"Externally changed", {}});
    const auto externalSave = docxstudio::ooxml::DocxDocument::writeNew(
        std::filesystem::path(QFile::encodeName(path).constData()),
        {externalParagraph}, page);
    check(static_cast<bool>(externalSave),
          "could not create external-change fixture");
    const QByteArray externalBytes = readFile(path);

    auto* saveAction = window.findChild<QAction*>(QStringLiteral("file.save"));
    check(saveAction != nullptr, "could not find the Save command");
    bool conflictWarningSeen = false;
    QTimer::singleShot(0, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (box && box->windowTitle() == QStringLiteral("Save As required")) {
                conflictWarningSeen = true;
                box->accept();
                return;
            }
        }
    });
    saveAction->trigger();
    check(conflictWarningSeen,
          "external modification did not produce a Save As warning");
    check(readFile(path) == externalBytes,
          "Save silently overwrote an externally modified DOCX");
    check(imported->isModified(),
          "blocked external-conflict save incorrectly marked the document saved");

    const QString nativeListPath = temporary.filePath(
        QStringLiteral("native-style-list.docx"));
    createNativeStyleAndNumberingFixture(nativeListPath);
    docxstudio::app::MainWindow nativeListWindow;
    check(nativeListWindow.openPath(nativeListPath),
          "desktop shell could not open native style/list DOCX");
    docxstudio::app::DocumentCanvas* nativeListCanvas = nullptr;
    for (auto* canvas :
         nativeListWindow.findChildren<docxstudio::app::DocumentCanvas*>()) {
        if (textOf(*canvas) == QStringLiteral("Heading")) {
            nativeListCanvas = canvas;
            break;
        }
    }
    check(nativeListCanvas != nullptr,
          "native style/list DOCX did not map to the editor");
    const auto nativeSnapshot = nativeListCanvas->snapshot();
    check(nativeSnapshot.document.paragraphs().size() == 3,
          "native style/list import changed paragraph count");
    const auto& heading = nativeSnapshot.document.paragraphs()[0];
    check(heading.format().alignment ==
              docxstudio::core::ParagraphAlignment::center &&
              heading.characterFormatAt(1).font_family == "Theme Serif" &&
              heading.characterFormatAt(1).bold == true,
          "paragraph style/theme presentation was flattened in the editor");
    const auto& firstListItem = nativeSnapshot.document.paragraphs()[1];
    const auto& secondListItem = nativeSnapshot.document.paragraphs()[2];
    const QString firstListText = QString::fromUtf16(
        firstListItem.text().data(),
        static_cast<qsizetype>(firstListItem.text().size()));
    const QString secondListText = QString::fromUtf16(
        secondListItem.text().data(),
        static_cast<qsizetype>(secondListItem.text().size()));
    const auto firstMarkerOffset = firstListText.indexOf(QStringLiteral("1.\t"));
    const auto secondMarkerOffset = secondListText.indexOf(QStringLiteral("2.\t"));
    check(firstMarkerOffset >= 0 && secondMarkerOffset == firstMarkerOffset &&
              firstListText.endsWith(QStringLiteral("1.\tAlpha")) &&
              secondListText.endsWith(QStringLiteral("2.\tBeta")) &&
              firstListItem.format().list_id.has_value() &&
              secondListItem.format().list_id ==
                  firstListItem.format().list_id &&
              firstListItem.format().list_level == 0 &&
              firstListItem.format().list_layout &&
              firstListItem.format().list_layout->levels[0]
                      .bullet_indent_spaces == firstMarkerOffset,
          "native numbering was not mapped to one semantic editor list");
    const auto firstBodyOffset = static_cast<std::size_t>(
        firstListText.indexOf(QStringLiteral("Alpha")) + 1);
    check(firstListItem.characterFormatAt(firstBodyOffset).font_family ==
                  "Theme Sans" &&
              firstListItem.characterFormatAt(firstBodyOffset)
                      .font_size_half_points == 20 &&
              firstListItem.characterFormatAt(firstBodyOffset).bold == false,
          "docDefaults or explicit false style formatting was flattened");

    check(nativeListCanvas->findNext(QStringLiteral("Alpha")),
          "could not select native list body text");
    nativeListCanvas->insertText(QStringLiteral("Edited"));
    auto* nativeSave = nativeListWindow.findChild<QAction*>(
        QStringLiteral("file.save"));
    check(nativeSave != nullptr, "could not find native-list Save command");
    nativeSave->trigger();
    check(!nativeListCanvas->isModified(),
          "safe native-list text patch did not establish a clean baseline");

    docxstudio::ooxml::Error nativeReopenError;
    auto nativeReopened = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(
            QFile::encodeName(nativeListPath).constData()),
        &nativeReopenError);
    check(nativeReopened != nullptr,
          "could not reopen safely patched native-list DOCX");
    check(nativeReopened->paragraphs().size() == 3 &&
              nativeReopened->paragraphs()[1].plainText() == "Edited" &&
              nativeReopened->paragraphs()[1].numbering &&
              nativeReopened->paragraphs()[1].numbering->marker_text == "1." &&
              nativeReopened->paragraphs()[2].numbering &&
              nativeReopened->paragraphs()[2].numbering->marker_text == "2.",
          "safe text patch duplicated or destroyed native numbering semantics");

    const QString inlineImagePath = temporary.filePath(
        QStringLiteral("mixed-inline-images.docx"));
    docxstudio::ooxml::NewInlineImage redImage;
    redImage.name = "red";
    redImage.width_emu = 36 * 12'700;
    redImage.height_emu = 24 * 12'700;
    redImage.bytes = solidPng(QColor(255, 0, 0));
    docxstudio::ooxml::NewInlineImage blueImage;
    blueImage.name = "blue";
    blueImage.width_emu = 28 * 12'700;
    blueImage.height_emu = 24 * 12'700;
    blueImage.bytes = solidPng(QColor(0, 0, 255));
    docxstudio::ooxml::NewParagraph mixedInlineParagraph;
    mixedInlineParagraph.runs.push_back({"BEFORE ", {}});
    docxstudio::ooxml::NewRun redRun;
    redRun.inline_image = std::move(redImage);
    mixedInlineParagraph.runs.push_back(std::move(redRun));
    mixedInlineParagraph.runs.push_back({" MIDDLE ", {}});
    docxstudio::ooxml::NewRun blueRun;
    blueRun.inline_image = std::move(blueImage);
    mixedInlineParagraph.runs.push_back(std::move(blueRun));
    mixedInlineParagraph.runs.push_back({" AFTER", {}});
    const auto mixedInlineSaved =
        docxstudio::ooxml::DocxDocument::writeNew(
            std::filesystem::path(
                QFile::encodeName(inlineImagePath).constData()),
            {mixedInlineParagraph});
    check(static_cast<bool>(mixedInlineSaved),
          "could not write mixed inline-image fixture");

    docxstudio::app::MainWindow inlineImageWindow;
    check(inlineImageWindow.openPath(inlineImagePath),
          "desktop shell could not open mixed inline-image DOCX");
    docxstudio::app::DocumentCanvas* inlineImageCanvas = nullptr;
    for (auto* canvas : inlineImageWindow.findChildren<
             docxstudio::app::DocumentCanvas*>()) {
        if (textOf(*canvas) ==
            QStringLiteral("BEFORE \ufffc MIDDLE \ufffc AFTER")) {
            inlineImageCanvas = canvas;
            break;
        }
    }
    check(inlineImageCanvas != nullptr,
          "mixed inline-image paragraph did not map to the editor");
    inlineImageWindow.resize(900, 520);
    inlineImageWindow.show();
    inlineImageCanvas->setFocus();
    QApplication::processEvents();
    check(inlineImageCanvas->findNext(QStringLiteral("BEFORE ")),
          "could not select text preceding an imported inline image");
    QKeyEvent collapseToEnd(QEvent::KeyPress, Qt::Key_Right,
                            Qt::NoModifier);
    QApplication::sendEvent(inlineImageCanvas, &collapseToEnd);
    QApplication::processEvents();
    const QRect beforeFirstImage = cursorRect(*inlineImageCanvas);
    QKeyEvent moveAcrossImage(QEvent::KeyPress, Qt::Key_Right,
                              Qt::NoModifier);
    QApplication::sendEvent(inlineImageCanvas, &moveAcrossImage);
    QApplication::processEvents();
    const QRect afterFirstImage = cursorRect(*inlineImageCanvas);
    const QImage mixedPaint =
        inlineImageCanvas->viewport()->grab().toImage();
    const QRect importedRed = saturatedColorBounds(mixedPaint, true);
    const QRect importedBlue = saturatedColorBounds(mixedPaint, false);
    check(importedRed.isValid() && importedBlue.isValid(),
          "imported inline pictures were not painted");
    check(importedRed.right() < importedBlue.left() &&
              importedRed.top() < importedBlue.bottom() &&
              importedBlue.top() < importedRed.bottom(),
          "imported inline pictures lost run order or were vertically stacked");
    const auto inlineSnapshot = inlineImageCanvas->snapshot();
    const auto& inlineParagraph =
        inlineSnapshot.document.paragraphs().front();
    check(inlineParagraph.images().size() == 2 &&
              inlineParagraph.images()[0].utf16_offset == 7 &&
              inlineParagraph.images()[1].utf16_offset == 16 &&
              beforeFirstImage.x() <= importedRed.left() + 2 &&
              afterFirstImage.x() >= importedRed.right() - 2 &&
              afterFirstImage.x() < importedBlue.left(),
          "imported image fragment offsets were not preserved among text runs");

    std::cout << "desktop DOCX import tests passed\n";
    return 0;
}
