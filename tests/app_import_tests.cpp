#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>

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

    std::cout << "desktop DOCX import tests passed\n";
    return 0;
}
