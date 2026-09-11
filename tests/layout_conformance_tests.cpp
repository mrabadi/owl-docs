#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QString>
#include <QTextBoundaryFinder>

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

void testRtlVisualCaretAndSelection(
    docxstudio::app::SpellChecker& spelling) {
    const QString text = QString::fromUtf8("אבגד");
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
}

void testCjkWrapAndSelection(docxstudio::app::SpellChecker& spelling) {
    const QString unit = QString::fromUtf8("日本語漢字仮名交じり文");
    const QString text = unit.repeated(8);
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

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("LayoutConformanceTests"));

    docxstudio::app::SpellChecker spelling;
    testRtlVisualCaretAndSelection(spelling);
    testCjkWrapAndSelection(spelling);
    testIndicClustersAndWrap(spelling);
    std::cout << "Layout conformance tests passed\n";
    return EXIT_SUCCESS;
}
