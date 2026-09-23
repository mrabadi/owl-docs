#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/SpellChecker.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRawFont>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextLayout>
#include <QTimer>

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
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
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void sendTextKey(DocumentCanvas& canvas, Qt::Key key, const QString& text,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
}

void sendMouseEvent(DocumentCanvas& canvas, QEvent::Type type,
                    const QPoint& position, Qt::MouseButton button,
                    Qt::MouseButtons buttons) {
    const QPoint globalPosition = canvas.viewport()->mapToGlobal(position);
    QMouseEvent event(type, QPointF(position), QPointF(position),
                      QPointF(globalPosition), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &event);
}

QRect inputMethodCursorRect(DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRect();
}

void commitInputMethodText(DocumentCanvas& canvas, const QString& text) {
    QInputMethodEvent event;
    event.setCommitString(text);
    QApplication::sendEvent(&canvas, &event);
    check(event.isAccepted(), "table IME commit event was not accepted");
}

QStringList paragraphTexts(const DocumentCanvas& canvas) {
    QStringList result;
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(fromUtf16(paragraph.text()));
    }
    return result;
}

docxstudio::core::NodeId insertAndPopulateTable(
    DocumentCanvas& canvas, std::size_t rows, std::size_t columns,
    bool headerRow, const QStringList& values) {
    check(values.size() == static_cast<qsizetype>(rows * columns),
          "table fixture does not contain one value per cell");
    check(canvas.insertTable(rows, columns, headerRow),
          "could not insert a table fixture");
    const auto inserted = canvas.snapshot();
    check(inserted.document.tables().size() == 1,
          "table fixture did not create exactly one table");
    const auto tableId = inserted.document.tables().front().id();
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            check(canvas.activateTableCell(tableId, row, column),
                  "could not activate a table fixture cell");
            canvas.insertText(values[static_cast<qsizetype>(
                row * columns + column)]);
        }
    }
    return tableId;
}

QImage renderedViewport(DocumentCanvas& canvas) {
    QApplication::processEvents();
    return canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
}

int blueSelectionPixels(const QImage& image) {
    int result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 220 &&
                pixel.blue() >= pixel.red() + 8 &&
                pixel.blue() >= pixel.green() + 5) {
                ++result;
            }
        }
    }
    return result;
}

int materiallyDifferentPixels(const QImage& left, const QImage& right,
                              int minimumRgbDistance) {
    check(left.size() == right.size(),
          "render comparison images have different dimensions");
    int result = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QColor a = left.pixelColor(x, y);
            const QColor b = right.pixelColor(x, y);
            const int distance = std::abs(a.red() - b.red()) +
                                 std::abs(a.green() - b.green()) +
                                 std::abs(a.blue() - b.blue());
            if (distance >= minimumRgbDistance) ++result;
        }
    }
    return result;
}

int pixelsNearColor(const QImage& image, std::uint32_t argb,
                    int tolerance = 6) {
    const QColor target = QColor::fromRgba(argb);
    int result = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - target.red()) <= tolerance &&
                std::abs(pixel.green() - target.green()) <= tolerance &&
                std::abs(pixel.blue() - target.blue()) <= tolerance) {
                ++result;
            }
        }
    }
    return result;
}

bool tableMatches(const DocumentCanvas& canvas,
                  docxstudio::core::NodeId tableId,
                  const docxstudio::core::Table& expected) {
    const auto snapshot = canvas.snapshot();
    const auto* table = snapshot.document.findTable(tableId);
    return table && *table == expected;
}

QByteArray zipMember(const QString& path, const char* memberName) {
    int code = 0;
    const auto encodedPath = QFile::encodeName(path);
    zip_t* archive = zip_open(encodedPath.constData(), ZIP_RDONLY, &code);
    check(archive != nullptr, "could not open saved DOCX as a ZIP package");
    zip_stat_t stat{};
    zip_stat_init(&stat);
    check(zip_stat(archive, memberName, ZIP_FL_UNCHANGED, &stat) == 0,
          "saved DOCX is missing word/document.xml");
    check(stat.size <= static_cast<zip_uint64_t>(
              std::numeric_limits<qsizetype>::max()),
          "saved document.xml is too large for the test process");
    zip_file_t* member = zip_fopen(archive, memberName, ZIP_FL_UNCHANGED);
    check(member != nullptr, "could not open saved word/document.xml");
    QByteArray result(static_cast<qsizetype>(stat.size), Qt::Uninitialized);
    const auto read = zip_fread(member, result.data(), stat.size);
    check(read == static_cast<zip_int64_t>(stat.size),
          "could not read saved word/document.xml");
    zip_fclose(member);
    zip_close(archive);
    return result;
}

void testCanvasTableEditing(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    canvas.insertText(QStringLiteral("BeforeAfter"));
    for (int index = 0; index < 5; ++index) sendKey(canvas, Qt::Key_Left);

    check(canvas.insertTable(2, 2, true),
          "could not insert a semantic table at the caret");
    auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().size() == 2 &&
              fromUtf16(snapshot.document.paragraphs()[0].text()) ==
                  QStringLiteral("Before") &&
              fromUtf16(snapshot.document.paragraphs()[1].text()) ==
                  QStringLiteral("After"),
          "table insertion did not atomically split the caret paragraph");
    check(snapshot.document.tables().size() == 1 &&
              snapshot.document.bodyBlocks().size() == 3 &&
              snapshot.document.bodyBlocks()[0].kind == BodyBlockKind::paragraph &&
              snapshot.document.bodyBlocks()[1].kind == BodyBlockKind::table &&
              snapshot.document.bodyBlocks()[2].kind == BodyBlockKind::paragraph,
          "inserted table is not a real ordered body block");
    const auto tableId = snapshot.document.tables().front().id();
    check(canvas.selectedTableId() == tableId,
          "new table was not selected for direct cell editing");

    canvas.insertText(QStringLiteral("Heading 1"));
    sendKey(canvas, Qt::Key_Tab);
    canvas.insertText(QStringLiteral("Heading 2"));
    sendKey(canvas, Qt::Key_Tab);
    canvas.insertText(QStringLiteral("Value"));
    sendKey(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
    canvas.insertText(QStringLiteral("!"));
    snapshot = canvas.snapshot();
    const auto* table = snapshot.document.findTable(tableId);
    check(table && table->rowCount() == 2 && table->columnCount() == 2 &&
              table->hasHeaderRow(),
          "table dimensions or header-row semantics changed during editing");
    check(fromUtf16(table->cell(0, 0)->text) == QStringLiteral("Heading 1") &&
              fromUtf16(table->cell(0, 1)->text) == QStringLiteral("!Heading 2") &&
              fromUtf16(table->cell(1, 0)->text) == QStringLiteral("Value"),
          "Tab/Shift+Tab did not traverse and edit table cells");
    canvas.undo();
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && fromUtf16(table->cell(0, 1)->text) ==
                       QStringLiteral("Heading 2"),
          "cell editing was not undoable");
    canvas.redo();

    check(canvas.selectTable(tableId), "could not select table by its stable ID");
    check(canvas.moveSelectedTable(false),
          "keyboard-accessible table move earlier failed");
    snapshot = canvas.snapshot();
    check(snapshot.document.bodyBlocks().front().id == tableId,
          "Move Table Earlier did not reorder the body block");
    canvas.undo();
    snapshot = canvas.snapshot();
    check(snapshot.document.bodyBlocks()[1].id == tableId,
          "Undo did not restore table body order");
    check(canvas.moveSelectedTable(true),
          "keyboard-accessible table move later failed");
    snapshot = canvas.snapshot();
    check(snapshot.document.bodyBlocks().back().id == tableId,
          "Move Table Later did not reorder the body block");
    canvas.undo();

    QApplication::processEvents();
    const QImage rendered = canvas.grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    const QString renderPath = qEnvironmentVariable(
        "OWL_DOCS_TABLE_RENDER_PATH");
    if (!renderPath.isEmpty()) {
        check(rendered.save(renderPath),
              "could not save the rendered table QA image");
    }
    bool foundLongBlueTableRule = false;
    for (int y = 80; y < rendered.height() - 40 && !foundLongBlueTableRule; ++y) {
        int bluePixels = 0;
        for (int x = 40; x < rendered.width() - 40; ++x) {
            const QColor pixel = rendered.pixelColor(x, y);
            if (pixel.blue() > pixel.red() + 35 &&
                pixel.blue() > pixel.green() + 20) {
                ++bluePixels;
            }
        }
        foundLongBlueTableRule = bluePixels > 180;
    }
    check(foundLongBlueTableRule,
          "screen rendering did not show a bordered selected table");

    QTemporaryDir output;
    check(output.isValid(), "could not create PDF test directory");
    const QString pdf = output.filePath(QStringLiteral("table.pdf"));
    QString error;
    check(canvas.exportPdf(pdf, error) && QFileInfo(pdf).size() > 500,
          "shared page renderer did not export the table to PDF");
    const QString pdftotext = QStandardPaths::findExecutable(
        QStringLiteral("pdftotext"));
    if (!pdftotext.isEmpty()) {
        QProcess process;
        process.start(pdftotext, {pdf, QStringLiteral("-")});
        check(process.waitForFinished(10000) &&
                  process.exitStatus() == QProcess::NormalExit &&
                  process.exitCode() == 0 &&
                  process.readAllStandardOutput().contains("Heading 1"),
              "exported table PDF did not retain searchable cell text");
    }
}

void testActiveCellAndRectangularSelectionPainting(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    const auto tableId = insertAndPopulateTable(
        canvas, 2, 2, false,
        {QStringLiteral("Alpha"), QStringLiteral("Bravo"),
         QStringLiteral("Charlie"), QStringLiteral("Delta")});

    canvas.selectAll();
    canvas.selectAll();
    const QImage neutralTable = renderedViewport(canvas);
    const int neutralBluePixels = blueSelectionPixels(neutralTable);

    check(canvas.activateTableCell(tableId, 0, 0, 2),
          "could not activate the table-cell painting fixture");
    const QImage activeCell = renderedViewport(canvas);
    const int activeBluePixels = blueSelectionPixels(activeCell);
    check(activeBluePixels <= neutralBluePixels + 100 &&
              materiallyDifferentPixels(neutralTable, activeCell, 20) < 500,
          "editing one table cell tinted the cell or table blue");

    check(canvas.selectTableCells(tableId, 0, 0, 1, 0),
          "could not select a rectangular table-cell range");
    const QImage selectedCells = renderedViewport(canvas);
    const int selectedBluePixels = blueSelectionPixels(selectedCells);
    check(selectedBluePixels > activeBluePixels + 1000,
          "an explicit rectangular cell selection had no visible blue tint");
    canvas.hide();
}

void testRectangularCellFormattingAndUndo(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    const auto tableId = insertAndPopulateTable(
        canvas, 2, 2, false,
        {QStringLiteral("One"), QStringLiteral("Two"),
         QStringLiteral("Three"), QStringLiteral("Four")});
    const auto before = canvas.snapshot();
    const auto* beforeTable = before.document.findTable(tableId);
    check(beforeTable != nullptr,
          "formatted-range fixture table is missing");
    std::vector<docxstudio::core::CharacterFormat> originalCharacterFormats;
    std::vector<docxstudio::core::ParagraphFormat> originalParagraphFormats;
    for (const auto& cell : beforeTable->cells()) {
        originalCharacterFormats.push_back(cell.characterFormatAt(1));
        originalParagraphFormats.push_back(cell.paragraph_format);
    }

    check(canvas.selectTableCells(tableId, 0, 0, 1, 1),
          "could not select the 2x2 formatting range");
    const QColor requestedColor(QStringLiteral("#336699"));
    canvas.setForeground(requestedColor);
    canvas.setFontPointSize(18.0);
    canvas.setAlignment(docxstudio::core::ParagraphAlignment::right);

    auto formatted = canvas.snapshot();
    const auto* formattedTable = formatted.document.findTable(tableId);
    check(formattedTable != nullptr,
          "formatted table disappeared");
    for (const auto& cell : formattedTable->cells()) {
        const auto character = cell.characterFormatAt(1);
        check(character.foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
                  character.font_size_half_points == 36,
              "color or font size did not apply to every selected cell");
        check(cell.paragraph_format.alignment ==
                  docxstudio::core::ParagraphAlignment::right,
              "right alignment did not apply to every selected cell");
    }

    canvas.undo();
    auto undone = canvas.snapshot();
    const auto* undoneTable = undone.document.findTable(tableId);
    check(undoneTable != nullptr,
          "table disappeared while undoing cell alignment");
    for (std::size_t index = 0; index < undoneTable->cells().size(); ++index) {
        const auto& cell = undoneTable->cells()[index];
        const auto character = cell.characterFormatAt(1);
        check(cell.paragraph_format == originalParagraphFormats[index] &&
                  character.foreground_argb ==
                      static_cast<std::uint32_t>(requestedColor.rgba()) &&
                  character.font_size_half_points == 36,
              "one Undo did not restore alignment across the selected range");
    }

    canvas.undo();
    undone = canvas.snapshot();
    undoneTable = undone.document.findTable(tableId);
    check(undoneTable != nullptr,
          "table disappeared while undoing cell font size");
    for (std::size_t index = 0; index < undoneTable->cells().size(); ++index) {
        const auto character = undoneTable->cells()[index].characterFormatAt(1);
        check(character.font_size_half_points ==
                  originalCharacterFormats[index].font_size_half_points &&
                  character.foreground_argb ==
                      static_cast<std::uint32_t>(requestedColor.rgba()),
              "one Undo did not restore font size across the selected range");
    }

    canvas.undo();
    undone = canvas.snapshot();
    undoneTable = undone.document.findTable(tableId);
    check(undoneTable != nullptr,
          "table disappeared while undoing cell color");
    for (std::size_t index = 0; index < undoneTable->cells().size(); ++index) {
        check(undoneTable->cells()[index].characterFormatAt(1) ==
                  originalCharacterFormats[index],
              "one Undo did not restore color across the selected range");
    }
}

void testEmptyCellFormattingSurvivesNavigation(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    check(canvas.insertTable(1, 2, false),
          "could not create empty-cell insertion-format fixture");
    const auto initial = canvas.snapshot();
    const auto tableId = initial.document.tables().front().id();
    check(canvas.activateTableCell(tableId, 0, 0),
          "could not activate empty-cell insertion-format fixture");

    const QString requestedFamily = QStringLiteral("DejaVu Serif");
    constexpr double requestedPoints = 17.5;
    const QColor requestedColor(QStringLiteral("#3157a4"));
    canvas.setFontFamily(requestedFamily);
    canvas.setFontPointSize(requestedPoints);
    canvas.setForeground(requestedColor);
    canvas.toggleItalic();

    auto snapshot = canvas.snapshot();
    const auto* table = snapshot.document.findTable(tableId);
    const auto* firstCell = table ? table->cell(0, 0) : nullptr;
    check(firstCell && firstCell->text.empty() &&
              firstCell->default_character_format.font_family ==
                  requestedFamily.toStdString() &&
              firstCell->default_character_format.font_size_half_points == 35 &&
              firstCell->default_character_format.foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              firstCell->default_character_format.italic == true,
          "formatting an empty table cell was not stored durably");

    sendKey(canvas, Qt::Key_Tab);
    sendKey(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
    canvas.refreshCursorFormat();
    check(canvas.currentFontFamily() == requestedFamily &&
              std::abs(canvas.currentFontPointSize() - requestedPoints) < 0.01 &&
              canvas.currentTextColor().rgba() == requestedColor.rgba(),
          "returning to an empty table cell reset its insertion format");

    canvas.insertText(QStringLiteral("Styled"));
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    firstCell = table ? table->cell(0, 0) : nullptr;
    const auto typedFormat = firstCell
        ? firstCell->characterFormatAt(firstCell->text.size())
        : docxstudio::core::CharacterFormat{};
    check(firstCell && firstCell->text == u"Styled" &&
              typedFormat.font_family == requestedFamily.toStdString() &&
              typedFormat.font_size_half_points == 35 &&
              typedFormat.foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              typedFormat.italic == true,
          "typing after returning to an empty cell did not use its stored format");

    canvas.undo();
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    firstCell = table ? table->cell(0, 0) : nullptr;
    check(firstCell && firstCell->text.empty() &&
              firstCell->default_character_format.font_family ==
                  requestedFamily.toStdString(),
          "undoing empty-cell typing also discarded its insertion format");

    // Cover the legacy/import transition where the visible characters are
    // styled but the end-of-cell marker is sparse. Deleting all text must
    // promote the visible style before navigation.
    using docxstudio::core::Document;
    using docxstudio::core::FormatRun;
    using docxstudio::core::NodeId;
    using docxstudio::core::Paragraph;
    using docxstudio::core::Table;
    using docxstudio::core::TableCell;
    auto bodyParagraph = Paragraph::create(u"");
    check(static_cast<bool>(bodyParagraph),
          "could not create table deletion fixture body paragraph");
    auto deletionDocument = Document::create(
        std::vector<Paragraph>{std::move(bodyParagraph.value())});
    check(static_cast<bool>(deletionDocument),
          "could not create table deletion fixture document");
    docxstudio::core::CharacterFormat legacyFormat;
    legacyFormat.font_family = requestedFamily.toStdString();
    legacyFormat.font_size_half_points = 35;
    legacyFormat.foreground_argb =
        static_cast<std::uint32_t>(requestedColor.rgba());
    legacyFormat.italic = true;
    const auto deletionTableId = NodeId::generate();
    std::vector<TableCell> deletionCells;
    deletionCells.emplace_back(
        NodeId::generate(), u"Styled",
        std::vector<FormatRun>{{0, 6, legacyFormat}});
    deletionCells.emplace_back(NodeId::generate(), u"");
    auto deletionTable = Table::restore(
        1, 2, false, deletionTableId, std::move(deletionCells));
    check(static_cast<bool>(deletionTable) &&
              static_cast<bool>(deletionDocument.value().insertTable(
                  std::nullopt, std::move(deletionTable.value()))),
          "could not create table deletion fixture");

    DocumentCanvas deletionCanvas(spelling);
    deletionCanvas.setDocument(std::move(deletionDocument.value()));
    check(deletionCanvas.activateTableCell(deletionTableId, 0, 0, 6),
          "could not activate styled deletion fixture cell");
    sendKey(deletionCanvas, Qt::Key_Home, Qt::ShiftModifier);
    sendKey(deletionCanvas, Qt::Key_Delete);
    snapshot = deletionCanvas.snapshot();
    table = snapshot.document.findTable(deletionTableId);
    firstCell = table ? table->cell(0, 0) : nullptr;
    check(firstCell && firstCell->text.empty() &&
              firstCell->default_character_format.font_family ==
                  requestedFamily.toStdString() &&
              firstCell->default_character_format.font_size_half_points == 35 &&
              firstCell->default_character_format.foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()) &&
              firstCell->default_character_format.italic == true,
          "deleting all styled cell text did not promote its insertion format");

    sendKey(deletionCanvas, Qt::Key_Tab);
    sendKey(deletionCanvas, Qt::Key_Backtab, Qt::ShiftModifier);
    deletionCanvas.refreshCursorFormat();
    check(deletionCanvas.currentFontFamily() == requestedFamily &&
              std::abs(deletionCanvas.currentFontPointSize() -
                       requestedPoints) < 0.01 &&
              deletionCanvas.currentTextColor().rgba() ==
                  requestedColor.rgba(),
          "returning to a deletion-created empty cell reset its format");
    deletionCanvas.insertText(QStringLiteral("Again"));
    snapshot = deletionCanvas.snapshot();
    table = snapshot.document.findTable(deletionTableId);
    firstCell = table ? table->cell(0, 0) : nullptr;
    check(firstCell && firstCell->text == u"Again" &&
              firstCell->characterFormatAt(5).font_family ==
                  requestedFamily.toStdString() &&
              firstCell->characterFormatAt(5).foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()),
          "typing after returning to a deletion-created empty cell lost its format");
}

void testTableRowAndColumnCommandsAndUndo(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas insertionCanvas(spelling);
    const auto insertionTableId = insertAndPopulateTable(
        insertionCanvas, 2, 2, false,
        {QStringLiteral("A"), QStringLiteral("B"),
         QStringLiteral("C"), QStringLiteral("D")});
    const auto originalInsertionTable =
        insertionCanvas.snapshot().document.tables().front();

    check(insertionCanvas.activateTableCell(insertionTableId, 1, 1),
          "could not target Insert Row Above");
    check(insertionCanvas.insertTableRow(false),
          "Insert Row Above failed");
    auto changed = insertionCanvas.snapshot();
    const auto* table = changed.document.findTable(insertionTableId);
    check(table && table->rowCount() == 3 &&
              table->cell(1, 0)->text.empty() &&
              table->cell(1, 1)->text.empty() &&
              fromUtf16(table->cell(2, 0)->text) == QStringLiteral("C"),
          "Insert Row Above inserted at the wrong position");
    insertionCanvas.undo();
    check(tableMatches(insertionCanvas, insertionTableId,
                       originalInsertionTable),
          "Undo did not restore Insert Row Above");

    check(insertionCanvas.activateTableCell(insertionTableId, 0, 0),
          "could not target Insert Row Below");
    check(insertionCanvas.insertTableRow(true),
          "Insert Row Below failed");
    changed = insertionCanvas.snapshot();
    table = changed.document.findTable(insertionTableId);
    check(table && table->rowCount() == 3 &&
              table->cell(1, 0)->text.empty() &&
              table->cell(1, 1)->text.empty() &&
              fromUtf16(table->cell(2, 1)->text) == QStringLiteral("D"),
          "Insert Row Below inserted at the wrong position");
    insertionCanvas.undo();
    check(tableMatches(insertionCanvas, insertionTableId,
                       originalInsertionTable),
          "Undo did not restore Insert Row Below");

    check(insertionCanvas.activateTableCell(insertionTableId, 1, 1),
          "could not target Insert Column Left");
    check(insertionCanvas.insertTableColumn(false),
          "Insert Column Left failed");
    changed = insertionCanvas.snapshot();
    table = changed.document.findTable(insertionTableId);
    check(table && table->columnCount() == 3 &&
              table->cell(0, 1)->text.empty() &&
              table->cell(1, 1)->text.empty() &&
              fromUtf16(table->cell(0, 2)->text) == QStringLiteral("B"),
          "Insert Column Left inserted at the wrong position");
    insertionCanvas.undo();
    check(tableMatches(insertionCanvas, insertionTableId,
                       originalInsertionTable),
          "Undo did not restore Insert Column Left");

    check(insertionCanvas.activateTableCell(insertionTableId, 0, 0),
          "could not target Insert Column Right");
    check(insertionCanvas.insertTableColumn(true),
          "Insert Column Right failed");
    changed = insertionCanvas.snapshot();
    table = changed.document.findTable(insertionTableId);
    check(table && table->columnCount() == 3 &&
              table->cell(0, 1)->text.empty() &&
              table->cell(1, 1)->text.empty() &&
              fromUtf16(table->cell(1, 2)->text) == QStringLiteral("D"),
          "Insert Column Right inserted at the wrong position");
    insertionCanvas.undo();
    check(tableMatches(insertionCanvas, insertionTableId,
                       originalInsertionTable),
          "Undo did not restore Insert Column Right");

    DocumentCanvas deletionCanvas(spelling);
    const auto deletionTableId = insertAndPopulateTable(
        deletionCanvas, 3, 3, false,
        {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
         QStringLiteral("D"), QStringLiteral("E"), QStringLiteral("F"),
         QStringLiteral("G"), QStringLiteral("H"), QStringLiteral("I")});
    const auto originalDeletionTable =
        deletionCanvas.snapshot().document.tables().front();

    check(deletionCanvas.selectTableCells(deletionTableId, 1, 0, 1, 2),
          "could not select a row for deletion");
    check(deletionCanvas.deleteSelectedTableRows(),
          "Delete Rows failed");
    changed = deletionCanvas.snapshot();
    table = changed.document.findTable(deletionTableId);
    check(table && table->rowCount() == 2 &&
              fromUtf16(table->cell(1, 0)->text) == QStringLiteral("G") &&
              fromUtf16(table->cell(1, 2)->text) == QStringLiteral("I"),
          "Delete Rows removed the wrong row");
    deletionCanvas.undo();
    check(tableMatches(deletionCanvas, deletionTableId,
                       originalDeletionTable),
          "Undo did not restore deleted rows");

    check(deletionCanvas.selectTableCells(deletionTableId, 0, 1, 2, 1),
          "could not select a column for deletion");
    check(deletionCanvas.deleteSelectedTableColumns(),
          "Delete Columns failed");
    changed = deletionCanvas.snapshot();
    table = changed.document.findTable(deletionTableId);
    check(table && table->columnCount() == 2 &&
              fromUtf16(table->cell(0, 1)->text) == QStringLiteral("C") &&
              fromUtf16(table->cell(2, 1)->text) == QStringLiteral("I"),
          "Delete Columns removed the wrong column");
    deletionCanvas.undo();
    check(tableMatches(deletionCanvas, deletionTableId,
                       originalDeletionTable),
          "Undo did not restore deleted columns");
}

void testTableStylePalettesRenderAndUndo(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    const auto tableId = insertAndPopulateTable(
        canvas, 3, 2, true,
        {QStringLiteral("Heading A"), QStringLiteral("Heading B"),
         QStringLiteral("Alpha"), QStringLiteral("Bravo"),
         QStringLiteral("Charlie"), QStringLiteral("Delta")});

    check(canvas.setTableStyle(QStringLiteral("medium-blue")),
          "could not apply the medium-blue table style");
    auto snapshot = canvas.snapshot();
    const auto* table = snapshot.document.findTable(tableId);
    check(table && table->style() ==
              docxstudio::core::TableStyle::medium_blue,
          "medium-blue style was not recorded in the document model");
    const QImage blueStyle = renderedViewport(canvas);

    check(canvas.setTableStyle(QStringLiteral("medium-orange")),
          "could not apply the medium-orange table style");
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && table->style() ==
              docxstudio::core::TableStyle::medium_orange,
          "medium-orange style was not recorded in the document model");
    const QImage orangeStyle = renderedViewport(canvas);
    check(materiallyDifferentPixels(blueStyle, orangeStyle, 35) > 1000,
          "distinct table style palettes rendered identically");

    canvas.undo();
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && table->style() ==
              docxstudio::core::TableStyle::medium_blue,
          "Undo did not restore the previous table style");
    const QImage restoredBlueStyle = renderedViewport(canvas);
    check(materiallyDifferentPixels(blueStyle, restoredBlueStyle, 8) < 100,
          "Undo did not restore the previous visible table palette");
    canvas.hide();
}

void testImportedStylePrecedenceAndStableCellPresentation(
    docxstudio::app::SpellChecker& spelling) {
    using docxstudio::app::ImportedCellBorderPresentation;
    using docxstudio::app::ImportedTableCellPresentation;
    using docxstudio::app::ImportedTablePresentation;
    using docxstudio::core::Document;
    using docxstudio::core::NodeId;
    using docxstudio::core::Paragraph;
    using docxstudio::core::Table;
    using docxstudio::core::TableCell;
    using docxstudio::core::TableStyle;

    auto paragraph = Paragraph::create(u"");
    check(static_cast<bool>(paragraph),
          "could not create the imported-style body paragraph");
    std::vector<Paragraph> paragraphs;
    paragraphs.push_back(std::move(paragraph.value()));
    auto document = Document::create(std::move(paragraphs));
    check(static_cast<bool>(document),
          "could not create the imported-style document");

    const NodeId tableId = NodeId::generate();
    const QStringList texts{QStringLiteral("A"), QStringLiteral("B"),
                            QStringLiteral("C"), QStringLiteral("D")};
    std::vector<TableCell> cells;
    std::vector<NodeId> cellIds;
    for (const auto& text : texts) {
        const NodeId cellId = NodeId::generate();
        cellIds.push_back(cellId);
        cells.emplace_back(cellId, text.toStdU16String());
    }
    auto table = Table::restore(2, 2, true, tableId, std::move(cells),
                                TableStyle::light_blue);
    check(static_cast<bool>(table) &&
              static_cast<bool>(document.value().insertTable(
                  std::nullopt, std::move(table.value()))),
          "could not create the recognized imported-style table");

    constexpr std::uint32_t kDirectBorder = 0xffff0000U;
    const std::vector<std::uint32_t> directFills{
        0xffff00ffU, 0xff00ff00U, 0xff00ffffU, 0xffffff00U};
    ImportedTablePresentation presentation;
    presentation.tableId = tableId;
    presentation.columnWidthsPoints = {80.0, 240.0};
    presentation.cellIds = cellIds;
    presentation.sourceSemanticStyle = TableStyle::light_blue;
    const ImportedCellBorderPresentation directBorder{kDirectBorder, 2.0};
    for (std::size_t index = 0; index < cellIds.size(); ++index) {
        ImportedTableCellPresentation cellPresentation;
        cellPresentation.sourceText = texts[static_cast<qsizetype>(index)];
        cellPresentation.fillArgb = directFills[index];
        cellPresentation.borderTop = directBorder;
        cellPresentation.borderRight = directBorder;
        cellPresentation.borderBottom = directBorder;
        cellPresentation.borderLeft = directBorder;
        presentation.cells.push_back(std::move(cellPresentation));
    }

    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.setDocument(std::move(document.value()));
    canvas.setImportedPresentation({}, {std::move(presentation)});
    canvas.show();

    const auto allDirectFillsAreVisible = [&](const QImage& image) {
        return std::all_of(
            directFills.begin(), directFills.end(),
            [&image](std::uint32_t fill) {
                return pixelsNearColor(image, fill) > 250;
            });
    };
    const QImage initial = renderedViewport(canvas);
    check(allDirectFillsAreVisible(initial),
          "a recognized imported table style overwrote a direct cell fill");
    check(pixelsNearColor(initial, kDirectBorder) > 100,
          "a recognized imported table style overwrote direct cell borders");

    const auto caretX = [&](std::size_t row, std::size_t column) {
        check(canvas.activateTableCell(tableId, row, column),
              "could not activate a cell while measuring imported widths");
        QApplication::processEvents();
        return inputMethodCursorRect(canvas).center().x();
    };

    check(canvas.activateTableCell(tableId, 0, 0),
          "could not target the imported table for row insertion");
    check(canvas.insertTableRow(true),
          "could not insert a row into the imported table");
    const int preservedFirstColumnWidth =
        std::abs(caretX(0, 1) - caretX(0, 0));
    check(preservedFirstColumnWidth < 160,
          "a row insertion discarded compatible imported column widths");
    const QImage afterRow = renderedViewport(canvas);
    check(allDirectFillsAreVisible(afterRow) &&
              pixelsNearColor(afterRow, kDirectBorder) > 100,
          "a row insertion detached presentation from surviving stable cell IDs");

    check(canvas.activateTableCell(tableId, 0, 0),
          "could not target the imported table for column insertion");
    check(canvas.insertTableColumn(true),
          "could not insert a column into the imported table");
    const int defaultFirstColumnWidth =
        std::abs(caretX(0, 1) - caretX(0, 0));
    check(defaultFirstColumnWidth > 170,
          "incompatible imported widths leaked across a column-count change");
    const QImage afterColumn = renderedViewport(canvas);
    check(allDirectFillsAreVisible(afterColumn) &&
              pixelsNearColor(afterColumn, kDirectBorder) > 100,
          "a column insertion detached presentation from surviving stable cell IDs");
    check(pixelsNearColor(afterColumn, 0xff5b9bd5U) > 250,
          "a newly inserted header cell did not receive semantic style presentation");
    canvas.hide();
}

void testImportedTableUsesUnroundedAutomaticLineAdvance(
    docxstudio::app::SpellChecker& spelling) {
    constexpr double kFontPointSize = 8.5;
    constexpr double kSmallerFontPointSize = 8.0;
    constexpr double kVerticalPaddingPoints = 4.5;
    constexpr double kBorderWidthPoints = 1.0;
    constexpr double kColumnWidthPoints = 180.0;
    constexpr int kZoomPercent = 200;
    constexpr std::uint32_t kBorderArgb = 0xffff00ffU;
    const QString cellText = QStringLiteral(
        "Alpha\u2028Bravo\u2028Charlie\u2028Delta");
    const QString fallbackCellText = QStringLiteral("\u6f22\u5b57");

    auto paragraph = docxstudio::core::Paragraph::create(u"");
    check(static_cast<bool>(paragraph),
          "could not create empty paragraph for imported table fixture");
    std::vector<docxstudio::core::Paragraph> paragraphs;
    paragraphs.push_back(std::move(paragraph.value()));
    auto document = docxstudio::core::Document::create(std::move(paragraphs));
    check(static_cast<bool>(document),
          "could not create document for imported table fixture");
    auto table = docxstudio::core::Table::create(
        3, 1, false, docxstudio::core::NodeId::generate(), std::nullopt);
    check(static_cast<bool>(table),
          "could not create imported table fixture");
    const auto tableId = table.value().id();
    check(static_cast<bool>(document.value().insertTable(
              std::nullopt, std::move(table.value()))),
          "could not insert imported table fixture");
    check(static_cast<bool>(document.value().setTableCellText(
              tableId, 0, 0, cellText.toStdU16String())),
          "could not populate imported table fixture");
    check(static_cast<bool>(document.value().setTableCellText(
              tableId, 1, 0, cellText.toStdU16String())),
          "could not populate mixed-run imported table fixture");
    check(static_cast<bool>(document.value().setTableCellText(
              tableId, 2, 0, fallbackCellText.toStdU16String())),
          "could not populate CJK-fallback imported table fixture");

    docxstudio::core::CharacterFormat cellFormat;
    cellFormat.font_family = "Ubuntu";
    cellFormat.font_size_half_points = 17;
    docxstudio::app::ImportedTableCellPresentation cellPresentation;
    cellPresentation.sourceText = cellText;
    cellPresentation.formats.push_back({
        0, static_cast<std::size_t>(cellText.size()), cellFormat});
    cellPresentation.lineSpacing = 240;
    cellPresentation.lineSpacingRule =
        docxstudio::core::LineSpacingRule::automatic;
    cellPresentation.paddingTopPoints = kVerticalPaddingPoints;
    cellPresentation.paddingBottomPoints = kVerticalPaddingPoints;
    const docxstudio::app::ImportedCellBorderPresentation border{
        kBorderArgb, kBorderWidthPoints};
    cellPresentation.borderTop = border;
    cellPresentation.borderRight = border;
    cellPresentation.borderBottom = border;
    cellPresentation.borderLeft = border;
    const qsizetype firstSeparator = cellText.indexOf(QChar::LineSeparator);
    check(firstSeparator > 0,
          "mixed-run table fixture is missing its first forced line break");
    docxstudio::core::CharacterFormat smallerCellFormat;
    smallerCellFormat.font_family = "Ubuntu";
    smallerCellFormat.font_size_half_points = 16;
    auto mixedCellPresentation = cellPresentation;
    mixedCellPresentation.formats = {
        {0, static_cast<std::size_t>(firstSeparator + 1), cellFormat},
        {static_cast<std::size_t>(firstSeparator + 1),
         static_cast<std::size_t>(cellText.size()), smallerCellFormat}};
    auto fallbackCellPresentation = cellPresentation;
    fallbackCellPresentation.sourceText = fallbackCellText;
    fallbackCellPresentation.formats = {
        {0, static_cast<std::size_t>(fallbackCellText.size()),
         smallerCellFormat}};
    docxstudio::app::ImportedTablePresentation tablePresentation;
    tablePresentation.tableId = tableId;
    tablePresentation.columnWidthsPoints = {kColumnWidthPoints};
    tablePresentation.cells = {
        cellPresentation, mixedCellPresentation, fallbackCellPresentation};

    DocumentCanvas canvas(spelling);
    canvas.resize(1000, 700);
    canvas.setZoomPercent(kZoomPercent);
    canvas.setDocument(std::move(document.value()));
    canvas.setImportedPresentation({}, {std::move(tablePresentation)});
    canvas.show();
    canvas.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();

    const QRect emptyParagraphCaret = inputMethodCursorRect(canvas);
    check(emptyParagraphCaret.width() > 0 &&
              emptyParagraphCaret.height() > 0 &&
              emptyParagraphCaret.intersects(canvas.viewport()->rect()),
          "empty body paragraph did not expose a visible caret rectangle");

    const QImage rendered = canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    const double pixelsPerPoint =
        (96.0 / 72.0) * (kZoomPercent / 100.0) *
        rendered.devicePixelRatio();
    const int minimumHorizontalBorderPixels = static_cast<int>(std::lround(
        kColumnWidthPoints * pixelsPerPoint * 0.6));
    std::vector<int> horizontalBorderRows;
    for (int y = 0; y < rendered.height(); ++y) {
        int borderPixels = 0;
        for (int x = 0; x < rendered.width(); ++x) {
            const QColor pixel = rendered.pixelColor(x, y);
            if (pixel.red() > 180 && pixel.green() < 120 &&
                pixel.blue() > 180) {
                ++borderPixels;
            }
        }
        if (borderPixels >= minimumHorizontalBorderPixels) {
            horizontalBorderRows.push_back(y);
        }
    }
    check(!horizontalBorderRows.empty(),
          "rendered imported table had no visible custom horizontal border");
    std::vector<std::pair<int, int>> borderClusters;
    for (const int row : horizontalBorderRows) {
        if (borderClusters.empty() ||
            row > borderClusters.back().second + 1) {
            borderClusters.emplace_back(row, row);
        } else {
            borderClusters.back().second = row;
        }
    }
    check(borderClusters.size() == 4,
          "rendered three-row imported table did not expose four horizontal borders");
    const double topBorderCenter =
        (borderClusters.front().first + borderClusters.front().second) / 2.0;
    const double firstInteriorBorderCenter =
        (borderClusters[1].first + borderClusters[1].second) / 2.0;
    const double secondInteriorBorderCenter =
        (borderClusters[2].first + borderClusters[2].second) / 2.0;
    const double bottomBorderCenter =
        (borderClusters.back().first + borderClusters.back().second) / 2.0;
    const double observedRowHeightPixels =
        firstInteriorBorderCenter - topBorderCenter;
    const double observedMixedRowHeightPixels =
        secondInteriorBorderCenter - firstInteriorBorderCenter;
    const double observedFallbackRowHeightPixels =
        bottomBorderCenter - secondInteriorBorderCenter;

    QFont font(QStringLiteral("Ubuntu"));
    const int realizedPixelSize = static_cast<int>(
        std::lround(kFontPointSize));
    font.setPixelSize(realizedPixelSize);
    font.setStretch(static_cast<int>(std::lround(
        kFontPointSize * 100.0 / realizedPixelSize)));
    font.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        static_cast<int>(font.styleStrategy()) |
        static_cast<int>(QFont::PreferNoShaping)));
    const QRawFont rawFont = QRawFont::fromFont(font);
    check(rawFont.isValid() && rawFont.pixelSize() > 0.0,
          "Ubuntu OpenType metrics were unavailable for the table regression");
    const double openTypeAdvance =
        (rawFont.ascent() + rawFont.descent() + rawFont.leading()) *
        kFontPointSize / rawFont.pixelSize();
    check(std::isfinite(openTypeAdvance) && openTypeAdvance > 0.0,
          "Ubuntu OpenType line advance was invalid");
    const double expectedRowHeightPoints = openTypeAdvance * 4.0 +
        kVerticalPaddingPoints * 2.0 + kBorderWidthPoints;
    const double expectedRowHeightPixels =
        expectedRowHeightPoints * pixelsPerPoint;

    QTextLayout roundedLayout(cellText, font);
    QTextOption roundedOption;
    roundedOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    roundedOption.setUseDesignMetrics(true);
    roundedLayout.setTextOption(roundedOption);
    double roundedContentHeightPoints = 0.0;
    int roundedLineCount = 0;
    roundedLayout.beginLayout();
    while (true) {
        QTextLine line = roundedLayout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(kColumnWidthPoints -
                          cellPresentation.paddingLeftPoints -
                          cellPresentation.paddingRightPoints);
        roundedContentHeightPoints += line.height();
        ++roundedLineCount;
    }
    roundedLayout.endLayout();
    check(roundedLineCount == 4,
          "four-line table fixture did not create four QTextLayout lines");
    const double roundedRowHeightPixels =
        (roundedContentHeightPoints + kVerticalPaddingPoints * 2.0 +
         kBorderWidthPoints) * pixelsPerPoint;
    const double openTypeError = std::abs(
        observedRowHeightPixels - expectedRowHeightPixels);
    const double roundedLineError = std::abs(
        observedRowHeightPixels - roundedRowHeightPixels);
    if (openTypeError > 2.0 || openTypeError >= roundedLineError) {
        std::cerr << "Observed imported row height "
                  << observedRowHeightPixels << " px; expected OpenType "
                  << expectedRowHeightPixels << " px; rounded QTextLine "
                  << roundedRowHeightPixels << " px\n";
        check(false,
              "imported automatic table height did not use unrounded OpenType line advances");
    }

    QFont smallerFont(QStringLiteral("Ubuntu"));
    smallerFont.setPixelSize(static_cast<int>(kSmallerFontPointSize));
    smallerFont.setStretch(100);
    smallerFont.setStyleStrategy(static_cast<QFont::StyleStrategy>(
        static_cast<int>(smallerFont.styleStrategy()) |
        static_cast<int>(QFont::PreferNoShaping)));
    const QRawFont smallerRawFont = QRawFont::fromFont(smallerFont);
    check(smallerRawFont.isValid() && smallerRawFont.pixelSize() > 0.0,
          "8 pt Ubuntu OpenType metrics were unavailable for the mixed-run regression");
    const double smallerOpenTypeAdvance =
        (smallerRawFont.ascent() + smallerRawFont.descent() +
         smallerRawFont.leading()) * kSmallerFontPointSize /
        smallerRawFont.pixelSize();
    check(std::isfinite(smallerOpenTypeAdvance) &&
              smallerOpenTypeAdvance > 0.0,
          "8 pt Ubuntu OpenType line advance was invalid");
    const double expectedMixedRowHeightPixels =
        (openTypeAdvance + smallerOpenTypeAdvance * 3.0 +
         kVerticalPaddingPoints * 2.0 + kBorderWidthPoints) *
        pixelsPerPoint;
    const double leakedFirstRunHeightPixels = expectedRowHeightPixels;
    const double mixedOpenTypeError = std::abs(
        observedMixedRowHeightPixels - expectedMixedRowHeightPixels);
    const double leakedFirstRunError = std::abs(
        observedMixedRowHeightPixels - leakedFirstRunHeightPixels);
    if (mixedOpenTypeError > 2.0 ||
        mixedOpenTypeError >= leakedFirstRunError) {
        std::cerr << "Observed mixed-run row height "
                  << observedMixedRowHeightPixels
                  << " px; expected per-line OpenType "
                  << expectedMixedRowHeightPixels
                  << " px; leaked first-run metric "
                  << leakedFirstRunHeightPixels << " px\n";
        check(false,
              "imported mixed-run table height leaked the first run metric into later lines");
    }

    QTextLayout fallbackLayout(fallbackCellText, smallerFont);
    QTextOption fallbackOption;
    fallbackOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    fallbackOption.setUseDesignMetrics(true);
    fallbackLayout.setTextOption(fallbackOption);
    fallbackLayout.beginLayout();
    QTextLine fallbackLine = fallbackLayout.createLine();
    check(fallbackLine.isValid(),
          "CJK fallback table fixture did not create a QTextLayout line");
    fallbackLine.setLineWidth(kColumnWidthPoints -
                              fallbackCellPresentation.paddingLeftPoints -
                              fallbackCellPresentation.paddingRightPoints);
    double fallbackGlyphRunAdvance = 0.0;
    QString fallbackFamily;
    bool sawFallbackFont = false;
    for (const auto& glyphRun : fallbackLine.glyphRuns()) {
        const QRawFont glyphFont = glyphRun.rawFont();
        if (!glyphFont.isValid() || glyphFont.pixelSize() <= 0.0) continue;
        const double designAdvance = glyphFont.ascent() +
            glyphFont.descent() + glyphFont.leading();
        if (!std::isfinite(designAdvance) || designAdvance <= 0.0) continue;
        const double normalizedAdvance = designAdvance *
            kSmallerFontPointSize / glyphFont.pixelSize();
        if (normalizedAdvance > fallbackGlyphRunAdvance) {
            fallbackGlyphRunAdvance = normalizedAdvance;
            fallbackFamily = glyphFont.familyName();
        }
        if (glyphFont.familyName().compare(
                smallerRawFont.familyName(), Qt::CaseInsensitive) != 0) {
            sawFallbackFont = true;
        }
    }
    check(!fallbackLayout.createLine().isValid(),
          "short CJK fallback table fixture unexpectedly wrapped");
    fallbackLayout.endLayout();
    check(sawFallbackFont,
          "CJK table fixture did not select a fallback glyph-run font");
    check(std::isfinite(fallbackGlyphRunAdvance) &&
              fallbackGlyphRunAdvance > smallerOpenTypeAdvance,
          "CJK fallback glyph-run advance was not taller than primary Ubuntu");
    const double expectedFallbackRowHeightPixels =
        (fallbackGlyphRunAdvance + kVerticalPaddingPoints * 2.0 +
         kBorderWidthPoints) * pixelsPerPoint;
    const double primaryOnlyRowHeightPixels =
        (smallerOpenTypeAdvance + kVerticalPaddingPoints * 2.0 +
         kBorderWidthPoints) * pixelsPerPoint;
    const double fallbackGlyphRunError = std::abs(
        observedFallbackRowHeightPixels - expectedFallbackRowHeightPixels);
    const double primaryOnlyError = std::abs(
        observedFallbackRowHeightPixels - primaryOnlyRowHeightPixels);
    if (fallbackGlyphRunError > 2.0 ||
        fallbackGlyphRunError >= primaryOnlyError ||
        observedFallbackRowHeightPixels + 1.0 <
            expectedFallbackRowHeightPixels) {
        std::cerr << "Observed CJK-fallback row height "
                  << observedFallbackRowHeightPixels
                  << " px; expected glyph-run font "
                  << fallbackFamily.toStdString() << ' '
                  << expectedFallbackRowHeightPixels
                  << " px; primary Ubuntu-only "
                  << primaryOnlyRowHeightPixels << " px\n";
        check(false,
              "imported CJK table row did not use its actual fallback glyph-run advance");
    }
    canvas.hide();
}

void testMouseHandleMovesTable(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    canvas.insertText(QStringLiteral("BeforeAfter"));
    for (int index = 0; index < 5; ++index) {
        sendKey(canvas, Qt::Key_Left);
    }
    check(canvas.insertTable(1, 1, false),
          "could not prepare the table drag fixture");
    const auto initial = canvas.snapshot();
    const auto tableId = initial.document.tables().front().id();
    check(initial.document.bodyBlocks()[1].id == tableId,
          "table drag fixture did not start between its paragraphs");

    // SelectAll first promotes the active cell to a whole-table selection;
    // the second invocation selects body text and exposes the normal purple
    // move handle, which can be located without relying on private geometry.
    canvas.selectAll();
    canvas.selectAll();
    QApplication::processEvents();
    const QImage image = canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    QPoint handle(-1, -1);
    const QColor handleColor(QStringLiteral("#5e2750"));
    for (int y = 0; y < image.height() && handle.x() < 0; ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == handleColor) {
                handle = QPoint(x, y);
                break;
            }
        }
    }
    check(handle.x() >= 0,
          "rendered table has no discoverable move handle");

    sendMouseEvent(canvas, QEvent::MouseButtonPress, handle,
                   Qt::LeftButton, Qt::LeftButton);
    const QPoint destination(handle.x(), canvas.viewport()->height() - 40);
    sendMouseEvent(canvas, QEvent::MouseMove, destination,
                   Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, destination,
                   Qt::LeftButton, Qt::NoButton);
    const auto moved = canvas.snapshot();
    check(moved.document.bodyBlocks().back().id == tableId,
          "dragging the visible table handle did not move the table block");
    canvas.hide();
}

void testInsertionIsOneUndoTransaction(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("left-right"));
    sendKey(canvas, Qt::Key_Home);
    for (int index = 0; index < 4; ++index) sendKey(canvas, Qt::Key_Right);
    check(canvas.insertTable(3, 2, false),
          "atomic insertion setup failed");
    canvas.undo();
    const auto snapshot = canvas.snapshot();
    check(snapshot.document.tables().empty() &&
              snapshot.document.paragraphs().size() == 1 &&
              fromUtf16(snapshot.document.paragraphs().front().text()) ==
                  QStringLiteral("left-right"),
          "one Undo did not reverse table insertion and paragraph split");
}

void testSelectedTableCommandsDoNotMutateStaleBody(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas pasteCanvas(spelling);
    pasteCanvas.insertText(QStringLiteral("left-right"));
    sendKey(pasteCanvas, Qt::Key_Home);
    for (int index = 0; index < 4; ++index) {
        sendKey(pasteCanvas, Qt::Key_Right);
    }
    check(pasteCanvas.insertTable(1, 1, false),
          "could not prepare selected-table paste fixture");
    const auto pasteSnapshot = pasteCanvas.snapshot();
    const auto pasteTableId = pasteSnapshot.document.tables().front().id();
    check(pasteCanvas.selectTable(pasteTableId),
          "could not select the paste fixture table");
    const QStringList bodyBeforePaste = paragraphTexts(pasteCanvas);
    QApplication::clipboard()->setText(QStringLiteral("must not leak"));
    sendKey(pasteCanvas, Qt::Key_V, Qt::ControlModifier);
    check(paragraphTexts(pasteCanvas) == bodyBeforePaste,
          "Paste on a selected table mutated the stale body-text selection");

    DocumentCanvas commandCanvas(spelling);
    commandCanvas.insertText(QStringLiteral("before-after"));
    sendKey(commandCanvas, Qt::Key_Home);
    for (int index = 0; index < 6; ++index) {
        sendKey(commandCanvas, Qt::Key_Right);
    }
    check(commandCanvas.insertTable(1, 1, false),
          "could not prepare active-table command fixture");
    commandCanvas.insertText(QStringLiteral("cell"));
    const auto activeSnapshot = commandCanvas.snapshot();
    const auto activeTableId = activeSnapshot.document.tables().front().id();
    const QStringList bodyBeforeCommands = paragraphTexts(commandCanvas);
    check(!commandCanvas.insertTable(2, 2, true),
          "Insert Table inside a cell silently inserted at the stale body caret");
    commandCanvas.toggleBullets();
    commandCanvas.toggleNumbering();
    check(commandCanvas.snapshot().document.tables().size() == 1 &&
              paragraphTexts(commandCanvas) == bodyBeforeCommands,
          "list/table commands in an active cell mutated stale body content");

    check(commandCanvas.selectTable(activeTableId),
          "could not select table for command-routing fixture");
    check(!commandCanvas.insertTable(2, 2, true),
          "Insert Table on a selected table silently used the stale body caret");
    commandCanvas.toggleBullets();
    commandCanvas.toggleNumbering();
    check(commandCanvas.snapshot().document.tables().size() == 1 &&
              paragraphTexts(commandCanvas) == bodyBeforeCommands,
          "list/table commands on a selected table mutated stale body content");
}

void testTabAtLastCellAppendsOneUndoableRow(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    check(canvas.insertTable(1, 2, false),
          "could not prepare final-cell Tab fixture");
    canvas.insertText(QStringLiteral("first"));
    sendKey(canvas, Qt::Key_Tab);
    canvas.insertText(QStringLiteral("last"));
    sendKey(canvas, Qt::Key_Tab);

    auto snapshot = canvas.snapshot();
    const auto tableId = snapshot.document.tables().front().id();
    const auto* table = snapshot.document.findTable(tableId);
    check(table && table->rowCount() == 2,
          "Tab in the final table cell did not append a row");
    canvas.insertText(QStringLiteral("new row"));
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && fromUtf16(table->cell(1, 0)->text) ==
                       QStringLiteral("new row"),
          "caret did not move into the appended row after final-cell Tab");

    canvas.undo();
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && table->rowCount() == 2 &&
              table->cell(1, 0)->text.empty(),
          "Undo of typing in an appended row also removed the row");
    canvas.undo();
    snapshot = canvas.snapshot();
    table = snapshot.document.findTable(tableId);
    check(table && table->rowCount() == 1,
          "the final-cell Tab row was not one undoable table transaction");
}

void testDoubleClickSelectsTableCellWord(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    check(canvas.insertTable(1, 1, false),
          "could not prepare table-cell word-selection fixture");
    canvas.insertText(QStringLiteral("alpha beta"));
    sendKey(canvas, Qt::Key_Home);
    for (int index = 0; index < 8; ++index) {
        sendKey(canvas, Qt::Key_Right);
    }
    QApplication::processEvents();
    const QPoint clickPosition = inputMethodCursorRect(canvas).center();
    sendMouseEvent(canvas, QEvent::MouseButtonPress, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, clickPosition,
                   Qt::LeftButton, Qt::NoButton);
    sendMouseEvent(canvas, QEvent::MouseButtonDblClick, clickPosition,
                   Qt::LeftButton, Qt::LeftButton);
    check(canvas.selectedText() == QStringLiteral("beta"),
          "double-click in a table cell did not select the clicked word");
    canvas.insertText(QStringLiteral("replacement"));
    const auto snapshot = canvas.snapshot();
    const auto& table = snapshot.document.tables().front();
    check(fromUtf16(table.cell(0, 0)->text) ==
              QStringLiteral("alpha replacement"),
          "typing did not replace a double-clicked table-cell word");
    canvas.hide();
}

void testTableContextSelectionAndClipboard(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    const QStringList values{
        QStringLiteral("alpha"), QStringLiteral("bravo"),
        QStringLiteral("charlie"), QStringLiteral("delta"),
        QStringLiteral("echo"), QStringLiteral("foxtrot"),
        QStringLiteral("golf"), QStringLiteral("hotel"),
        QStringLiteral("india")};
    const auto tableId = insertAndPopulateTable(
        canvas, 3, 3, false, values);
    const auto originalTable =
        canvas.snapshot().document.tables().front();

    const auto pointForCell = [&](std::size_t row, std::size_t column,
                                  std::size_t offset = 0) {
        check(canvas.activateTableCell(tableId, row, column, offset),
              "could not locate a table cell for context-menu testing");
        QApplication::processEvents();
        return inputMethodCursorRect(canvas).center();
    };
    const QPoint topLeft = pointForCell(0, 0);
    const QPoint middle = pointForCell(1, 1);
    const QPoint outside = pointForCell(2, 2);

    sendMouseEvent(canvas, QEvent::MouseButtonPress, topLeft,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseMove, middle,
                   Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(canvas, QEvent::MouseButtonRelease, middle,
                   Qt::LeftButton, Qt::NoButton);
    const QString rectangularText =
        QStringLiteral("alpha\tbravo\ndelta\techo");
    check(canvas.selectedText() == rectangularText,
          "mouse drag did not create the expected rectangular cell selection");

    bool preservedInsideSelection = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu != nullptr,
              "right-clicking selected table cells did not open a menu");
        auto* cut = menu->findChild<QAction*>(QStringLiteral("context.cut"));
        auto* copy = menu->findChild<QAction*>(QStringLiteral("context.copy"));
        auto* deleteTable = menu->findChild<QAction*>(
            QStringLiteral("context.deleteTable"));
        check(cut && copy && cut->isEnabled() && copy->isEnabled(),
              "rectangular table selection did not enable Cut and Copy");
        check(deleteTable == nullptr,
              "a cell-range context menu exposed destructive whole-table commands");
        preservedInsideSelection = canvas.selectedText() == rectangularText;
        menu->close();
    });
    QContextMenuEvent insideEvent(
        QContextMenuEvent::Mouse, middle,
        canvas.viewport()->mapToGlobal(middle));
    QApplication::sendEvent(canvas.viewport(), &insideEvent);
    check(preservedInsideSelection,
          "right-click inside a rectangular cell selection collapsed it");

    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    canvas.copy();
    check(QApplication::clipboard()->text() == rectangularText,
          "Copy did not retain explicit rectangular cell-selection behavior");
    canvas.cut();
    auto snapshot = canvas.snapshot();
    const auto* cutTable = snapshot.document.findTable(tableId);
    check(cutTable && cutTable->cell(0, 0)->text.empty() &&
              cutTable->cell(0, 1)->text.empty() &&
              cutTable->cell(1, 0)->text.empty() &&
              cutTable->cell(1, 1)->text.empty() &&
              QApplication::clipboard()->text() == rectangularText,
          "Cut did not clear exactly the selected table-cell rectangle");
    canvas.undo();
    check(tableMatches(canvas, tableId, originalTable),
          "Undo did not restore a rectangular table-cell Cut");

    bool collapsedActionsDisabled = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu != nullptr,
              "right-clicking an unselected table cell did not open a menu");
        auto* cut = menu->findChild<QAction*>(QStringLiteral("context.cut"));
        auto* copy = menu->findChild<QAction*>(QStringLiteral("context.copy"));
        collapsedActionsDisabled = cut && copy && !cut->isEnabled() &&
                                   !copy->isEnabled() &&
                                   canvas.selectedText().isEmpty();
        menu->close();
    });
    QContextMenuEvent outsideEvent(
        QContextMenuEvent::Mouse, outside,
        canvas.viewport()->mapToGlobal(outside));
    QApplication::sendEvent(canvas.viewport(), &outsideEvent);
    check(collapsedActionsDisabled,
          "a collapsed table-cell caret enabled context Cut or Copy");

    QApplication::clipboard()->setText(QStringLiteral("do not replace"));
    const auto beforeCollapsedCommands =
        canvas.snapshot().document.tables().front();
    canvas.copy();
    canvas.cut();
    check(QApplication::clipboard()->text() ==
              QStringLiteral("do not replace") &&
              tableMatches(canvas, tableId, beforeCollapsedCommands),
          "Copy or Cut at a collapsed table-cell caret copied or cleared the cell");

    check(canvas.activateTableCell(tableId, 2, 2, 0),
          "could not activate a cell for selected-text clipboard testing");
    sendKey(canvas, Qt::Key_Right, Qt::ShiftModifier);
    check(canvas.selectedText() == QStringLiteral("i"),
          "table-cell keyboard text selection was not created");
    canvas.copy();
    check(QApplication::clipboard()->text() == QStringLiteral("i"),
          "Copy lost explicit table-cell text-selection behavior");
    canvas.cut();
    snapshot = canvas.snapshot();
    check(fromUtf16(snapshot.document.findTable(tableId)->cell(2, 2)->text) ==
              QStringLiteral("ndia") &&
              QApplication::clipboard()->text() == QStringLiteral("i"),
          "Cut did not replace only the selected table-cell text");
    canvas.undo();
    check(tableMatches(canvas, tableId, originalTable),
          "Undo did not restore a table-cell text Cut");

    check(canvas.selectTable(tableId),
          "could not create a whole-table clipboard selection");
    const QString wholeTableText = QStringLiteral(
        "alpha\tbravo\tcharlie\ndelta\techo\tfoxtrot\ngolf\thotel\tindia");
    canvas.copy();
    check(QApplication::clipboard()->text() == wholeTableText,
          "Copy lost whole-table selection behavior");
    canvas.cut();
    check(canvas.snapshot().document.tables().empty() &&
              QApplication::clipboard()->text() == wholeTableText,
          "Cut lost whole-table selection behavior");
    canvas.undo();
    check(tableMatches(canvas, tableId, originalTable),
          "Undo did not restore a whole-table Cut");
    canvas.hide();
}

void testPasteTextOnlyInTableCells(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    const auto tableId = insertAndPopulateTable(
        canvas, 2, 2, false,
        {QStringLiteral("One"), QStringLiteral("Two"),
         QStringLiteral("Three"), QStringLiteral("Four")});

    check(canvas.activateTableCell(tableId, 0, 0, 0),
          "could not activate a cell for plain-text replacement");
    for (int index = 0; index < 3; ++index) {
        sendKey(canvas, Qt::Key_Right, Qt::ShiftModifier);
    }
    const QColor requestedColor(QStringLiteral("#3157a4"));
    canvas.setForeground(requestedColor);
    QApplication::clipboard()->setText(QStringLiteral("Cell"));
    const auto beforeSelectionPaste = canvas.snapshot();
    sendKey(canvas, Qt::Key_V,
            Qt::ControlModifier | Qt::ShiftModifier);
    auto pasted = canvas.snapshot();
    const auto* table = pasted.document.findTable(tableId);
    check(pasted.revision.value() ==
              beforeSelectionPaste.revision.value() + 1 &&
              table && fromUtf16(table->cell(0, 0)->text) ==
                           QStringLiteral("Cell") &&
              table->cell(0, 0)->characterFormatAt(2).foreground_argb ==
                  static_cast<std::uint32_t>(requestedColor.rgba()),
          "plain-text paste did not replace a table-cell text selection with its current format");
    canvas.undo();
    pasted = canvas.snapshot();
    table = pasted.document.findTable(tableId);
    check(table && fromUtf16(table->cell(0, 0)->text) ==
                       QStringLiteral("One"),
          "one Undo did not restore a table-cell text selection paste");

    check(canvas.activateTableCell(tableId, 0, 1, 2),
          "could not place a table-cell caret for plain-text paste");
    QApplication::clipboard()->setText(QStringLiteral("X\nY"));
    const auto beforeCaretPaste = canvas.snapshot();
    canvas.pasteTextOnly();
    pasted = canvas.snapshot();
    table = pasted.document.findTable(tableId);
    check(pasted.revision.value() == beforeCaretPaste.revision.value() + 1 &&
              table && fromUtf16(table->cell(0, 1)->text) ==
                           QStringLiteral("TwX\u2028Yo"),
          "plain-text paste at a table-cell caret lost its line break or insertion point");
    canvas.undo();
    pasted = canvas.snapshot();
    table = pasted.document.findTable(tableId);
    check(table && fromUtf16(table->cell(0, 1)->text) ==
                       QStringLiteral("Two"),
          "one Undo did not restore a table-cell caret paste");

    check(canvas.selectTableCells(tableId, 0, 0, 0, 1),
          "could not prepare a rectangular cell paste selection");
    QApplication::clipboard()->setText(
        QStringLiteral("only\tplain\ntext"));
    const auto beforeRangePaste = canvas.snapshot();
    canvas.pasteTextOnly();
    pasted = canvas.snapshot();
    table = pasted.document.findTable(tableId);
    check(pasted.revision.value() == beforeRangePaste.revision.value() + 1 &&
              table && table->rowCount() == 2 && table->columnCount() == 2 &&
              fromUtf16(table->cell(0, 0)->text) ==
                  QStringLiteral("only\tplain\u2028text") &&
              table->cell(0, 1)->text.empty(),
          "plain-text paste interpreted tabs/newlines as a table or failed to replace the selected cell range");
    canvas.undo();
    pasted = canvas.snapshot();
    table = pasted.document.findTable(tableId);
    check(table && fromUtf16(table->cell(0, 0)->text) ==
                       QStringLiteral("One") &&
              fromUtf16(table->cell(0, 1)->text) == QStringLiteral("Two"),
          "one Undo did not restore an entire rectangular cell paste");
}

void testTableCellSpellcheckCommitTiming(
    docxstudio::app::SpellChecker& spelling) {
    check(spelling.available(),
          "Hunspell is unavailable for the table spellcheck timing regression");
    check(!spelling.isCorrect(QStringLiteral("teh")),
          "the table spellcheck timing fixture is unexpectedly accepted");
    const auto redPixels = [](DocumentCanvas& canvas) {
        return pixelsNearColor(
            renderedViewport(canvas), 0xffd92d20U, 40);
    };
    const auto type = [](DocumentCanvas& canvas, const QString& text) {
        for (const auto character : text) {
            sendTextKey(canvas, Qt::Key_unknown, QString(character));
        }
    };
    const auto prepare = [](DocumentCanvas& canvas) {
        canvas.resize(900, 700);
        canvas.show();
        canvas.setFocus();
        QApplication::processEvents();
    };

    DocumentCanvas navigation(spelling);
    prepare(navigation);
    const auto tableId = insertAndPopulateTable(
        navigation, 1, 1, false, {QStringLiteral(" good")});
    check(navigation.activateTableCell(tableId, 0, 0, 0),
          "could not position the table timing caret");
    type(navigation, QStringLiteral("tehh"));
    check(redPixels(navigation) <= 2,
          "a misspelled table-cell word was underlined mid-typing");
    sendKey(navigation, Qt::Key_Left);
    check(redPixels(navigation) <= 2,
          "moving within a pending table word exposed its squiggle");
    sendKey(navigation, Qt::Key_Delete);
    check(fromUtf16(navigation.snapshot().document.findTable(tableId)
                        ->cell(0, 0)->text) == QStringLiteral("teh good") &&
              redPixels(navigation) <= 2,
          "Delete did not recompute and retain the pending table word");
    sendKey(navigation, Qt::Key_Right);
    check(redPixels(navigation) > 2,
          "moving off a table-cell typo did not commit it");
    sendKey(navigation, Qt::Key_Left);
    check(redPixels(navigation) > 2,
          "revisiting a committed table typo hid its squiggle");
    navigation.hide();

    DocumentCanvas space(spelling);
    prepare(space);
    check(space.insertTable(1, 1, false),
          "could not insert the table space timing fixture");
    type(space, QStringLiteral("teh"));
    check(redPixels(space) <= 2,
          "a table typo was decorated before the terminating space");
    sendTextKey(space, Qt::Key_Space, QStringLiteral(" "));
    check(redPixels(space) > 2,
          "Space did not commit a table-cell misspelling");
    space.hide();

    DocumentCanvas newline(spelling);
    prepare(newline);
    check(newline.insertTable(1, 1, false),
          "could not insert the table newline timing fixture");
    type(newline, QStringLiteral("teh"));
    sendKey(newline, Qt::Key_Return);
    check(redPixels(newline) > 2,
          "a table-cell line break did not commit a misspelling");
    newline.hide();

    DocumentCanvas appendedRow(spelling);
    prepare(appendedRow);
    check(appendedRow.insertTable(1, 1, false),
          "could not insert the final-cell Tab timing fixture");
    type(appendedRow, QStringLiteral("teh"));
    sendKey(appendedRow, Qt::Key_Tab);
    const auto appendedSnapshot = appendedRow.snapshot();
    check(appendedSnapshot.document.tables().front().rowCount() == 2 &&
              redPixels(appendedRow) > 2,
          "Tab from the final cell appended a row without committing its typo");
    appendedRow.hide();

    DocumentCanvas pasted(spelling);
    prepare(pasted);
    check(pasted.insertTable(1, 1, false),
          "could not insert the table paste timing fixture");
    QApplication::clipboard()->setText(QStringLiteral("teh"));
    pasted.paste();
    check(redPixels(pasted) > 2,
          "pasted table-cell text was mistaken for live typing");
    pasted.hide();

    DocumentCanvas ime(spelling);
    prepare(ime);
    check(ime.insertTable(1, 1, false),
          "could not insert the table IME timing fixture");
    commitInputMethodText(ime, QStringLiteral("teh"));
    check(redPixels(ime) <= 2,
          "an IME-committed table word was underlined mid-entry");
    commitInputMethodText(ime, QStringLiteral(" "));
    check(redPixels(ime) > 2,
          "an IME space did not commit the table-cell misspelling");
    ime.hide();

    DocumentCanvas focus(spelling);
    prepare(focus);
    check(focus.insertTable(1, 1, false),
          "could not insert the table focus timing fixture");
    type(focus, QStringLiteral("teh"));
    focus.clearFocus();
    QApplication::processEvents();
    check(redPixels(focus) > 2,
          "leaving a table cell did not commit its pending misspelling");
    focus.hide();
}

void testTableCellSpellingContextReplacement(
    docxstudio::app::SpellChecker& spelling) {
    check(spelling.available(),
          "Hunspell is unavailable for the table spelling regression");
    const QString misspelling = QStringLiteral("teh");
    check(!spelling.isCorrect(misspelling),
          "the table spelling fixture is unexpectedly accepted");
    const auto suggestions = spelling.suggestions(misspelling);
    check(!suggestions.isEmpty(),
          "Hunspell returned no correction for the table spelling fixture");
    const QString replacement = suggestions.front();

    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.show();
    const auto tableId = insertAndPopulateTable(
        canvas, 1, 1, false,
        {QStringLiteral("teh remains here")});
    check(canvas.activateTableCell(tableId, 0, 0, 1),
          "could not position the table spelling context caret");
    QApplication::processEvents();
    const QPoint wordPoint = inputMethodCursorRect(canvas).center();
    const QImage misspelledRendering = renderedViewport(canvas);
    const int misspellingPixels = pixelsNearColor(
        misspelledRendering, 0xffd92d20U, 40);
    check(misspellingPixels > 2,
          "a misspelled table-cell word had no local spelling decoration");

    bool suggestionTriggered = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu != nullptr,
              "right-clicking a misspelled table word did not open a menu");
        QAction* correction = nullptr;
        for (auto* action : menu->actions()) {
            if (action->objectName() ==
                    QStringLiteral("context.spellingSuggestion") &&
                action->text() == replacement) {
                correction = action;
                break;
            }
        }
        check(correction != nullptr,
              "table-cell context menu omitted Hunspell suggestions");
        suggestionTriggered = true;
        correction->trigger();
        menu->close();
    });
    QContextMenuEvent spellingEvent(
        QContextMenuEvent::Mouse, wordPoint,
        canvas.viewport()->mapToGlobal(wordPoint));
    QApplication::sendEvent(canvas.viewport(), &spellingEvent);
    const auto snapshot = canvas.snapshot();
    const auto* table = snapshot.document.findTable(tableId);
    check(suggestionTriggered && table &&
              fromUtf16(table->cell(0, 0)->text) ==
                  replacement + QStringLiteral(" remains here"),
          "table spelling correction replaced more than the clicked word");
    const QImage correctedRendering = renderedViewport(canvas);
    check(pixelsNearColor(correctedRendering, 0xffd92d20U, 40) + 2 <
              misspellingPixels,
          "table-cell spelling decoration remained after correction");
    canvas.hide();
}

void testSingleDialogAndNativeSave() {
    docxstudio::app::MainWindow window;
    window.resize(1000, 760);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<DocumentCanvas*>();
    auto* insertAction = window.findChild<QAction*>(
        QStringLiteral("insert.table"));
    auto* headerFooterAction = window.findChild<QAction*>(
        QStringLiteral("insert.headerFooter"));
    auto* pasteTextOnlyAction = window.findChild<QAction*>(
        QStringLiteral("edit.pasteTextOnly"));
    check(canvas && insertAction && headerFooterAction && pasteTextOnlyAction &&
              pasteTextOnlyAction->text() ==
                  QStringLiteral("Paste as Text Only") &&
              pasteTextOnlyAction->shortcut() ==
                  QKeySequence(QStringLiteral("Ctrl+Shift+V")),
          "could not reach the Paste as Text Only command and shortcut");

    headerFooterAction->trigger();
    auto* headerLeft = canvas->findChild<QPlainTextEdit*>(
        QStringLiteral("headerStoryEditor0"));
    auto* headerCenter = canvas->findChild<QPlainTextEdit*>(
        QStringLiteral("headerStoryEditor1"));
    auto* headerRight = canvas->findChild<QPlainTextEdit*>(
        QStringLiteral("headerStoryEditor2"));
    check(canvas->isHeaderFooterEditing() && headerLeft && headerCenter &&
              headerRight && headerLeft->isVisible() &&
              headerCenter->isVisible() && headerRight->isVisible(),
          "Header & Footer did not expose three on-page editing regions");
    headerLeft->setPlainText(QStringLiteral("Owl Docs"));
    headerCenter->setPlainText(QStringLiteral("Confidential"));
    canvas->undo();
    check(canvas->headerText().isEmpty() && headerLeft->toPlainText().isEmpty() &&
              headerCenter->toPlainText().isEmpty(),
          "one Undo did not remove and refresh the on-page header edit");
    canvas->redo();
    check(canvas->headerText() ==
              QStringLiteral("Owl Docs\tConfidential\t") &&
              headerLeft->toPlainText() == QStringLiteral("Owl Docs") &&
              headerCenter->toPlainText() == QStringLiteral("Confidential"),
          "Redo did not restore and refresh the on-page header edit");
    canvas->beginHeaderFooterEditing(true, 2, 0);
    auto* footerRight = canvas->findChild<QPlainTextEdit*>(
        QStringLiteral("footerStoryEditor2"));
    check(footerRight && footerRight->isVisible(),
          "footer right region was not directly editable");
    footerRight->setPlainText(QStringLiteral("Page {PAGE} of {PAGES}"));
    canvas->endHeaderFooterEditing();
    check(canvas->headerText() ==
              QStringLiteral("Owl Docs\tConfidential\t") &&
              canvas->footerText() ==
                  QStringLiteral("\t\tPage {PAGE} of {PAGES}"),
          "on-page header/footer regions did not apply to the document");
    const QPoint headerCenterPoint(canvas->viewport()->width() / 2, 58);
    sendMouseEvent(*canvas, QEvent::MouseButtonPress, headerCenterPoint,
                   Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(*canvas, QEvent::MouseButtonRelease, headerCenterPoint,
                   Qt::LeftButton, Qt::NoButton);
    check(canvas->isHeaderFooterEditing() && headerCenter->hasFocus(),
          "clicking the center header region did not activate it in place");
    canvas->endHeaderFooterEditing();

    bool inspectedDialog = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("insertTableDialog"));
        check(dialog && dialog->isModal(),
              "Insert Table did not open one modal dialog");
        auto* rows = dialog->findChild<QSpinBox*>(
            QStringLiteral("insertTable.rows"));
        auto* columns = dialog->findChild<QSpinBox*>(
            QStringLiteral("insertTable.columns"));
        auto* header = dialog->findChild<QCheckBox*>(
            QStringLiteral("insertTable.header"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("insertTable.buttons"));
        check(rows && columns && header && buttons &&
                  rows->minimum() == 1 && rows->maximum() == 50 &&
                  columns->minimum() == 1 && columns->maximum() == 20,
              "Insert Table dialog does not contain bounded rows, columns, and header controls");
        rows->setValue(3);
        columns->setValue(2);
        header->setChecked(true);
        inspectedDialog = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    insertAction->trigger();
    check(inspectedDialog, "Insert Table dialog was not inspected");
    auto snapshot = canvas->snapshot();
    check(snapshot.document.tables().size() == 1 &&
              snapshot.document.tables().front().rowCount() == 3 &&
              snapshot.document.tables().front().columnCount() == 2 &&
              snapshot.document.tables().front().hasHeaderRow(),
          "single Insert Table dialog did not create the requested table");
    canvas->insertText(QStringLiteral("Saved cell"));

    QTemporaryDir output;
    check(output.isValid(), "could not create DOCX test directory");
    const QString path = output.filePath(QStringLiteral("semantic-table.docx"));
    auto* saveAs = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr, "could not reach Save As command");
    bool selectedDestination = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(
                  dialog, "accept", Qt::DirectConnection),
              "could not accept Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path),
          "new table document was not saved");
    const auto xml = zipMember(path, "word/document.xml");
    check(xml.contains("<w:tbl>") && xml.contains("<w:tblHeader/>") &&
              xml.contains("Saved cell"),
          "new document save did not emit a native WordprocessingML table");
    check(!xml.contains("Saved cell\t"),
          "new document save flattened the semantic table to tabbed text");
    check(zipMember(path, "word/header1.xml").contains("Owl Docs") &&
              zipMember(path, "word/footer1.xml").contains("NUMPAGES"),
          "new document save omitted native header/footer parts");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    docxstudio::app::SpellChecker spelling;
    testCanvasTableEditing(spelling);
    testActiveCellAndRectangularSelectionPainting(spelling);
    testRectangularCellFormattingAndUndo(spelling);
    testEmptyCellFormattingSurvivesNavigation(spelling);
    testTableRowAndColumnCommandsAndUndo(spelling);
    testTableStylePalettesRenderAndUndo(spelling);
    testImportedStylePrecedenceAndStableCellPresentation(spelling);
    testImportedTableUsesUnroundedAutomaticLineAdvance(spelling);
    testMouseHandleMovesTable(spelling);
    testInsertionIsOneUndoTransaction(spelling);
    testSelectedTableCommandsDoNotMutateStaleBody(spelling);
    testTabAtLastCellAppendsOneUndoableRow(spelling);
    testDoubleClickSelectsTableCellWord(spelling);
    testTableContextSelectionAndClipboard(spelling);
    testPasteTextOnlyInTableCells(spelling);
    testTableCellSpellcheckCommitTiming(spelling);
    testTableCellSpellingContextReplacement(spelling);
    testSingleDialogAndNativeSave();
    std::cout << "table UI tests passed\n";
    return 0;
}
