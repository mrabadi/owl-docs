#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>

#include <zip.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
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

void createStyleProvenanceFixture(const QString& path) {
    docxstudio::ooxml::NewParagraph seed;
    seed.runs.push_back({"seed", {}});
    const auto initial = docxstudio::ooxml::DocxDocument::writeNew(
        std::filesystem::path(QFile::encodeName(path).constData()), {seed});
    check(static_cast<bool>(initial),
          "could not create style-provenance fixture package");

    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr>"
        "<w:r><w:t xml:space=\"preserve\">Inherited </w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"CC0000\"/></w:rPr>"
        "<w:t>Direct</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"FirmBlue\"/></w:pPr>"
        "<w:r><w:t xml:space=\"preserve\">Inherited </w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"CC0000\"/></w:rPr>"
        "<w:t>Direct</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    const std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Carlito\" w:hAnsi=\"Carlito\"/>"
        "<w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:rPr><w:b w:val=\"0\"/>"
        "<w:color w:val=\"000000\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
        "<w:name w:val=\"Foreign Heading 1\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:rPr><w:b/><w:color w:val=\"336699\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"FirmBlue\">"
        "<w:name w:val=\"Firm Blue\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:rPr><w:b/><w:color w:val=\"336699\"/></w:rPr></w:style>"
        "</w:styles>";
    replacePackageMembers(
        path,
        {{"word/document.xml", document}, {"word/styles.xml", styles}});
}

void createForeignBuiltInStyleFixture(const QString& path) {
    createStyleProvenanceFixture(path);
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr>"
        "<w:r><w:t xml:space=\"preserve\">Inherited </w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"CC0000\"/></w:rPr>"
        "<w:t>Direct</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    replacePackageMembers(path, {{"word/document.xml", document}});
}

void createEmptyForeignBuiltInStyleFixture(const QString& path,
                                           bool directMarkOverrides) {
    createStyleProvenanceFixture(path);
    const std::string directMark = directMarkOverrides
        ? "<w:rPr><w:b w:val=\"0\"/><w:i w:val=\"0\"/>"
          "<w:color w:val=\"CC0000\"/></w:rPr>"
        : "";
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body><w:p><w:pPr><w:pStyle w:val=\"Heading1\"/>" +
        directMark +
        "</w:pPr></w:p><w:sectPr/></w:body></w:document>";
    const std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Carlito\" w:hAnsi=\"Carlito\"/>"
        "<w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:rPr><w:b w:val=\"0\"/>"
        "<w:color w:val=\"000000\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
        "<w:name w:val=\"Foreign Heading 1\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:rPr><w:i/></w:rPr></w:pPr>"
        "<w:rPr><w:b/><w:color w:val=\"336699\"/></w:rPr></w:style>"
        "</w:styles>";
    replacePackageMembers(
        path,
        {{"word/document.xml", document}, {"word/styles.xml", styles}});
}

docxstudio::app::DocumentCanvas* canvasWithText(
    docxstudio::app::MainWindow& window, const QString& text);

void testImportedStyleProvenance(QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("style-provenance.docx"));
    createStyleProvenanceFixture(path);
    docxstudio::app::MainWindow window;
    check(window.openPath(path),
          "desktop shell could not open style-provenance fixture");
    docxstudio::app::DocumentCanvas* canvas = nullptr;
    for (auto* candidate :
         window.findChildren<docxstudio::app::DocumentCanvas*>()) {
        if (candidate->snapshot().document.paragraphs().size() == 2) {
            canvas = candidate;
            break;
        }
    }
    check(canvas != nullptr,
          "style-provenance fixture did not map to the editor");
    const auto imported = canvas->snapshot();
    for (const auto& paragraph : imported.document.paragraphs()) {
        check(paragraph.styleProvenance().has_value() &&
                  paragraph.characterFormatAt(1).foreground_argb ==
                      0xff336699U &&
                  paragraph.characterFormatAt(11).foreground_argb ==
                      0xffcc0000U,
              "import lost a source style baseline or direct red override");
    }
    check(imported.document.paragraphs()[0].styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              imported.document.paragraphs()[1].styleId() ==
                  std::optional<std::string>{"FirmBlue"},
          "style-provenance fixture lost built-in/custom identities");

    canvas->selectAll();
    canvas->applyParagraphStyle(QStringLiteral("Normal"));
    const auto normalized = canvas->snapshot();
    for (const auto& paragraph : normalized.document.paragraphs()) {
        check(paragraph.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  paragraph.characterFormatAt(1).foreground_argb ==
                      0xff000000U &&
                  paragraph.characterFormatAt(1).bold == false &&
                  paragraph.characterFormatAt(11).foreground_argb ==
                      0xffcc0000U &&
                  paragraph.characterFormatAt(11).bold == false &&
                  paragraph.styleProvenance() &&
                  paragraph.styleProvenance()
                          ->inherited_character_format.foreground_argb ==
                      0xff000000U &&
                  paragraph.styleProvenance()
                          ->character_overrides.size() == 1 &&
                  paragraph.styleProvenance()
                          ->character_overrides.front().start == 10 &&
                  paragraph.styleProvenance()
                          ->character_overrides.front().end == 16 &&
                  paragraph.styleProvenance()
                          ->character_overrides.front()
                          .properties.foreground_argb,
              "Normal failed to replace inherited source formatting while preserving a direct override");
    }
}

void testForeignBuiltInSimplifiedSavePreservesAppearance(
    QTemporaryDir& temporary) {
    const QString sourcePath = temporary.filePath(
        QStringLiteral("foreign-heading-source.docx"));
    const QString simplifiedPath = temporary.filePath(
        QStringLiteral("foreign-heading-simplified.docx"));
    createForeignBuiltInStyleFixture(sourcePath);

    docxstudio::app::MainWindow imported;
    check(imported.openPath(sourcePath),
          "could not open foreign built-in style fixture");
    auto* canvas = canvasWithText(imported, QStringLiteral("Inherited Direct"));
    auto* saveAs = imported.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(canvas && saveAs,
          "could not reach foreign built-in style Save As controls");
    const auto before = canvas->snapshot();
    const auto& sourceParagraph = before.document.paragraphs().front();
    check(sourceParagraph.styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              sourceParagraph.characterFormatAt(1).font_size_half_points ==
                  22 &&
              sourceParagraph.characterFormatAt(1).foreground_argb ==
                  0xff336699U &&
              sourceParagraph.characterFormatAt(1).bold == true &&
              sourceParagraph.characterFormatAt(11).foreground_argb ==
                  0xffcc0000U &&
              sourceParagraph.format().space_before_emu == std::nullopt &&
              sourceParagraph.format().space_after_emu == std::nullopt,
          "foreign built-in style fixture did not expose its source appearance");

    // Imported packages only regenerate after a supported structural or
    // formatting edit.  A harmless page-layout change deliberately exercises
    // the explicit simplified-copy path rather than the byte-copy Save As.
    canvas->setMarginsPoints(
        canvas->marginTopPoints() + 1.0, canvas->marginRightPoints(),
        canvas->marginBottomPoints(), canvas->marginLeftPoints());
    check(canvas->hasNonTextChanges(),
          "foreign style fixture was not marked for structural regeneration");

    bool selectedDestination = false;
    bool confirmedSimplification = false;
    bool unexpectedDialog = false;
    QTimer monitor;
    monitor.setInterval(1);
    QObject::connect(&monitor, &QTimer::timeout, &imported, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget);
                dialog && dialog->isVisible() && !selectedDestination) {
                dialog->selectFile(simplifiedPath);
                selectedDestination = true;
                check(QMetaObject::invokeMethod(
                          dialog, "accept", Qt::DirectConnection),
                      "could not accept foreign style Save As destination");
                continue;
            }
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (!box || !box->isVisible()) continue;
            if (box->windowTitle() ==
                QStringLiteral("Compatibility warning")) {
                confirmedSimplification = true;
                box->done(QMessageBox::Yes);
            } else {
                unexpectedDialog = true;
                box->accept();
            }
        }
    });
    monitor.start();
    saveAs->trigger();
    monitor.stop();
    check(selectedDestination && confirmedSimplification &&
              !unexpectedDialog && QFileInfo::exists(simplifiedPath) &&
              !canvas->isModified(),
          "foreign style fixture did not save as an explicit simplified copy");

    docxstudio::ooxml::Error parsedError;
    auto parsed = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(
            QFile::encodeName(simplifiedPath).constData()),
        &parsedError);
    check(parsed && parsed->paragraphs().size() == 1 &&
              parsed->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading1"} &&
              parsed->paragraphs()[0].style_provenance &&
              parsed->paragraphs()[0]
                      .style_provenance->direct_space_before_twips == 0 &&
              parsed->paragraphs()[0]
                      .style_provenance->direct_space_after_twips == 0 &&
              parsed->paragraphs()[0].runs.size() == 2 &&
              parsed->paragraphs()[0].runs[0]
                      .paragraph_style_overrides.font_size_half_points == 22 &&
              parsed->paragraphs()[0].runs[0]
                      .paragraph_style_overrides.foreground_rgb == 0x00336699U &&
              parsed->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.font_size_half_points == 22 &&
              parsed->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.foreground_rgb == 0x00cc0000U,
          "simplified foreign Heading 1 did not serialize differences from Owl's generated baseline");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(simplifiedPath),
          "could not reopen simplified foreign built-in style fixture");
    auto* reopenedCanvas = canvasWithText(
        reopened, QStringLiteral("Inherited Direct"));
    check(reopenedCanvas != nullptr,
          "could not reach reopened simplified foreign style fixture");
    const auto after = reopenedCanvas->snapshot();
    const auto& reopenedParagraph = after.document.paragraphs().front();
    check(reopenedParagraph.styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              reopenedParagraph.characterFormatAt(1).font_size_half_points ==
                  22 &&
              reopenedParagraph.characterFormatAt(1).foreground_argb ==
                  0xff336699U &&
              reopenedParagraph.characterFormatAt(1).bold == true &&
              reopenedParagraph.characterFormatAt(11).foreground_argb ==
                  0xffcc0000U &&
              reopenedParagraph.format().space_before_emu == 0 &&
              reopenedParagraph.format().space_after_emu == 0,
          "simplified foreign built-in style changed appearance after reopen");
}

docxstudio::app::DocumentCanvas* canvasWithText(
    docxstudio::app::MainWindow& window, const QString& text) {
    for (auto* canvas :
         window.findChildren<docxstudio::app::DocumentCanvas*>()) {
        if (textOf(*canvas) == text) return canvas;
    }
    return nullptr;
}

docxstudio::app::DocumentCanvas* canvasWithStyle(
    docxstudio::app::MainWindow& window, const std::string& styleId) {
    for (auto* canvas :
         window.findChildren<docxstudio::app::DocumentCanvas*>()) {
        const auto snapshot = canvas->snapshot();
        if (snapshot.document.paragraphs().size() == 1 &&
            snapshot.document.paragraphs().front().styleId() == styleId) {
            return canvas;
        }
    }
    return nullptr;
}

void saveAsForTest(docxstudio::app::MainWindow& window, QAction& saveAs,
                   const QString& path, bool expectSimplification) {
    bool selectedDestination = false;
    bool confirmedSimplification = false;
    bool unexpectedDialog = false;
    QTimer monitor;
    monitor.setInterval(1);
    QObject::connect(&monitor, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget);
                dialog && dialog->isVisible() && !selectedDestination) {
                dialog->selectFile(path);
                selectedDestination = true;
                check(QMetaObject::invokeMethod(
                          dialog, "accept", Qt::DirectConnection),
                      "could not accept styled-empty Save As destination");
                continue;
            }
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (!box || !box->isVisible()) continue;
            if (box->windowTitle() ==
                QStringLiteral("Compatibility warning")) {
                confirmedSimplification = true;
                box->done(QMessageBox::Yes);
            } else {
                unexpectedDialog = true;
                box->accept();
            }
        }
    });
    monitor.start();
    saveAs.trigger();
    monitor.stop();
    check(selectedDestination &&
              confirmedSimplification == expectSimplification &&
              !unexpectedDialog && QFileInfo::exists(path),
          "styled-empty document did not complete the expected Save As path");
}

void testImportedEmptyStyledParagraphMarkLifecycle(
    QTemporaryDir& temporary) {
    const QString inheritedPath = temporary.filePath(
        QStringLiteral("foreign-empty-heading-inherited.docx"));
    createEmptyForeignBuiltInStyleFixture(inheritedPath, false);

    // Opening and typing directly into an empty styled paragraph must use the
    // resolved source style baseline, including style-chain pPr/rPr.
    docxstudio::app::MainWindow typedSource;
    check(typedSource.openPath(inheritedPath),
          "could not open inherited empty Heading 1 fixture");
    auto* typedCanvas = canvasWithStyle(typedSource, "Heading1");
    check(typedCanvas != nullptr,
          "could not reach inherited empty Heading 1 canvas");
    const auto inheritedEmpty = typedCanvas->snapshot();
    const auto& inheritedParagraph =
        inheritedEmpty.document.paragraphs().front();
    const auto& inheritedMark =
        inheritedParagraph.paragraphMarkCharacterFormat();
    check(inheritedParagraph.text().empty() &&
              inheritedMark.font_size_half_points == 22 &&
              inheritedMark.bold == true && inheritedMark.italic == true &&
              inheritedMark.foreground_argb == 0xff336699U &&
              inheritedParagraph.styleProvenance() &&
              inheritedParagraph.styleProvenance()
                      ->inherited_character_format.italic == std::nullopt &&
              inheritedParagraph.styleProvenance()
                      ->inherited_paragraph_mark_character_format.italic ==
                  true,
          "empty Heading 1 did not resolve a distinct inherited mark baseline");
    typedCanvas->insertText(QStringLiteral("Inherited typing"));
    const auto typed = typedCanvas->snapshot();
    check(typed.document.paragraphs().front()
                  .characterFormatAt(1) == inheritedMark,
          "typing into an imported empty Heading 1 used editor defaults");

    // A style transition while still empty replaces inherited mark values;
    // they must not be mistaken for direct formatting.
    docxstudio::app::MainWindow transitionedEmpty;
    check(transitionedEmpty.openPath(inheritedPath),
          "could not reopen inherited empty Heading 1 fixture");
    auto* transitionedCanvas = canvasWithStyle(
        transitionedEmpty, "Heading1");
    check(transitionedCanvas != nullptr,
          "could not reach empty Heading 1 for a style transition");
    transitionedCanvas->applyParagraphStyle(QStringLiteral("Heading2"));
    const auto transitioned = transitionedCanvas->snapshot();
    const auto& transitionedParagraph =
        transitioned.document.paragraphs().front();
    const auto& transitionedMark =
        transitionedParagraph.paragraphMarkCharacterFormat();
    check(transitionedParagraph.text().empty() &&
              transitionedParagraph.styleId() ==
                  std::optional<std::string>{"Heading2"} &&
              transitionedMark.font_size_half_points == 26 &&
              transitionedMark.bold == true &&
              !transitionedMark.italic.value_or(false) &&
              transitionedMark.foreground_argb == 0xff77216fU &&
              transitionedParagraph.styleProvenance() &&
              transitionedParagraph.styleProvenance()
                      ->paragraph_mark_overrides.empty(),
          "empty style transition retained inherited source mark formatting");
    transitionedCanvas->insertText(QStringLiteral("Transitioned typing"));
    check(transitionedCanvas->snapshot().document.paragraphs().front()
                  .characterFormatAt(1) == transitionedMark,
          "typing after an empty style transition lost the target baseline");

    // Direct pPr/rPr values, including explicit false values, survive a style
    // transition and a simplified DOCX round trip.
    const QString directPath = temporary.filePath(
        QStringLiteral("foreign-empty-heading-direct.docx"));
    const QString directCopy = temporary.filePath(
        QStringLiteral("foreign-empty-heading-direct-copy.docx"));
    createEmptyForeignBuiltInStyleFixture(directPath, true);
    docxstudio::app::MainWindow direct;
    check(direct.openPath(directPath),
          "could not open direct empty Heading 1 fixture");
    auto* directCanvas = canvasWithStyle(direct, "Heading1");
    auto* directSaveAs = direct.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(directCanvas && directSaveAs,
          "could not reach direct empty Heading 1 controls");
    const auto directEmpty = directCanvas->snapshot();
    const auto& directParagraph = directEmpty.document.paragraphs().front();
    const auto& directMark = directParagraph.paragraphMarkCharacterFormat();
    check(directMark.font_size_half_points == 22 &&
              directMark.bold == false && directMark.italic == false &&
              directMark.foreground_argb == 0xffcc0000U &&
              directParagraph.styleProvenance() &&
              directParagraph.styleProvenance()
                      ->paragraph_mark_overrides.bold &&
              directParagraph.styleProvenance()
                      ->paragraph_mark_overrides.italic &&
              directParagraph.styleProvenance()
                      ->paragraph_mark_overrides.foreground_argb,
          "direct empty paragraph-mark clears/formatting were not imported");
    directCanvas->insertText(QStringLiteral("Direct typing"));
    directCanvas->applyParagraphStyle(QStringLiteral("Heading2"));
    const auto directTransition = directCanvas->snapshot();
    const auto& directTransitionParagraph =
        directTransition.document.paragraphs().front();
    check(directTransitionParagraph.characterFormatAt(1)
                  .font_size_half_points == 26 &&
              directTransitionParagraph.characterFormatAt(1).bold == false &&
              directTransitionParagraph.characterFormatAt(1).italic == false &&
              directTransitionParagraph.characterFormatAt(1)
                      .foreground_argb == 0xffcc0000U &&
              directTransitionParagraph.paragraphMarkCharacterFormat()
                      .bold == false &&
              directTransitionParagraph.paragraphMarkCharacterFormat()
                      .italic == false &&
              directTransitionParagraph.paragraphMarkCharacterFormat()
                      .foreground_argb == 0xffcc0000U,
          "style transition discarded direct paragraph-mark clears/formatting");
    saveAsForTest(direct, *directSaveAs, directCopy, true);
    check(!directCanvas->isModified(),
          "direct empty-style simplified copy remained modified");

    docxstudio::ooxml::Error directError;
    auto directPackage = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(directCopy).constData()),
        &directError);
    check(directPackage && directPackage->paragraphs().size() == 1 &&
              directPackage->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading2"} &&
              directPackage->paragraphs()[0].runs.size() == 1 &&
              directPackage->paragraphs()[0].runs[0]
                      .paragraph_style_overrides.bold == false &&
              directPackage->paragraphs()[0].runs[0]
                      .paragraph_style_overrides.italic == false &&
              directPackage->paragraphs()[0].runs[0]
                      .paragraph_style_overrides.foreground_rgb == 0x00cc0000U &&
              directPackage->paragraphs()[0].style_provenance &&
              directPackage->paragraphs()[0]
                      .style_provenance->direct_paragraph_mark_format.bold ==
                  false &&
              directPackage->paragraphs()[0]
                      .style_provenance->direct_paragraph_mark_format.italic ==
                  false &&
              directPackage->paragraphs()[0]
                      .style_provenance->direct_paragraph_mark_format
                      .foreground_rgb == 0x00cc0000U,
          "direct paragraph-mark formatting was not native after simplified save");

    docxstudio::app::MainWindow directReopened;
    check(directReopened.openPath(directCopy),
          "could not reopen direct paragraph-mark simplified copy");
    auto* directReopenedCanvas = canvasWithText(
        directReopened, QStringLiteral("Direct typing"));
    check(directReopenedCanvas != nullptr,
          "could not reach reopened direct paragraph-mark copy");
    const auto directAfter = directReopenedCanvas->snapshot();
    const auto& directAfterParagraph =
        directAfter.document.paragraphs().front();
    check(directAfterParagraph.characterFormatAt(1)
                  .font_size_half_points == 26 &&
              directAfterParagraph.characterFormatAt(1).bold == false &&
              directAfterParagraph.characterFormatAt(1).italic == false &&
              directAfterParagraph.characterFormatAt(1).foreground_argb ==
                  0xffcc0000U &&
              directAfterParagraph.paragraphMarkCharacterFormat().bold ==
                  false &&
              directAfterParagraph.paragraphMarkCharacterFormat().italic ==
                  false &&
              directAfterParagraph.paragraphMarkCharacterFormat()
                      .foreground_argb == 0xffcc0000U,
          "direct paragraph-mark formatting changed after simplified reopen");
}

void testAuthoredEmptyStyledParagraphRoundTrip(QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("authored-empty-heading.docx"));
    docxstudio::app::MainWindow authored;
    auto* canvas = authored.findChild<docxstudio::app::DocumentCanvas*>();
    auto* saveAs = authored.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(canvas && saveAs,
          "could not reach authored empty Heading 1 controls");
    canvas->applyParagraphStyle(QStringLiteral("Heading1"));
    const auto authoredEmpty = canvas->snapshot();
    const auto& authoredParagraph =
        authoredEmpty.document.paragraphs().front();
    check(authoredParagraph.text().empty() &&
              authoredParagraph.styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              authoredParagraph.paragraphMarkCharacterFormat()
                      .font_size_half_points == 32 &&
              authoredParagraph.paragraphMarkCharacterFormat().bold == true &&
              authoredParagraph.paragraphMarkCharacterFormat()
                      .foreground_argb == 0xffe95420U,
          "authored empty Heading 1 did not establish its insertion baseline");
    saveAsForTest(authored, *saveAs, path, false);
    check(!canvas->isModified(),
          "authored empty Heading 1 remained modified after Save As");

    docxstudio::ooxml::Error error;
    auto package = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(package && package->paragraphs().size() == 1 &&
              package->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading1"} &&
              package->paragraphs()[0].style_provenance &&
              package->paragraphs()[0]
                      .style_provenance->direct_paragraph_mark_format ==
                  docxstudio::ooxml::BasicRunFormat{} &&
              package->paragraphs()[0]
                      .style_provenance->inherited_paragraph_mark_format ==
                  package->paragraphs()[0]
                      .style_provenance->inherited_character_format,
          "authored empty Heading 1 wrote its inherited mark as direct rPr");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "could not reopen authored empty Heading 1");
    auto* reopenedCanvas = canvasWithStyle(reopened, "Heading1");
    check(reopenedCanvas != nullptr,
          "could not reach reopened authored empty Heading 1");
    reopenedCanvas->insertText(QStringLiteral("Authored typing"));
    const auto after = reopenedCanvas->snapshot();
    const auto& format =
        after.document.paragraphs().front().characterFormatAt(1);
    check(format.font_size_half_points == 32 && format.bold == true &&
              format.foreground_argb == 0xffe95420U,
          "typing into reopened authored Heading 1 used editor defaults");
}

void testCanonicalSimpleSaveGuard(QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("owl-canonical-simple.docx"));
    {
        docxstudio::app::MainWindow authored;
        auto* canvas = authored.findChild<docxstudio::app::DocumentCanvas*>();
        auto* saveAs = authored.findChild<QAction*>(
            QStringLiteral("file.saveAs"));
        check(canvas && saveAs,
              "could not reach canonical simple-document Save As");
        canvas->insertText(QStringLiteral("Canonical simple document"));
        bool selectedDestination = false;
        QTimer::singleShot(0, &authored, [&] {
            auto* dialog = authored.findChild<QFileDialog*>();
            check(dialog != nullptr,
                  "canonical simple Save As did not open a file dialog");
            dialog->selectFile(path);
            selectedDestination = true;
            check(QMetaObject::invokeMethod(
                      dialog, "accept", Qt::DirectConnection),
                  "could not accept canonical simple Save As destination");
        });
        saveAs->trigger();
        check(selectedDestination && QFileInfo::exists(path) &&
                  !canvas->isModified(),
              "could not establish a canonical simple DOCX baseline");
    }

    {
        docxstudio::app::MainWindow reopened;
        check(reopened.openPath(path),
              "could not reopen canonical simple DOCX");
        auto* canvas = canvasWithText(
            reopened, QStringLiteral("Canonical simple document"));
        auto* save = reopened.findChild<QAction*>(
            QStringLiteral("file.save"));
        check(canvas && save,
              "could not reach reopened canonical simple document");
        canvas->selectAll();
        canvas->toggleBold();
        check(canvas->isModified() && canvas->hasNonTextChanges(),
              "canonical simple formatting edit was not classified correctly");

        bool unexpectedWarning = false;
        QTimer monitor;
        monitor.setInterval(1);
        QObject::connect(&monitor, &QTimer::timeout, &reopened, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                unexpectedWarning = true;
                box->accept();
            }
        });
        monitor.start();
        save->trigger();
        monitor.stop();
        check(!unexpectedWarning && !canvas->isModified(),
              "reopened canonical simple DOCX incorrectly required a simplified Save As");

        docxstudio::ooxml::Error error;
        auto saved = docxstudio::ooxml::DocxDocument::open(
            std::filesystem::path(QFile::encodeName(path).constData()),
            &error);
        check(saved && !saved->paragraphs().empty() &&
                  !saved->paragraphs().front().runs.empty() &&
                  saved->paragraphs().front().runs.front().format.bold == true,
              "normal Save did not persist canonical simple formatting");

        const QByteArray beforeDefaultsChange = readFile(path);
        canvas->setEditorDefaults(
            canvas->defaultFontFamily(),
            canvas->defaultFontPointSize() + 1.0,
            canvas->tabWidthSpaces());
        canvas->selectAll();
        canvas->toggleItalic();
        check(canvas->isModified() && canvas->hasNonTextChanges(),
              "defaults-mismatch formatting edit was not classified correctly");

        int defaultsMismatchWarnings = 0;
        QTimer defaultsMonitor;
        defaultsMonitor.setInterval(1);
        QObject::connect(
            &defaultsMonitor, &QTimer::timeout, &reopened, [&] {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    auto* box = qobject_cast<QMessageBox*>(widget);
                    if (!box || !box->isVisible()) continue;
                    if (box->windowTitle() ==
                            QStringLiteral("Compatibility warning") &&
                        box->text().contains(QStringLiteral(
                            "preserves but cannot safely rewrite"))) {
                        ++defaultsMismatchWarnings;
                        box->done(QMessageBox::Yes);
                    } else {
                        box->reject();
                    }
                }
            });
        defaultsMonitor.start();
        save->trigger();
        defaultsMonitor.stop();
        check(defaultsMismatchWarnings == 1 && !canvas->isModified() &&
                  readFile(path) != beforeDefaultsChange,
              "confirmed compatibility save did not overwrite the reopened document");

        canvas->selectAll();
        canvas->toggleUnderline();
        bool repeatedWarning = false;
        QTimer repeatMonitor;
        repeatMonitor.setInterval(1);
        QObject::connect(
            &repeatMonitor, &QTimer::timeout, &reopened, [&] {
                for (auto* widget : QApplication::topLevelWidgets()) {
                    auto* box = qobject_cast<QMessageBox*>(widget);
                    if (!box || !box->isVisible()) continue;
                    repeatedWarning = true;
                    box->reject();
                }
            });
        repeatMonitor.start();
        save->trigger();
        repeatMonitor.stop();
        check(!repeatedWarning && !canvas->isModified(),
              "compatibility warning repeated after the confirmed rebuild");
    }

    const QString opaquePath = temporary.filePath(
        QStringLiteral("owl-canonical-simple-opaque.docx"));
    check(QFile::copy(path, opaquePath),
          "could not copy opaque save-guard fixture");
    const std::string opaquePayload("opaque\0must-remain", 18);
    replacePackageMembers(
        opaquePath, {{"customXml/item1.bin", opaquePayload}});
    const QByteArray opaqueBefore = readFile(opaquePath);
    {
        docxstudio::app::MainWindow imported;
        check(imported.openPath(opaquePath),
              "could not open opaque simple-body DOCX");
        auto* canvas = canvasWithText(
            imported, QStringLiteral("Canonical simple document"));
        auto* save = imported.findChild<QAction*>(QStringLiteral("file.save"));
        check(canvas && save,
              "could not reach opaque simple-body save command");
        canvas->selectAll();
        canvas->toggleItalic();

        int preservationWarnings = 0;
        QString warningText;
        QTimer monitor;
        monitor.setInterval(1);
        QObject::connect(&monitor, &QTimer::timeout, &imported, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                if (box->windowTitle() ==
                    QStringLiteral("Compatibility warning")) {
                    ++preservationWarnings;
                    warningText = box->text();
                    box->done(QMessageBox::Yes);
                } else {
                    box->reject();
                }
            }
        });
        monitor.start();
        save->trigger();
        monitor.stop();
        check(preservationWarnings == 1 &&
                  warningText.contains(
                      QStringLiteral("preserves but cannot safely rewrite")) &&
                  !canvas->isModified() &&
                  readFile(opaquePath) != opaqueBefore,
              "confirmed opaque-content warning did not permit atomic overwrite");
    }
}

void testNativeParagraphStyleRoundTrip(QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("owl-native-paragraph-styles.docx"));
    {
        docxstudio::app::MainWindow authored;
        auto* canvas = authored.findChild<docxstudio::app::DocumentCanvas*>();
        auto* saveAs = authored.findChild<QAction*>(
            QStringLiteral("file.saveAs"));
        check(canvas && saveAs,
              "could not reach native-style authoring controls");
        canvas->insertText(QStringLiteral("Native heading"));
        canvas->applyParagraphStyle(QStringLiteral("Heading1"));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &enter);
        canvas->insertText(QStringLiteral("Normal body"));
        check(canvas->findNext(QStringLiteral("heading")),
              "could not select the native heading override span");
        canvas->setForeground(QColor(QStringLiteral("#cc0000")));
        docxstudio::core::CharacterFormatDelta explicitEqualBold;
        explicitEqualBold.bold =
            docxstudio::core::PropertyDelta<bool>::set(true);
        canvas->applyCharacterFormat(explicitEqualBold);
        canvas->setAlignment(docxstudio::core::ParagraphAlignment::left);

        bool selectedDestination = false;
        QTimer::singleShot(0, &authored, [&] {
            auto* dialog = authored.findChild<QFileDialog*>();
            check(dialog != nullptr,
                  "native-style Save As did not open a file dialog");
            dialog->selectFile(path);
            selectedDestination = true;
            check(QMetaObject::invokeMethod(
                      dialog, "accept", Qt::DirectConnection),
                  "could not accept native-style Save As destination");
        });
        saveAs->trigger();
        check(selectedDestination && QFileInfo::exists(path) &&
                  !canvas->isModified(),
              "native-style document was not saved");
    }

    docxstudio::ooxml::Error writtenError;
    auto written = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()),
        &writtenError);
    check(written && written->paragraphs().size() == 2 &&
              written->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading1"} &&
              written->paragraphs()[1].style_id ==
                  std::optional<std::string>{"Normal"},
          "authored style identity or next-style identity was not native DOCX");
    check(written->paragraphs()[0].runs.size() == 2 &&
              written->paragraphs()[0].runs[0]
                  .paragraph_style_overrides ==
                  docxstudio::ooxml::BasicRunFormat{} &&
              written->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.foreground_rgb ==
                  0x00cc0000U &&
              written->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.bold == true,
          "native style save flattened inheritance or lost a direct run override");
    check(written->paragraphs()[0].style_provenance &&
              written->paragraphs()[0]
                      .style_provenance->direct_alignment ==
                  docxstudio::ooxml::BasicParagraphAlignment::left,
          "native style save lost an explicit equal-valued paragraph override");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "could not reopen native-style Owl Docs package");
    auto* canvas = canvasWithText(reopened, QStringLiteral("Native heading"));
    auto* save = reopened.findChild<QAction*>(QStringLiteral("file.save"));
    check(canvas && save &&
              canvas->snapshot().document.paragraphs()[0].styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              canvas->snapshot().document.paragraphs()[1].styleId() ==
                  std::optional<std::string>{"Normal"},
          "desktop import dropped native paragraph-style identity");

    // The initial caret is in the first paragraph. Changing its named style
    // is a supported canonical rewrite and must not force a simplified copy.
    canvas->applyParagraphStyle(QStringLiteral("Heading2"));
    const auto headingTwoSnapshot = canvas->snapshot();
    const auto& headingTwo = headingTwoSnapshot.document.paragraphs()[0];
    check(headingTwo.characterFormatAt(1).font_size_half_points == 26 &&
              headingTwo.characterFormatAt(1).foreground_argb ==
                  0xff77216fU &&
              headingTwo.characterFormatAt(8).foreground_argb ==
                  0xffcc0000U &&
              headingTwo.characterFormatAt(8).bold == true &&
              headingTwo.format().space_before_emu == 10 * 12'700 &&
              headingTwo.format().space_after_emu == 4 * 12'700,
          "save/reopen turned inherited Heading 1 properties into direct overrides");
    bool unexpectedWarning = false;
    QTimer monitor;
    monitor.setInterval(1);
    QObject::connect(&monitor, &QTimer::timeout, &reopened, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (!box || !box->isVisible()) continue;
            unexpectedWarning = true;
            box->accept();
        }
    });
    monitor.start();
    save->trigger();
    monitor.stop();
    check(!unexpectedWarning && !canvas->isModified(),
          "canonical named-style edit incorrectly required simplified Save As");

    docxstudio::ooxml::Error reopenedError;
    auto resaved = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()),
        &reopenedError);
    check(resaved && resaved->paragraphs().size() == 2 &&
              resaved->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading2"} &&
              resaved->paragraphs()[1].style_id ==
                  std::optional<std::string>{"Normal"},
          "normal Save did not persist the changed native style");
    check(resaved->paragraphs()[0].runs.size() == 2 &&
              resaved->paragraphs()[0].runs[0]
                  .paragraph_style_overrides ==
                  docxstudio::ooxml::BasicRunFormat{} &&
              resaved->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.foreground_rgb ==
                  0x00cc0000U &&
              resaved->paragraphs()[0].runs[1]
                      .paragraph_style_overrides.bold == true &&
              resaved->paragraphs()[0].style_provenance &&
              resaved->paragraphs()[0]
                      .style_provenance->direct_alignment ==
                  docxstudio::ooxml::BasicParagraphAlignment::left &&
              !resaved->paragraphs()[0].style_provenance->direct_space_before_twips &&
              !resaved->paragraphs()[0].style_provenance->direct_space_after_twips,
          "resaved Heading 2 flattened its style baseline into direct formatting");

    docxstudio::app::MainWindow reopenedAgain;
    check(reopenedAgain.openPath(path),
          "could not reopen the resaved Heading 2 package");
    auto* normalCanvas = canvasWithText(
        reopenedAgain, QStringLiteral("Native heading"));
    check(normalCanvas != nullptr,
          "could not reach the twice-reopened native-style document");
    normalCanvas->applyParagraphStyle(QStringLiteral("Normal"));
    const auto normalSnapshot = normalCanvas->snapshot();
    const auto& normal = normalSnapshot.document.paragraphs()[0];
    check(normal.characterFormatAt(1).font_size_half_points == 22 &&
              normal.characterFormatAt(1).foreground_argb == 0xff000000U &&
              normal.characterFormatAt(1).bold == false &&
              normal.characterFormatAt(8).foreground_argb == 0xffcc0000U &&
              normal.characterFormatAt(8).bold == true &&
              normal.format().space_before_emu == 0 &&
              normal.format().space_after_emu == 0,
          "second style transition did not update inheritance while preserving direct red");
}

void testNativeStyledObjectFormatsAreSparse(QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("owl-native-styled-objects.docx"));
    docxstudio::app::MainWindow authored;
    auto* canvas = authored.findChild<docxstudio::app::DocumentCanvas*>();
    auto* saveAs = authored.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(canvas && saveAs,
          "could not reach styled-object authoring controls");
    canvas->insertText(QStringLiteral("Styled objects "));
    canvas->applyParagraphStyle(QStringLiteral("Heading1"));
    check(canvas->insertEquation(QStringLiteral("x^2")),
          "could not insert a styled equation");
    check(canvas->insertInlineImage(
              solidPng(QColor(0, 96, 192)),
              QStringLiteral("Styled picture")),
          "could not insert a styled picture");

    bool selectedDestination = false;
    QTimer::singleShot(0, &authored, [&] {
        auto* dialog = authored.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "styled-object Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(
                  dialog, "accept", Qt::DirectConnection),
              "could not accept styled-object Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path),
          "styled-object document was not saved");

    docxstudio::ooxml::Error error;
    auto reopened = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(reopened && reopened->paragraphs().size() == 1 &&
              reopened->paragraphs()[0].style_id ==
                  std::optional<std::string>{"Heading1"},
          "styled-object document did not reopen with its native style");
    bool sawEquation = false;
    bool sawImage = false;
    for (const auto& run : reopened->paragraphs()[0].runs) {
        const bool hasTypedObject = std::any_of(
            run.fragments.begin(), run.fragments.end(),
            [&](const auto& fragment) {
                sawEquation = sawEquation ||
                    fragment.kind ==
                        docxstudio::ooxml::FragmentKind::equation;
                sawImage = sawImage ||
                    fragment.kind ==
                        docxstudio::ooxml::FragmentKind::inline_image;
                return fragment.kind ==
                           docxstudio::ooxml::FragmentKind::equation ||
                       fragment.kind ==
                           docxstudio::ooxml::FragmentKind::inline_image;
            });
        if (hasTypedObject) {
            check(run.paragraph_style_overrides ==
                      docxstudio::ooxml::BasicRunFormat{},
                  "equation/image run serialized inherited style properties directly");
        }
    }
    check(sawEquation && sawImage,
          "styled equation or picture was lost during save/reopen");
    check(reopened->paragraphs()[0].style_provenance &&
              !reopened->paragraphs()[0]
                   .style_provenance->direct_space_before_twips &&
              !reopened->paragraphs()[0]
                   .style_provenance->direct_space_after_twips,
          "styled object paragraph serialized inherited spacing directly");
}

void testNativeEquationDirectFormatSurvivesStyleTransitions(
    QTemporaryDir& temporary) {
    const QString path = temporary.filePath(
        QStringLiteral("owl-native-direct-equation.docx"));
    const QString transitionedPath = temporary.filePath(
        QStringLiteral("owl-native-direct-equation-heading2.docx"));
    docxstudio::app::MainWindow authored;
    auto* canvas = authored.findChild<docxstudio::app::DocumentCanvas*>();
    auto* saveAs = authored.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(canvas && saveAs,
          "could not reach direct-equation authoring controls");
    canvas->insertText(QStringLiteral("Equation "));
    canvas->applyParagraphStyle(QStringLiteral("Heading1"));
    canvas->toggleBold();
    canvas->setForeground(QColor(QStringLiteral("#cc0000")));
    check(canvas->insertEquation(QStringLiteral("x^2")),
          "could not insert a directly formatted equation");
    const auto authoredSnapshot = canvas->snapshot();
    const auto& authoredParagraph =
        authoredSnapshot.document.paragraphs().front();
    check(authoredParagraph.equations().size() == 1,
          "authored equation atom is absent");
    const auto equationOffset =
        authoredParagraph.equations().front().utf16_offset;
    const auto authoredEquationFormat =
        authoredParagraph.characterFormatAt(equationOffset + 1);
    const auto authoredEquationMask =
        authoredParagraph.styleOverrideMaskAt(equationOffset + 1);
    check(authoredEquationFormat.font_size_half_points == 32 &&
              authoredEquationFormat.bold == false &&
              authoredEquationFormat.foreground_argb == 0xffcc0000U &&
              authoredEquationMask.foreground_argb &&
              !authoredEquationMask.font_size_half_points &&
              authoredEquationMask.bold,
          "authored equation did not separate direct red/explicit bold clear from Heading 1 inheritance");
    saveAsForTest(authored, *saveAs, path, false);

    docxstudio::ooxml::Error packageError;
    auto package = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()),
        &packageError);
    check(package && package->paragraphs().size() == 1,
          packageError.message.empty()
              ? "direct-equation package did not reopen"
              : packageError.message.c_str());
    const auto equationRun = std::find_if(
        package->paragraphs()[0].runs.begin(),
        package->paragraphs()[0].runs.end(), [](const auto& run) {
            return std::any_of(
                run.fragments.begin(), run.fragments.end(),
                [](const auto& fragment) {
                    return fragment.kind ==
                        docxstudio::ooxml::FragmentKind::equation;
                });
        });
    check(equationRun != package->paragraphs()[0].runs.end() &&
              equationRun->format.font_size_half_points == 32 &&
              equationRun->format.bold == false &&
              equationRun->format.foreground_rgb == 0x00cc0000U &&
              equationRun->paragraph_style_overrides.foreground_rgb ==
                  0x00cc0000U &&
              !equationRun->paragraph_style_overrides
                   .font_size_half_points &&
              equationRun->paragraph_style_overrides.bold == false,
          "direct equation w:rPr did not reopen with sparse native formatting");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "could not reopen direct-equation document in the editor");
    auto* reopenedCanvas = canvasWithStyle(reopened, "Heading1");
    auto* transitionSaveAs = reopened.findChild<QAction*>(
        QStringLiteral("file.saveAs"));
    check(reopenedCanvas && transitionSaveAs,
          "could not reach reopened direct-equation controls");
    reopenedCanvas->applyParagraphStyle(QStringLiteral("Heading2"));
    const auto headingTwoSnapshot = reopenedCanvas->snapshot();
    const auto& headingTwo =
        headingTwoSnapshot.document.paragraphs().front();
    check(headingTwo.equations().size() == 1,
          "equation atom was lost during Heading 2 transition");
    const auto headingTwoEquationOffset =
        headingTwo.equations().front().utf16_offset;
    check(headingTwo.characterFormatAt(1).font_size_half_points == 26 &&
              headingTwo.characterFormatAt(1).foreground_argb ==
                  0xff77216fU &&
              headingTwo.characterFormatAt(headingTwoEquationOffset + 1)
                      .font_size_half_points == 26 &&
              headingTwo.characterFormatAt(headingTwoEquationOffset + 1)
                      .bold == false &&
              headingTwo.characterFormatAt(headingTwoEquationOffset + 1)
                      .foreground_argb == 0xffcc0000U,
          "equation style transition failed to update inheritance or retain direct red/bold clear");
    // Imported packages containing equations are deliberately outside the
    // narrow current-writer canonical-envelope proof. Exercise the explicit
    // simplified-copy path rather than weakening that safety gate.
    saveAsForTest(
        reopened, *transitionSaveAs, transitionedPath, true);
    check(!reopenedCanvas->isModified(),
          "direct-equation Heading 2 simplified Save As did not complete");

    docxstudio::app::MainWindow reopenedAgain;
    check(reopenedAgain.openPath(transitionedPath),
          "could not reopen direct equation after Heading 2 save");
    auto* normalCanvas = canvasWithStyle(reopenedAgain, "Heading2");
    check(normalCanvas != nullptr,
          "could not reach saved Heading 2 equation paragraph");
    normalCanvas->applyParagraphStyle(QStringLiteral("Normal"));
    const auto normalSnapshot = normalCanvas->snapshot();
    const auto& normal = normalSnapshot.document.paragraphs().front();
    check(normal.equations().size() == 1,
          "equation atom was lost after save/reopen");
    const auto normalEquationOffset = normal.equations().front().utf16_offset;
    check(normal.characterFormatAt(1).font_size_half_points == 22 &&
              normal.characterFormatAt(1).bold == false &&
              normal.characterFormatAt(1).foreground_argb == 0xff000000U &&
              normal.characterFormatAt(normalEquationOffset + 1)
                      .font_size_half_points == 22 &&
              normal.characterFormatAt(normalEquationOffset + 1).bold ==
                  false &&
              normal.characterFormatAt(normalEquationOffset + 1)
                      .foreground_argb == 0xffcc0000U,
          "equation direct formatting changed after save/reopen and Normal transition");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory failed");
    testCanonicalSimpleSaveGuard(temporary);
    testNativeParagraphStyleRoundTrip(temporary);
    testNativeStyledObjectFormatsAreSparse(temporary);
    testNativeEquationDirectFormatSurvivesStyleTransitions(temporary);
    testForeignBuiltInSimplifiedSavePreservesAppearance(temporary);
    testImportedEmptyStyledParagraphMarkLifecycle(temporary);
    testAuthoredEmptyStyledParagraphRoundTrip(temporary);
    testImportedStyleProvenance(temporary);
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
    check(heading.styleId() ==
              std::optional<std::string>{"SampleHeading"} &&
              heading.format().alignment ==
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
