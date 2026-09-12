#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QString>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using docxstudio::app::DocumentCanvas;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

docxstudio::core::Document documentWithText(const QString& text) {
    auto paragraph = docxstudio::core::Paragraph::create(
        text.toStdU16String());
    check(static_cast<bool>(paragraph),
          "could not create multilingual paragraph");
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{
            std::move(paragraph.value())});
    check(static_cast<bool>(document),
          "could not create multilingual document");
    return std::move(document.value());
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

QRect cursorRect(DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRect();
}

int inputMethodCursorPosition(DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorPosition);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorPosition).toInt();
}

qsizetype nextGraphemeBoundary(const QString& text, qsizetype offset) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(offset);
    return finder.toNextBoundary();
}

bool isGraphemeBoundary(const QString& text, qsizetype offset) {
    if (offset == 0 || offset == text.size()) return true;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(offset);
    return finder.isAtBoundary();
}

int selectionBluePixels(DocumentCanvas& canvas) {
    QApplication::processEvents();
    const QImage image = canvas.viewport()->grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.blue() - pixel.red() > 30 &&
                pixel.blue() - pixel.green() > 10 &&
                pixel.blue() > 180) {
                ++count;
            }
        }
    }
    return count;
}

void prepare(DocumentCanvas& canvas) {
    canvas.resize(620, 360);
    canvas.show();
    canvas.setFocus();
    QApplication::processEvents();
}

void checkShapedGlyphCoverage(const QString& text, const char* message) {
    QTextLayout layout(text, QFont(QStringLiteral("DejaVu Sans"), 16));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(180.0);
    }
    layout.endLayout();

    const auto runs = layout.glyphRuns();
    check(!runs.empty(), message);
    std::size_t glyphCount = 0;
    for (const auto& run : runs) {
        check(run.rawFont().isValid(), message);
        for (const auto glyph : run.glyphIndexes()) {
            check(glyph != 0U, message);
            ++glyphCount;
        }
    }
    check(glyphCount > 0, message);
}

void testEnglishShapingCaretSelectionAndWrap(
    docxstudio::app::SpellChecker& spelling) {
    const QString text = QStringLiteral(
        "Owl Docs keeps ordinary English editing responsive and predictable. ")
                             .repeated(5);
    checkShapedGlyphCoverage(
        text, "English shaping produced a missing or invalid glyph run");

    DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    canvas.setPageSizePoints(190.0, 500.0);
    canvas.setMarginsPoints(24.0, 24.0, 24.0, 24.0);
    canvas.setDocument(documentWithText(text));
    prepare(canvas);

    sendKey(canvas, Qt::Key_Right, Qt::ShiftModifier);
    check(canvas.selection().focus.utf16_offset == 1 &&
              canvas.selectedText() == QStringLiteral("O"),
          "English selection did not advance by one grapheme");
    sendKey(canvas, Qt::Key_Home);
    const QRect firstLine = cursorRect(canvas);
    sendKey(canvas, Qt::Key_Down);
    const auto wrappedOffset = static_cast<qsizetype>(
        canvas.selection().focus.utf16_offset);
    check(wrappedOffset > 0 && wrappedOffset < text.size() &&
              isGraphemeBoundary(text, wrappedOffset) &&
              cursorRect(canvas).y() > firstLine.y(),
          "English line wrapping or vertical caret navigation failed");
    canvas.hide();
}

void testRtlVisualCaretAndSelection(
    docxstudio::app::SpellChecker& spelling) {
    const QString text = QString::fromUtf8("אבגד");
    checkShapedGlyphCoverage(
        text, "RTL shaping produced a missing or invalid glyph run");
    DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    canvas.setDocument(documentWithText(text));
    prepare(canvas);

    const auto paragraphId =
        canvas.snapshot().document.paragraphs().front().id();
    const QRect logicalStart = cursorRect(canvas);
    check(logicalStart.isValid(), "RTL logical-start caret is not visible");

    const qsizetype firstBoundary = nextGraphemeBoundary(text, 0);
    check(firstBoundary > 0, "RTL fixture has no first grapheme boundary");
    sendKey(canvas, Qt::Key_Left);
    check(canvas.selection().focus.paragraph_id == paragraphId &&
              canvas.selection().focus.utf16_offset ==
                  static_cast<std::size_t>(firstBoundary),
          "Left did not move visually left through RTL text");
    const QRect firstCaret = cursorRect(canvas);
    check(firstCaret.x() < logicalStart.x(),
          "RTL caret moved right after the Left key");

    const qsizetype secondBoundary =
        nextGraphemeBoundary(text, firstBoundary);
    check(secondBoundary > firstBoundary,
          "RTL fixture has no second grapheme boundary");
    sendKey(canvas, Qt::Key_Left, Qt::ShiftModifier);
    check(canvas.selection().focus.utf16_offset ==
              static_cast<std::size_t>(secondBoundary) &&
              canvas.selectedText() ==
                  text.mid(firstBoundary, secondBoundary - firstBoundary),
          "Shift+Left split or selected the wrong RTL grapheme");
    check(selectionBluePixels(canvas) > 12,
          "RTL selection produced no visible selection decoration");

    sendKey(canvas, Qt::Key_Right);
    check(canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.utf16_offset ==
                  static_cast<std::size_t>(firstBoundary),
          "Right did not collapse an RTL selection to its visual-right edge");
    sendKey(canvas, Qt::Key_Right);
    check(canvas.selection().focus.utf16_offset == 0 &&
              cursorRect(canvas).x() > firstCaret.x(),
          "Right did not return the RTL caret to logical start");
    canvas.hide();

    const QString rtlCluster = QString::fromUtf8("שָׁ");
    const QString wrappedText = rtlCluster.repeated(36);
    DocumentCanvas wrapped(spelling);
    wrapped.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    wrapped.setPageSizePoints(170.0, 500.0);
    wrapped.setMarginsPoints(24.0, 24.0, 24.0, 24.0);
    wrapped.setDocument(documentWithText(wrappedText));
    prepare(wrapped);
    sendKey(wrapped, Qt::Key_Home);
    const QRect firstLine = cursorRect(wrapped);
    sendKey(wrapped, Qt::Key_Down);
    const auto wrappedOffset = static_cast<qsizetype>(
        wrapped.selection().focus.utf16_offset);
    check(wrappedOffset > 0 && wrappedOffset < wrappedText.size(),
          "RTL text did not wrap onto a second visual line");
    check(isGraphemeBoundary(wrappedText, wrappedOffset),
          "RTL line breaking placed the caret inside a marked grapheme");
    check(cursorRect(wrapped).y() > firstLine.y(),
          "RTL Down navigation did not reach the next visual line");
    wrapped.hide();

    DocumentCanvas tableCell(spelling);
    tableCell.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    check(tableCell.insertTable(1, 1, false),
          "could not create RTL table-cell fixture");
    tableCell.insertText(text);
    const auto tableId = tableCell.snapshot().document.tables().front().id();
    check(tableCell.activateTableCell(tableId, 0, 0, 0),
          "could not position the RTL table-cell caret");
    prepare(tableCell);
    const QRect tableLogicalStart = cursorRect(tableCell);
    sendKey(tableCell, Qt::Key_Left);
    check(inputMethodCursorPosition(tableCell) == firstBoundary &&
              cursorRect(tableCell).x() < tableLogicalStart.x(),
          "table-cell Left did not follow RTL visual order");
    sendKey(tableCell, Qt::Key_Left, Qt::ShiftModifier);
    check(inputMethodCursorPosition(tableCell) == secondBoundary &&
              tableCell.selectedText() ==
                  text.mid(firstBoundary, secondBoundary - firstBoundary),
          "RTL table-cell selection split or selected the wrong grapheme");
    sendKey(tableCell, Qt::Key_Right);
    check(inputMethodCursorPosition(tableCell) == firstBoundary,
          "Right did not collapse an RTL table-cell selection visually");
    tableCell.hide();
}

void testCjkWrapAndSelection(docxstudio::app::SpellChecker& spelling) {
    const QString unit = QString::fromUtf8("日本語漢字仮名交じり文");
    const QString text = unit.repeated(8);
    checkShapedGlyphCoverage(
        text, "CJK shaping produced a missing or invalid glyph run");
    DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 14.0, 4);
    canvas.setPageSizePoints(170.0, 500.0);
    canvas.setMarginsPoints(24.0, 24.0, 24.0, 24.0);
    canvas.setDocument(documentWithText(text));
    prepare(canvas);

    sendKey(canvas, Qt::Key_Home);
    const QRect firstLineCaret = cursorRect(canvas);
    sendKey(canvas, Qt::Key_Down);
    const auto wrappedOffset = static_cast<qsizetype>(
        canvas.selection().focus.utf16_offset);
    check(wrappedOffset > 0 && wrappedOffset < text.size(),
          "CJK text without spaces did not wrap onto a second visual line");
    check(isGraphemeBoundary(text, wrappedOffset),
          "CJK line breaking placed the caret inside a grapheme");
    check(cursorRect(canvas).y() > firstLineCaret.y(),
          "CJK Down navigation did not reach the next visual line");

    const qsizetype next = nextGraphemeBoundary(text, wrappedOffset);
    check(next > wrappedOffset, "CJK fixture ended at its wrapped caret");
    sendKey(canvas, Qt::Key_Right, Qt::ShiftModifier);
    check(canvas.selection().focus.utf16_offset ==
              static_cast<std::size_t>(next) &&
              canvas.selectedText() == text.mid(wrappedOffset,
                                                 next - wrappedOffset),
          "CJK selection did not follow one complete grapheme");
    canvas.hide();
}

void testIndicClustersAndWrap(docxstudio::app::SpellChecker& spelling) {
    const QString cluster = QString::fromUtf8("क्षि");
    const QString text = cluster.repeated(28);
    checkShapedGlyphCoverage(
        text, "Indic shaping produced a missing or invalid glyph run");
    const qsizetype firstBoundary = nextGraphemeBoundary(text, 0);
    check(firstBoundary > 1,
          "Indic fixture was not recognized as a multi-code-unit grapheme");

    DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    canvas.setPageSizePoints(170.0, 500.0);
    canvas.setMarginsPoints(24.0, 24.0, 24.0, 24.0);
    canvas.setDocument(documentWithText(text));
    prepare(canvas);

    sendKey(canvas, Qt::Key_Right);
    check(canvas.selection().focus.utf16_offset ==
              static_cast<std::size_t>(firstBoundary),
          "Right split an Indic shaping cluster");
    sendKey(canvas, Qt::Key_Left, Qt::ShiftModifier);
    check(canvas.selection().focus.utf16_offset == 0 &&
              canvas.selectedText() == text.left(firstBoundary),
          "Indic reverse selection split a shaping cluster");

    sendKey(canvas, Qt::Key_Home);
    sendKey(canvas, Qt::Key_Down);
    const auto wrappedOffset = static_cast<qsizetype>(
        canvas.selection().focus.utf16_offset);
    check(wrappedOffset > 0 && wrappedOffset < text.size(),
          "Indic text did not wrap onto a second visual line");
    check(isGraphemeBoundary(text, wrappedOffset),
          "Indic line breaking placed the caret inside a shaping cluster");
    canvas.hide();
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

void testViewOnlyInlineImagesKeepFragmentOrder(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    canvas.setPageSizePoints(500.0, 300.0);
    canvas.setMarginsPoints(40.0, 40.0, 40.0, 40.0);
    canvas.setDocument(documentWithText(QStringLiteral("ABCD")));
    const auto paragraphId =
        canvas.snapshot().document.paragraphs().front().id();

    QImage red(8, 8, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    QImage blue(8, 8, QImage::Format_ARGB32_Premultiplied);
    blue.fill(QColor(0, 0, 255));
    canvas.setImportedPresentation(
        {{paragraphId, 1, red, 36.0, 24.0, QStringLiteral("red")},
         {paragraphId, 1, blue, 28.0, 24.0, QStringLiteral("blue")}},
        {});
    canvas.resize(800, 430);
    canvas.show();
    canvas.setFocus();
    QApplication::processEvents();

    const QRect before = cursorRect(canvas);
    sendKey(canvas, Qt::Key_Right);
    const QRect afterImages = cursorRect(canvas);
    check(canvas.selection().focus.utf16_offset == 1,
          "inline image navigation changed the editor text offset");

    const QImage painted = canvas.viewport()->grab().toImage();
    const QRect redBounds = saturatedColorBounds(painted, true);
    const QRect blueBounds = saturatedColorBounds(painted, false);
    check(redBounds.isValid() && blueBounds.isValid(),
          "inline image test colors were not painted");
    check(redBounds.right() < blueBounds.left(),
          "multiple inline images lost their source fragment order");
    check(redBounds.top() < blueBounds.bottom() &&
              blueBounds.top() < redBounds.bottom(),
          "inline images were stacked after the paragraph instead of sharing its line");
    check(before.x() < redBounds.left() &&
              afterImages.x() >= blueBounds.right() - 2,
          "caret did not skip the view-only inline objects at their text boundary");

    sendKey(canvas, Qt::Key_Right, Qt::ShiftModifier);
    check(canvas.selectedText() == QStringLiteral("B") &&
              canvas.selection().focus.utf16_offset == 2,
          "selection offsets were corrupted by display-only image slots");
    sendKey(canvas, Qt::Key_Left);
    check(canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.utf16_offset == 1,
          "selection did not collapse across an inline image boundary");
    canvas.hide();
}

QRect paintedRedBounds(DocumentCanvas& canvas) {
    QApplication::processEvents();
    return saturatedColorBounds(canvas.viewport()->grab().toImage(), true);
}

void collapseFindToRight(DocumentCanvas& canvas, const QString& text) {
    sendKey(canvas, Qt::Key_Home, Qt::ControlModifier);
    check(canvas.findNext(text, true),
          "could not locate anchor-tracking test text");
    sendKey(canvas, Qt::Key_Right);
}

void testInlineImageAnchorsTrackEditsAndStructure(
    docxstudio::app::SpellChecker& spelling) {
    QImage red(8, 8, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));

    DocumentCanvas insertion(spelling);
    insertion.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    insertion.setPageSizePoints(500.0, 300.0);
    insertion.setMarginsPoints(40.0, 40.0, 40.0, 40.0);
    insertion.setDocument(documentWithText(QStringLiteral("ABCD")));
    const auto insertionParagraph =
        insertion.snapshot().document.paragraphs().front().id();
    insertion.setImportedPresentation(
        {{insertionParagraph, 2, red, 36.0, 24.0,
          QStringLiteral("tracked")}},
        {});
    insertion.resize(800, 430);
    insertion.show();
    insertion.setFocus();
    QApplication::processEvents();

    insertion.insertText(QStringLiteral("iiiiiiii"));
    collapseFindToRight(insertion, QStringLiteral("ii"));
    const QRect shiftedImage = paintedRedBounds(insertion);
    check(shiftedImage.isValid() &&
              cursorRect(insertion).x() < shiftedImage.left(),
          "an insertion before an imported image left its anchor stale");
    collapseFindToRight(insertion, QStringLiteral("iiiiiiiiAB"));
    check(cursorRect(insertion).x() >= shiftedImage.right() - 2,
          "the shifted image was not retained at its original text boundary");

    insertion.undo();
    collapseFindToRight(insertion, QStringLiteral("AB"));
    const QRect undoneImage = paintedRedBounds(insertion);
    check(undoneImage.isValid() &&
              cursorRect(insertion).x() >= undoneImage.right() - 2,
          "undo did not restore the imported image anchor");
    insertion.redo();
    collapseFindToRight(insertion, QStringLiteral("ii"));
    check(cursorRect(insertion).x() < paintedRedBounds(insertion).left(),
          "redo did not restore the shifted imported image anchor");
    insertion.hide();

    DocumentCanvas structure(spelling);
    structure.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    structure.setPageSizePoints(500.0, 300.0);
    structure.setMarginsPoints(40.0, 40.0, 40.0, 40.0);
    structure.setDocument(documentWithText(QStringLiteral("ABCD")));
    const auto structureParagraph =
        structure.snapshot().document.paragraphs().front().id();
    structure.setImportedPresentation(
        {{structureParagraph, 2, red, 36.0, 24.0,
          QStringLiteral("structural")}},
        {});
    structure.resize(800, 430);
    structure.show();
    structure.setFocus();
    QApplication::processEvents();
    const QRect oneLineImage = paintedRedBounds(structure);

    collapseFindToRight(structure, QStringLiteral("A"));
    sendKey(structure, Qt::Key_Return);
    sendKey(structure, Qt::Key_Right);
    const QRect splitImage = paintedRedBounds(structure);
    check(structure.snapshot().document.paragraphs().size() == 2 &&
              splitImage.isValid() &&
              splitImage.top() > oneLineImage.top() + 6 &&
              cursorRect(structure).x() >= splitImage.right() - 2,
          "splitting before an imported image did not move its anchor to the new paragraph");

    sendKey(structure, Qt::Key_Home);
    sendKey(structure, Qt::Key_Backspace);
    const QRect mergedImage = paintedRedBounds(structure);
    check(structure.snapshot().document.paragraphs().size() == 1 &&
              mergedImage.top() < splitImage.top() - 6,
          "merging paragraphs did not rebase the imported image anchor");
    structure.undo();
    check(structure.snapshot().document.paragraphs().size() == 2 &&
              paintedRedBounds(structure).top() > mergedImage.top() + 6,
          "undo of paragraph merge did not restore the image paragraph anchor");
    structure.redo();
    check(structure.snapshot().document.paragraphs().size() == 1 &&
              paintedRedBounds(structure).top() < splitImage.top() - 6,
          "redo of paragraph merge did not restore the rebased image anchor");
    structure.hide();

    DocumentCanvas preview(spelling);
    preview.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    preview.setPageSizePoints(500.0, 300.0);
    preview.setMarginsPoints(40.0, 40.0, 40.0, 40.0);
    preview.setDocument(documentWithText(QStringLiteral("ABCD")));
    const auto previewParagraph =
        preview.snapshot().document.paragraphs().front().id();
    preview.setImportedPresentation(
        {{previewParagraph, 2, red, 36.0, 24.0,
          QStringLiteral("preview-tracked")}},
        {});
    preview.resize(800, 430);
    preview.show();
    preview.setFocus();
    QApplication::processEvents();
    const QRect previewBaseline = paintedRedBounds(preview);
    const auto previewRevision = preview.snapshot().revision;
    const std::vector<docxstudio::core::Operation> previewOperations{
        docxstudio::core::InsertText{
            {previewParagraph, 0}, u"iiiiiiii", std::nullopt}};
    QString summary;
    QString error;
    check(preview.createOperationsPreview(
              previewRevision, previewOperations,
              QStringLiteral("Move imported picture anchor"), summary, error),
          "could not create an image-anchor preview");
    const QRect previewedImage = paintedRedBounds(preview);
    check(preview.hasPreview() && previewedImage.isValid() &&
              previewedImage.left() > previewBaseline.left() + 10 &&
              preview.snapshot().document.paragraphs().front().text() ==
                  u"ABCD",
          "preview did not transform the image anchor without changing live text");
    preview.discardPreview();
    const QRect discardedImage = paintedRedBounds(preview);
    check(!preview.hasPreview() && discardedImage.isValid() &&
              std::abs(discardedImage.left() - previewBaseline.left()) <= 1 &&
              preview.snapshot().document.paragraphs().front().text() ==
                  u"ABCD",
          "discarding a preview did not restore the live image anchor");

    summary.clear();
    error.clear();
    check(preview.createOperationsPreview(
              preview.snapshot().revision, previewOperations,
              QStringLiteral("Accept imported picture anchor"), summary,
              error) &&
              preview.acceptPreview(error),
          "could not accept an image-anchor preview");
    const QRect acceptedImage = paintedRedBounds(preview);
    check(acceptedImage.isValid() &&
              acceptedImage.left() > previewBaseline.left() + 10 &&
              preview.snapshot().document.paragraphs().front().text() ==
                  u"iiiiiiiiABCD",
          "accepting a preview did not commit its transformed image anchor");
    preview.undo();
    check(preview.snapshot().document.paragraphs().front().text() == u"ABCD" &&
              std::abs(paintedRedBounds(preview).left() -
                       previewBaseline.left()) <= 1,
          "undo of an accepted preview did not restore its image anchor");
    preview.hide();

    DocumentCanvas replacement(spelling);
    replacement.setEditorDefaults(QStringLiteral("DejaVu Sans"), 16.0, 4);
    replacement.setPageSizePoints(500.0, 300.0);
    replacement.setMarginsPoints(40.0, 40.0, 40.0, 40.0);
    replacement.setDocument(documentWithText(QStringLiteral("ABCD")));
    const auto replacementParagraph =
        replacement.snapshot().document.paragraphs().front().id();
    replacement.setImportedPresentation(
        {{replacementParagraph, 2, red, 36.0, 24.0,
          QStringLiteral("replacement-tracked")}},
        {});
    replacement.resize(800, 430);
    replacement.show();
    replacement.setFocus();
    QApplication::processEvents();
    const QRect replacementBaseline = paintedRedBounds(replacement);
    const std::vector<docxstudio::core::Operation> replacementOperations{
        docxstudio::core::ReplaceRange{
            {{replacementParagraph, 1}, {replacementParagraph, 3}},
            u"XYZ", std::nullopt}};
    summary.clear();
    error.clear();
    check(replacement.createOperationsPreview(
              replacement.snapshot().revision, replacementOperations,
              QStringLiteral("Replace across imported picture"), summary,
              error) &&
              replacement.acceptPreview(error),
          "could not replace text spanning an imported image anchor");
    collapseFindToRight(replacement, QStringLiteral("A"));
    const QRect replacementImage = paintedRedBounds(replacement);
    check(replacement.snapshot().document.paragraphs().front().text() ==
                  u"AXYZD" &&
              replacementImage.isValid() &&
              cursorRect(replacement).x() >= replacementImage.right() - 2,
          "replacement did not preserve the image at the deletion boundary");
    replacement.undo();
    check(replacement.snapshot().document.paragraphs().front().text() ==
                  u"ABCD" &&
              std::abs(paintedRedBounds(replacement).left() -
                       replacementBaseline.left()) <= 1,
          "undo of image-spanning replacement did not restore its anchor");
    replacement.hide();
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("LayoutConformanceTests"));

    docxstudio::app::SpellChecker spelling;
    testEnglishShapingCaretSelectionAndWrap(spelling);
    testRtlVisualCaretAndSelection(spelling);
    testCjkWrapAndSelection(spelling);
    testIndicClustersAndWrap(spelling);
    testViewOnlyInlineImagesKeepFragmentOrder(spelling);
    testInlineImageAnchorsTrackEditsAndStructure(spelling);
    std::cout << "Layout conformance tests passed\n";
    return EXIT_SUCCESS;
}
