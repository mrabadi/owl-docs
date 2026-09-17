#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/math/latex_parser.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTimer>

#include <zip.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace {

using docxstudio::app::DocumentCanvas;
using docxstudio::core::BodyBlockKind;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString fromUtf16(const std::u16string& text) {
    return QString::fromUtf16(
        text.data(), static_cast<qsizetype>(text.size()));
}

QByteArray zipMember(const QString& path, const char* memberName) {
    int code = 0;
    const auto encodedPath = QFile::encodeName(path);
    zip_t* archive = zip_open(encodedPath.constData(), ZIP_RDONLY, &code);
    check(archive != nullptr, "could not open combined DOCX package");
    zip_stat_t stat{};
    zip_stat_init(&stat);
    check(zip_stat(archive, memberName, ZIP_FL_UNCHANGED, &stat) == 0,
          "combined DOCX is missing word/document.xml");
    check(stat.size <= static_cast<zip_uint64_t>(
              std::numeric_limits<qsizetype>::max()),
          "combined document.xml exceeds the test process limit");
    zip_file_t* member = zip_fopen(archive, memberName, ZIP_FL_UNCHANGED);
    check(member != nullptr, "could not open combined document.xml");
    QByteArray result(static_cast<qsizetype>(stat.size), Qt::Uninitialized);
    check(zip_fread(member, result.data(), stat.size) ==
              static_cast<zip_int64_t>(stat.size),
          "could not read combined document.xml");
    check(zip_fclose(member) == 0, "could not close combined document.xml");
    zip_discard(archive);
    return result;
}

DocumentCanvas* semanticCanvas(docxstudio::app::MainWindow& window) {
    for (auto* candidate : window.findChildren<DocumentCanvas*>()) {
        const auto snapshot = candidate->snapshot();
        if (std::any_of(
                snapshot.document.paragraphs().begin(),
                snapshot.document.paragraphs().end(),
                [](const auto& paragraph) {
                    return !paragraph.equations().empty();
                })) {
            return candidate;
        }
    }
    return nullptr;
}

DocumentCanvas* tableCanvas(docxstudio::app::MainWindow& window) {
    for (auto* candidate : window.findChildren<DocumentCanvas*>()) {
        if (!candidate->snapshot().document.tables().empty()) return candidate;
    }
    return nullptr;
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

DocumentCanvas* listCanvas(docxstudio::app::MainWindow& window,
                           const QString& content) {
    for (auto* candidate : window.findChildren<DocumentCanvas*>()) {
        const auto snapshot = candidate->snapshot();
        for (const auto& paragraph : snapshot.document.paragraphs()) {
            if (fromUtf16(paragraph.text()).contains(content)) return candidate;
        }
    }
    return nullptr;
}

void testEmptyBodyParagraphFormattingSurvivesSaveAndReopen() {
    namespace core = docxstudio::core;
    namespace ooxml = docxstudio::ooxml;

    docxstudio::app::MainWindow authored;
    authored.resize(1000, 760);
    authored.show();
    QApplication::processEvents();
    auto* canvas = authored.findChild<DocumentCanvas*>();
    check(canvas != nullptr,
          "could not reach the empty-paragraph save canvas");
    const QColor requestedColor(QStringLiteral("#284b63"));
    constexpr std::int32_t kRequestedHalfPoints = 31;
    canvas->setForeground(requestedColor);
    canvas->setFontPointSize(15.5);
    canvas->toggleItalic();

    auto snapshot = canvas->snapshot();
    check(snapshot.document.paragraphs().size() == 1 &&
              snapshot.document.paragraphs()[0].text().empty() &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .font_size_half_points == kRequestedHalfPoints &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .italic == true,
          "formatting an empty body paragraph did not update its paragraph mark");

    QTemporaryDir output;
    check(output.isValid(),
          "could not create empty-paragraph DOCX test directory");
    const QString path = output.filePath(
        QStringLiteral("empty-paragraph-format.docx"));
    auto* saveAs = authored.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr,
          "could not reach Save As for the empty paragraph");
    bool selectedDestination = false;
    QTimer::singleShot(0, &authored, [&] {
        auto* dialog = authored.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "empty-paragraph Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept the empty-paragraph Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path) &&
              !canvas->isModified(),
          "formatted empty paragraph was not saved as a clean DOCX baseline");

    const QByteArray xml = zipMember(path, "word/document.xml");
    check(xml.contains("<w:p><w:pPr>") &&
              xml.contains("<w:pStyle w:val=\"Normal\"/>") &&
              xml.contains("<w:rPr>") &&
              xml.contains("<w:color w:val=\"284B63\"/>") &&
              xml.contains("<w:sz w:val=\"31\"/>") &&
              xml.contains("<w:i/>"),
          "native Save As did not serialize the empty paragraph insertion format");

    ooxml::Error error;
    auto package = ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(package != nullptr && package->paragraphs().size() == 1 &&
              package->paragraphs()[0].paragraph_mark_format.has_value() &&
              package->paragraphs()[0].paragraph_mark_format
                      ->foreground_rgb == 0x284b63U &&
              package->paragraphs()[0].paragraph_mark_format
                      ->font_size_half_points == kRequestedHalfPoints &&
              package->paragraphs()[0].paragraph_mark_format->italic == true &&
              package->compatibility().classification ==
                  ooxml::CompatibilityClass::basic_body_text_patch,
          "saved paragraph-mark formatting did not reopen through OOXML");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its formatted empty paragraph");
    DocumentCanvas* reopenedCanvas = nullptr;
    for (auto* candidate : reopened.findChildren<DocumentCanvas*>()) {
        const auto candidateSnapshot = candidate->snapshot();
        if (candidateSnapshot.document.paragraphs().size() == 1 &&
            candidateSnapshot.document.paragraphs()[0]
                    .paragraphMarkCharacterFormat()
                    .foreground_argb ==
                static_cast<std::uint32_t>(requestedColor.rgba())) {
            reopenedCanvas = candidate;
            break;
        }
    }
    check(reopenedCanvas != nullptr,
          "semantic DOCX import lost the empty paragraph insertion format");
    snapshot = reopenedCanvas->snapshot();
    check(snapshot.document.paragraphs().size() == 1 &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .font_size_half_points == kRequestedHalfPoints &&
              snapshot.document.paragraphs()[0]
                      .paragraphMarkCharacterFormat()
                      .italic == true &&
              !reopenedCanvas->isModified(),
          "semantic DOCX import lost the empty paragraph insertion format");

    reopenedCanvas->insertText(QStringLiteral("Owl"));
    snapshot = reopenedCanvas->snapshot();
    const auto& typedParagraph = snapshot.document.paragraphs()[0];
    check(fromUtf16(typedParagraph.text()) == QStringLiteral("Owl") &&
              typedParagraph.characterFormatAt(1).foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              typedParagraph.characterFormatAt(1).font_size_half_points ==
                  kRequestedHalfPoints &&
              typedParagraph.characterFormatAt(1).italic == true,
          "typing after DOCX reopen did not consume the paragraph-mark format");
}

void testCombinedNativeReopenKeepsSemanticOrder() {
    namespace math = docxstudio::math;
    namespace ooxml = docxstudio::ooxml;

    const auto parsed = math::parseLatex(R"(\frac{x_1}{\sqrt{y}})");
    check(parsed.hasValue(), "combined DOCX equation fixture is invalid");
    const std::string latex = math::toCanonicalLatex(parsed.value());

    ooxml::NewParagraph before;
    before.runs.emplace_back("Before ", ooxml::BasicRunFormat{});
    before.runs.emplace_back(
        "", ooxml::BasicRunFormat{}, ooxml::EquationPayload{latex, false});
    before.runs.emplace_back(" after", ooxml::BasicRunFormat{});
    const ooxml::NewTable table{
        2, 2, true, {"Header A", "Header B", "one", "two"}};
    const ooxml::NewParagraph tail{{ooxml::NewRun{"Tail", {}}}};

    QTemporaryDir output;
    check(output.isValid(), "could not create combined DOCX test directory");
    const QString path = output.filePath(QStringLiteral("table-equation.docx"));
    const QByteArray nativePath = QFile::encodeName(path);
    const auto save = ooxml::DocxDocument::writeNew(
        std::filesystem::path(nativePath.constData()),
        ooxml::NewDocumentBody{{before, table, tail}});
    check(save.saved,
          save.error ? save.error->message.c_str()
                     : "combined table/equation DOCX was not saved");

    const QByteArray xml = zipMember(path, "word/document.xml");
    const qsizetype leadingText = xml.indexOf("Before ");
    const qsizetype equation = xml.indexOf("<m:oMath>");
    const qsizetype tableStart = xml.indexOf("<w:tbl>");
    const qsizetype trailingText = xml.indexOf(">Tail</w:t>");
    check(leadingText >= 0 && equation > leadingText &&
              tableStart > equation && trailingText > tableStart,
          "combined DOCX did not serialize paragraph/equation/table order");
    check(xml.count("<w:tr>") == 2 && xml.count("<w:tc>") == 4 &&
              xml.contains("<w:tblHeader/>") && xml.contains("<m:f>"),
          "combined DOCX did not contain a native table and native equation");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its combined native DOCX");
    DocumentCanvas* imported = semanticCanvas(reopened);
    check(imported != nullptr,
          "combined native DOCX did not reconstruct its equation atom");
    const auto snapshot = imported->snapshot();

    check(snapshot.document.paragraphs().size() == 2 &&
              snapshot.document.tables().size() == 1 &&
              snapshot.document.bodyBlocks().size() == 3,
          "reopened native table was flattened into ordinary paragraphs");
    check(snapshot.document.bodyBlocks()[0].kind == BodyBlockKind::paragraph &&
              snapshot.document.bodyBlocks()[1].kind == BodyBlockKind::table &&
              snapshot.document.bodyBlocks()[2].kind == BodyBlockKind::paragraph,
          "combined native DOCX body-block order changed on reopen");

    const auto& importedTable = snapshot.document.tables().front();
    check(importedTable.rowCount() == 2 &&
              importedTable.columnCount() == 2 &&
              importedTable.hasHeaderRow() &&
              fromUtf16(importedTable.cell(0, 0)->text) ==
                  QStringLiteral("Header A") &&
              fromUtf16(importedTable.cell(1, 1)->text) ==
                  QStringLiteral("two"),
          "reopened native table lost dimensions, header, or cell contents");

    const auto& importedParagraph = snapshot.document.paragraphs().front();
    check(importedParagraph.equations().size() == 1 &&
              importedParagraph.equations().front().utf16_offset == 7 &&
              importedParagraph.equations().front().canonical_latex == latex,
          "combined native equation changed around the table on reopen");
    check(!imported->isModified(),
          "reopening the combined native DOCX incorrectly marked it modified");

    check(imported->findNext(QStringLiteral("Tail")),
          "could not select the trailing body paragraph for an edit");
    imported->insertText(QStringLiteral("Tail edited"));
    check(imported->isModified() && !imported->hasNonTextChanges(),
          "ordinary trailing text was not classified as a patchable text edit");
    auto* saveAction = reopened.findChild<QAction*>(QStringLiteral("file.save"));
    check(saveAction != nullptr,
          "could not reach Save for the reopened combined DOCX");
    saveAction->trigger();
    check(!imported->isModified(),
          "patch-saving trailing text did not establish a clean baseline");

    const QByteArray editedXml = zipMember(path, "word/document.xml");
    const qsizetype editedEquation = editedXml.indexOf("<m:oMath>");
    const qsizetype editedTable = editedXml.indexOf("<w:tbl>");
    const qsizetype editedTail = editedXml.indexOf(">Tail edited</w:t>");
    check(editedEquation >= 0 && editedTable > editedEquation &&
              editedTail > editedTable && editedXml.count("<w:tc>") == 4,
          "patch-saving trailing text flattened or reordered native content");

    docxstudio::app::MainWindow reopenedAgain;
    check(reopenedAgain.openPath(path),
          "Owl Docs could not reopen the text-patched combined DOCX");
    DocumentCanvas* importedAgain = semanticCanvas(reopenedAgain);
    check(importedAgain != nullptr,
          "text-patched combined DOCX lost its semantic equation");
    const auto editedSnapshot = importedAgain->snapshot();
    check(editedSnapshot.document.paragraphs().size() == 2 &&
              editedSnapshot.document.tables().size() == 1 &&
              editedSnapshot.document.bodyBlocks().size() == 3 &&
              editedSnapshot.document.bodyBlocks()[1].kind ==
                  BodyBlockKind::table,
          "text-patched combined DOCX did not reopen with a semantic table");
    check(fromUtf16(editedSnapshot.document.paragraphs().back().text()) ==
              QStringLiteral("Tail edited") &&
              editedSnapshot.document.paragraphs().front().equations().size() == 1,
          "text patch or neighboring equation changed after save/reopen");
}

void testTableOnlyImportUsesTrailingEditSurface() {
    namespace ooxml = docxstudio::ooxml;

    QTemporaryDir output;
    check(output.isValid(), "could not create table-only DOCX test directory");
    const QString path = output.filePath(QStringLiteral("table-only.docx"));
    const QByteArray nativePath = QFile::encodeName(path);
    const ooxml::NewTable table{1, 2, false, {"left", "right"}};
    const auto save = ooxml::DocxDocument::writeNew(
        std::filesystem::path(nativePath.constData()),
        ooxml::NewDocumentBody{{table}});
    check(save.saved,
          save.error ? save.error->message.c_str()
                     : "table-only DOCX was not saved");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its table-only native DOCX");
    DocumentCanvas* imported = tableCanvas(reopened);
    check(imported != nullptr,
          "table-only native DOCX did not reconstruct a semantic table");
    const auto snapshot = imported->snapshot();
    check(snapshot.document.tables().size() == 1 &&
              snapshot.document.paragraphs().size() == 1 &&
              snapshot.document.paragraphs().front().text().empty() &&
              snapshot.document.bodyBlocks().size() == 2,
          "table-only import did not retain one required empty edit surface");
    check(snapshot.document.bodyBlocks()[0].kind == BodyBlockKind::table &&
              snapshot.document.bodyBlocks()[1].kind ==
                  BodyBlockKind::paragraph,
          "table-only import placed the synthetic edit surface before the table");
}

void testEditedTableFormattingSurvivesSaveAndReopen() {
    namespace core = docxstudio::core;
    namespace ooxml = docxstudio::ooxml;

    docxstudio::app::MainWindow authored;
    authored.resize(1000, 760);
    authored.show();
    QApplication::processEvents();
    auto* canvas = authored.findChild<DocumentCanvas*>();
    check(canvas != nullptr,
          "could not reach the authored-table canvas");
    check(canvas->insertTable(2, 2, true),
          "could not create the formatted table round-trip fixture");
    const auto inserted = canvas->snapshot();
    check(inserted.document.tables().size() == 1,
          "formatted table fixture did not create one table");
    const auto tableId = inserted.document.tables().front().id();

    const auto populate = [&](std::size_t row, std::size_t column,
                              const QString& text) {
        check(canvas->activateTableCell(tableId, row, column),
              "could not activate a formatted table cell");
        canvas->insertText(text);
    };
    populate(0, 0, QStringLiteral("Alpha"));
    populate(0, 1, QStringLiteral("Bravo"));
    populate(1, 0, QStringLiteral("Charlie"));
    // Cell (1, 1) deliberately remains empty. Formatting its end marker must
    // survive DOCX regeneration and become the format of later typed text.

    check(canvas->selectTableCells(tableId, 0, 0, 1, 1),
          "could not select the 2x2 table range for formatting");
    const QColor requestedColor(QStringLiteral("#336699"));
    constexpr std::int32_t kRequestedHalfPoints = 35;
    canvas->setForeground(requestedColor);
    canvas->setFontPointSize(17.5);
    canvas->setAlignment(core::ParagraphAlignment::right);
    check(canvas->setTableStyle(QStringLiteral("banded-aubergine")),
          "could not apply the non-default table style");
    check(canvas->selectTableCells(tableId, 0, 0, 0, 0),
          "could not select one styled header cell");
    canvas->toggleBold();

    auto snapshot = canvas->snapshot();
    const auto* formatted = snapshot.document.findTable(tableId);
    check(formatted && formatted->style() ==
              core::TableStyle::banded_aubergine,
          "authored table did not retain its requested style");
    check(formatted->cell(0, 0) &&
              formatted->cell(0, 0)->characterFormatAt(1).bold == false,
          "explicit unbold did not override the styled header cell");
    for (const auto& cell : formatted->cells()) {
        const auto character = cell.characterFormatAt(
            cell.text.empty() ? 0U : 1U);
        check(character.foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
                  character.font_size_half_points == kRequestedHalfPoints &&
                  cell.paragraph_format.alignment ==
                      core::ParagraphAlignment::right,
              "formatting did not reach every selected cell before Save As");
    }

    QTemporaryDir output;
    check(output.isValid(),
          "could not create formatted-table DOCX test directory");
    const QString path = output.filePath(
        QStringLiteral("edited-formatted-table.docx"));
    auto* saveAs = authored.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr,
          "could not reach Save As for the formatted table");
    bool selectedDestination = false;
    QTimer::singleShot(0, &authored, [&] {
        auto* dialog = authored.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "formatted-table Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept the formatted-table Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path) &&
              !canvas->isModified(),
          "formatted table was not saved as a clean DOCX baseline");

    const QByteArray xml = zipMember(path, "word/document.xml");
    check(xml.contains(
              "<w:tblStyle w:val=\"ColorfulList-Accent4\"/>") &&
              xml.count("<w:jc w:val=\"right\"/>") >= 4 &&
              xml.count("<w:color w:val=\"336699\"/>") >= 4 &&
              xml.count("<w:sz w:val=\"35\"/>") >= 4 &&
              xml.contains("<w:b w:val=\"0\"/>") &&
              xml.contains("<w:pPr><w:jc w:val=\"right\"/><w:rPr>"),
          "saved OOXML omitted the table style or selected cell formatting");

    ooxml::Error error;
    auto package = ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(package != nullptr,
          error.message.empty() ? "could not inspect formatted-table DOCX"
                                : error.message.c_str());
    const ooxml::ImportedTableBlock* packageTable = nullptr;
    for (const auto& block : package->bodyBlocks()) {
        if (const auto* candidate =
                std::get_if<ooxml::ImportedTableBlock>(&block)) {
            packageTable = candidate;
            break;
        }
    }
    check(packageTable && packageTable->rows == 2 &&
              packageTable->columns == 2 &&
              packageTable->style ==
                  ooxml::BasicTableStyle::banded_aubergine &&
              packageTable->cells.size() == 4,
          "raw DOCX inspection lost table dimensions or style");
    const std::array<std::string, 4> expectedTexts{
        "Alpha", "Bravo", "Charlie", ""};
    for (std::size_t index = 0; index < packageTable->cells.size(); ++index) {
        const auto sourceIndex =
            packageTable->cells[index].source_paragraph_index;
        check(sourceIndex < package->paragraphs().size(),
              "raw DOCX table cell points outside parsed paragraphs");
        const auto& paragraph = package->paragraphs()[sourceIndex];
        check(paragraph.plainText() == expectedTexts[index] &&
                  paragraph.alignment ==
                      ooxml::BasicParagraphAlignment::right &&
                  (index == 3 ? paragraph.runs.empty()
                              : !paragraph.runs.empty()),
              "raw DOCX cell content or paragraph alignment changed");
        check(std::all_of(
                  paragraph.runs.begin(), paragraph.runs.end(),
                  [](const ooxml::Run& run) {
                      return run.format.foreground_rgb == 0x336699U &&
                             run.format.font_size_half_points ==
                                 35;
                  }),
              "raw DOCX cell character formatting changed");
        check(paragraph.paragraph_mark_format.has_value() &&
                  paragraph.paragraph_mark_format->foreground_rgb ==
                      0x336699U &&
                  paragraph.paragraph_mark_format
                          ->font_size_half_points == 35,
              index == 3
                  ? "raw DOCX empty cell lost paragraph-mark insertion formatting"
                  : "raw DOCX non-empty cell lost paragraph-mark insertion formatting");
    }

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its formatted table DOCX");
    auto* reopenedCanvas = tableCanvas(reopened);
    check(reopenedCanvas != nullptr,
          "reopened formatted DOCX has no semantic table");
    snapshot = reopenedCanvas->snapshot();
    const auto& reopenedTable = snapshot.document.tables().front();
    check(reopenedTable.rowCount() == 2 &&
              reopenedTable.columnCount() == 2 &&
              reopenedTable.style() ==
                  core::TableStyle::banded_aubergine &&
              reopenedTable.cells().size() == 4,
          "MainWindow reopen lost formatted table dimensions or style");
    for (std::size_t index = 0; index < reopenedTable.cells().size(); ++index) {
        const auto& cell = reopenedTable.cells()[index];
        const auto character = cell.characterFormatAt(
            cell.text.empty() ? 0U : 1U);
        check(fromUtf16(cell.text).toStdString() == expectedTexts[index] &&
                  character.foreground_argb ==
                      static_cast<std::uint32_t>(requestedColor.rgba()) &&
                  character.font_size_half_points == kRequestedHalfPoints &&
                  cell.default_character_format.foreground_argb ==
                      static_cast<std::uint32_t>(requestedColor.rgba()) &&
                  cell.default_character_format.font_size_half_points ==
                      kRequestedHalfPoints &&
                  cell.paragraph_format.alignment ==
                      core::ParagraphAlignment::right,
              "MainWindow reopen lost table content, character formatting, or paragraph-mark insertion formatting");
    }
    check(reopenedTable.cell(0, 0) &&
              reopenedTable.cell(0, 0)->characterFormatAt(1).bold == false,
          "MainWindow reopen lost the explicit unbold header override");
    check(!reopenedCanvas->isModified(),
          "reopening formatted table DOCX incorrectly marked it modified");

    const auto reopenedTableId = reopenedTable.id();
    check(reopenedCanvas->activateTableCell(reopenedTableId, 1, 1),
          "could not activate the reopened empty formatted cell");
    reopenedCanvas->insertText(QStringLiteral("Z"));
    const auto typed = reopenedCanvas->snapshot();
    const auto* typedTable = typed.document.findTable(reopenedTableId);
    const auto* typedCell = typedTable ? typedTable->cell(1, 1) : nullptr;
    check(typedCell && fromUtf16(typedCell->text) == QStringLiteral("Z") &&
              typedCell->characterFormatAt(1).foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              typedCell->characterFormatAt(1).font_size_half_points ==
                  kRequestedHalfPoints,
          "the reopened empty cell did not retain formatting for later typing");
}

void testNativeListGeometrySurvivesSaveAndReopen() {
    namespace core = docxstudio::core;
    namespace ooxml = docxstudio::ooxml;

    docxstudio::app::MainWindow window;
    window.resize(1000, 760);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<DocumentCanvas*>();
    check(canvas != nullptr, "could not reach the list save canvas");

    canvas->insertText(QStringLiteral("9. alpha"));
    sendKey(*canvas, Qt::Key_Return);
    sendKey(*canvas, Qt::Key_Tab);
    canvas->insertText(QStringLiteral("beta wraps onto another visual line"));
    core::ListLayout layout;
    layout.levels[0] = {2, 3};
    layout.levels[1] = {7, 2};
    check(canvas->setCurrentListLayout(layout),
          "could not apply custom geometry to the generated list");

    auto snapshot = canvas->snapshot();
    check(snapshot.document.paragraphs().size() == 2 &&
              fromUtf16(snapshot.document.paragraphs()[0].text()) ==
                  QStringLiteral("  9.\talpha") &&
              fromUtf16(snapshot.document.paragraphs()[1].text()) ==
                  QStringLiteral("       A.\tbeta wraps onto another visual line"),
          "custom list properties did not normalize the stored prefixes");

    QTemporaryDir output;
    check(output.isValid(), "could not create list DOCX test directory");
    const QString path = output.filePath(QStringLiteral("native-list.docx"));
    auto* saveAs = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr, "could not reach Save As for the list document");
    bool selectedDestination = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "list Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept the list Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path),
          "native list document was not saved");

    ooxml::Error error;
    auto package = ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(package != nullptr,
          error.message.empty() ? "could not reopen raw list package"
                                : error.message.c_str());
    check(package->paragraphs().size() == 2,
          "saved list package has the wrong paragraph count");
    for (const auto& paragraph : package->paragraphs()) {
        check(paragraph.left_indent_twips.has_value() &&
                  paragraph.first_line_indent_twips.value_or(0) < 0 &&
                  paragraph.left_tab_stops_twips.size() == 1 &&
                  static_cast<std::int64_t>(
                      paragraph.left_tab_stops_twips.front()) ==
                      *paragraph.left_indent_twips,
              "saved list paragraph lacks a matching text tab and hanging indent");
    }
    const auto& firstPackageParagraph = package->paragraphs()[0];
    const auto& secondPackageParagraph = package->paragraphs()[1];
    check(firstPackageParagraph.plainText() == "alpha" &&
              secondPackageParagraph.plainText() ==
                  "beta wraps onto another visual line",
          "native list markers were duplicated into paragraph text");
    check(firstPackageParagraph.numbering && secondPackageParagraph.numbering &&
              firstPackageParagraph.numbering->num_id ==
                  secondPackageParagraph.numbering->num_id &&
              firstPackageParagraph.numbering->level == 0 &&
              secondPackageParagraph.numbering->level == 1 &&
              firstPackageParagraph.numbering->marker_text == "9." &&
              secondPackageParagraph.numbering->marker_text == "A.",
          "native list identity, level, start, or marker was not recovered");

    const QByteArray xml = zipMember(path, "word/document.xml");
    check(xml.count("<w:numPr>") == 2 &&
              xml.count("<w:ilvl ") == 2 &&
              xml.count("<w:numId ") == 2 &&
              xml.count("<w:tab/>") == 0,
          "list DOCX does not attach native numPr semantics to each paragraph");
    const QByteArray numbering = zipMember(path, "word/numbering.xml");
    check(numbering.count("<w:lvl ") == 2 &&
              numbering.contains("<w:start w:val=\"9\"/>") &&
              numbering.contains("<w:numFmt w:val=\"decimal\"/>") &&
              numbering.contains("<w:numFmt w:val=\"upperLetter\"/>") &&
              numbering.contains("<w:lvlText w:val=\"%1.\"/>") &&
              numbering.contains("<w:lvlText w:val=\"%2.\"/>") &&
              numbering.count("w:val=\"num\"") == 2 &&
              numbering.count("w:hanging=") == 2,
          "numbering.xml lacks the list formats, starts, or level geometry");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its native-list DOCX");
    auto* imported = listCanvas(reopened, QStringLiteral("beta wraps"));
    check(imported != nullptr, "reopened list was not mapped to the editor");
    snapshot = imported->snapshot();
    check(snapshot.document.paragraphs().size() == 2,
          "reopened list has the wrong paragraph count");
    const auto& first = snapshot.document.paragraphs()[0];
    const auto& second = snapshot.document.paragraphs()[1];
    check(fromUtf16(first.text()) == QStringLiteral("  9.\talpha") &&
              fromUtf16(second.text()) ==
                  QStringLiteral("       A.\tbeta wraps onto another visual line"),
          "reopening native numbering changed the editor list presentation");
    check(first.format().list_id && second.format().list_id &&
              first.format().list_id == second.format().list_id &&
              first.format().list_level == 0 &&
              second.format().list_level == 1 &&
              first.format().list_layout && second.format().list_layout &&
              first.format().list_layout->levels[0] ==
                  core::ListLevelLayout{2, 3} &&
              second.format().list_layout->levels[1] ==
                  core::ListLevelLayout{7, 2},
          "reopening did not recover per-level list geometry");
    check(first.format().left_indent_emu.value_or(0) == 0 &&
              second.format().left_indent_emu.value_or(0) == 0 &&
              !first.format().first_line_indent_emu &&
              !second.format().first_line_indent_emu,
          "DOCX hanging geometry was applied twice inside Owl Docs");
    check(!imported->isModified(),
          "reopening list geometry incorrectly marked the document modified");
}

void testNumberedLevelStylesSurviveSaveAndReopen() {
    namespace core = docxstudio::core;
    namespace ooxml = docxstudio::ooxml;

    constexpr std::size_t kItemsPerLevel = 3;
    const std::array<std::array<QString, kItemsPerLevel>,
                     core::kListLevelCount>
        markers{{
            {QStringLiteral("1."), QStringLiteral("2."), QStringLiteral("3.")},
            {QStringLiteral("A."), QStringLiteral("B."), QStringLiteral("C.")},
            {QStringLiteral("I."), QStringLiteral("II."), QStringLiteral("III.")},
            {QStringLiteral("a."), QStringLiteral("b."), QStringLiteral("c.")},
            {QStringLiteral("i."), QStringLiteral("ii."), QStringLiteral("iii.")},
            {QStringLiteral("1."), QStringLiteral("2."), QStringLiteral("3.")},
            {QStringLiteral("A."), QStringLiteral("B."), QStringLiteral("C.")},
            {QStringLiteral("I."), QStringLiteral("II."), QStringLiteral("III.")},
            {QStringLiteral("a."), QStringLiteral("b."), QStringLiteral("c.")},
            {QStringLiteral("i."), QStringLiteral("ii."), QStringLiteral("iii.")},
        }};

    std::vector<core::Paragraph> paragraphs;
    std::vector<core::NodeId> paragraphIds;
    std::vector<std::size_t> paragraphLevels;
    std::vector<QString> expectedTexts;
    paragraphs.reserve(markers.size() * kItemsPerLevel);
    paragraphIds.reserve(markers.size() * kItemsPerLevel);
    paragraphLevels.reserve(markers.size() * kItemsPerLevel);
    expectedTexts.reserve(markers.size() * kItemsPerLevel);
    for (std::size_t level = 0; level < markers.size(); ++level) {
        for (std::size_t item = 0; item < kItemsPerLevel; ++item) {
            const QString text =
                QString(static_cast<qsizetype>(level * 4U), QLatin1Char(' ')) +
                markers[level][item] + QLatin1Char('\t') +
                QStringLiteral("level %1 item %2").arg(level + 1U).arg(item + 1U);
            auto paragraph = core::Paragraph::create(text.toStdU16String());
            check(paragraph.hasValue(),
                  "could not create a numbered-level test paragraph");
            paragraphIds.push_back(paragraph.value().id());
            paragraphLevels.push_back(level);
            expectedTexts.push_back(text);
            paragraphs.push_back(std::move(paragraph.value()));
        }
    }
    auto document = core::Document::create(std::move(paragraphs));
    check(document.hasValue(),
          "could not create the numbered-level test document");

    const core::NodeId listId = core::NodeId::generate();
    const core::ListLayout layout;
    for (std::size_t index = 0; index < paragraphIds.size(); ++index) {
        core::ParagraphFormatDelta delta;
        delta.list_id = core::PropertyDelta<core::NodeId>::set(listId);
        delta.list_level = core::PropertyDelta<std::uint8_t>::set(
            static_cast<std::uint8_t>(paragraphLevels[index]));
        delta.list_layout = core::PropertyDelta<core::ListLayout>::set(layout);
        check(document.value().applyParagraphFormat(
                  {paragraphIds[index]}, delta).hasValue(),
              "could not assign numbered-level semantics");
    }

    docxstudio::app::MainWindow window;
    auto* canvas = window.findChild<DocumentCanvas*>();
    check(canvas != nullptr,
          "could not reach the numbered-level save canvas");
    canvas->setDocument(std::move(document.value()));

    QTemporaryDir output;
    check(output.isValid(),
          "could not create numbered-level DOCX test directory");
    const QString path = output.filePath(
        QStringLiteral("numbered-level-styles.docx"));
    auto* saveAs = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr,
          "could not reach Save As for the numbered-level document");
    bool selectedDestination = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "numbered-level Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept the numbered-level Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path),
          "numbered-level document was not saved");

    ooxml::Error error;
    auto package = ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(path).constData()), &error);
    check(package != nullptr,
          error.message.empty() ? "could not reopen numbered-level package"
                                : error.message.c_str());
    check(package->paragraphs().size() == expectedTexts.size(),
          "saved numbered-level package has the wrong paragraph count");
    constexpr std::size_t kNativeLevelCount = 9;
    for (std::size_t index = 0; index < expectedTexts.size(); ++index) {
        const auto& importedParagraph = package->paragraphs()[index];
        const QString expectedBody =
            QStringLiteral("level %1 item %2")
                .arg(paragraphLevels[index] + 1U)
                .arg((index % kItemsPerLevel) + 1U);
        const bool nativeLevel =
            paragraphLevels[index] < kNativeLevelCount;
        check(QString::fromUtf8(importedParagraph.plainText()) ==
                  (nativeLevel ? expectedBody : expectedTexts[index]),
              "saving changed native or tenth-level fallback text");
        if (nativeLevel) {
            const QString importedMarker = importedParagraph.numbering
                ? QString::fromUtf8(importedParagraph.numbering->marker_text)
                : QString{};
            const QString expectedMarker =
                markers[paragraphLevels[index]][index % kItemsPerLevel];
            check(importedParagraph.numbering.has_value() &&
                      importedMarker == expectedMarker &&
                      importedParagraph.numbering->level ==
                          paragraphLevels[index],
                  "saving changed a native level-dependent numbered marker");
        } else {
            check(!importedParagraph.numbering.has_value(),
                  "the tenth editor level was emitted as invalid native numbering");
        }
        check(importedParagraph.left_indent_twips.has_value() &&
                  importedParagraph.first_line_indent_twips.value_or(0) < 0 &&
                  importedParagraph.left_tab_stops_twips.size() == 1 &&
                  static_cast<std::int64_t>(
                      importedParagraph.left_tab_stops_twips.front()) ==
                      *importedParagraph.left_indent_twips,
              "a native numbered item lost its level text geometry");
        if (nativeLevel && index > 0) {
            check(importedParagraph.numbering->num_id ==
                      package->paragraphs()[0].numbering->num_id,
                  "one numbered list was serialized as multiple numId values");
        }
    }

    const QByteArray xml = zipMember(path, "word/document.xml");
    check(xml.count("<w:numPr>") ==
                  static_cast<qsizetype>(kNativeLevelCount * kItemsPerLevel) &&
              xml.count("<w:tab/>") ==
                  static_cast<qsizetype>(kItemsPerLevel) &&
              !xml.contains("w:ilvl=\"9\"") && !xml.contains("%10"),
          "numbered-level DOCX did not use the standards-valid tenth-level fallback");
    const QByteArray numbering = zipMember(path, "word/numbering.xml");
    check(numbering.count("<w:lvl ") ==
                  static_cast<qsizetype>(kNativeLevelCount) &&
              numbering.count("<w:num w:numId=") == 1 &&
              numbering.count("<w:numFmt w:val=\"decimal\"/>") == 2 &&
              numbering.count("<w:numFmt w:val=\"upperLetter\"/>") == 2 &&
              numbering.count("<w:numFmt w:val=\"upperRoman\"/>") == 2 &&
              numbering.count("<w:numFmt w:val=\"lowerLetter\"/>") == 2 &&
              numbering.count("<w:numFmt w:val=\"lowerRoman\"/>") == 1 &&
              !numbering.contains("w:ilvl=\"9\"") &&
              !numbering.contains("%10"),
          "numbering.xml exceeded the nine-level native format limit");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its numbered-level DOCX");
    auto* imported = listCanvas(reopened, QStringLiteral("level 10 item 3"));
    check(imported != nullptr,
          "reopened numbered-level list was not mapped to the editor");
    const auto snapshot = imported->snapshot();
    check(snapshot.document.paragraphs().size() == expectedTexts.size(),
          "reopened numbered-level list has the wrong paragraph count");
    std::optional<core::NodeId> reopenedListId;
    for (std::size_t index = 0; index < expectedTexts.size(); ++index) {
        const auto& paragraph = snapshot.document.paragraphs()[index];
        check(fromUtf16(paragraph.text()) == expectedTexts[index],
              "reopening changed a level-dependent numbered marker");
        check(paragraph.format().list_id.has_value() &&
                  paragraph.format().list_level ==
                      static_cast<std::uint8_t>(paragraphLevels[index]) &&
                  paragraph.format().list_layout == layout,
              "reopening did not recover numbered-list level semantics");
        if (!reopenedListId) reopenedListId = paragraph.format().list_id;
        check(paragraph.format().list_id == reopenedListId,
              "reopening split one numbered list into multiple lists");
    }
    check(!imported->isModified(),
          "reopening numbered-level markers incorrectly marked the document modified");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    testEmptyBodyParagraphFormattingSurvivesSaveAndReopen();
    testCombinedNativeReopenKeepsSemanticOrder();
    testTableOnlyImportUsesTrailingEditSurface();
    testEditedTableFormattingSurvivesSaveAndReopen();
    testNativeListGeometrySurvivesSaveAndReopen();
    testNumberedLevelStylesSurviveSaveAndReopen();
    std::cout << "combined DOCX round-trip tests passed\n";
    return 0;
}
