#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
int darkPixels(QWidget* widget) {
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    widget->render(&painter);
    painter.end();
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).lightness() < 90) ++count;
    return count;
}
QImage rendering(QWidget* widget) {
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
}
QRect caretInk(QPlainTextEdit* editor) {
    editor->setFocus();
    const auto focused = rendering(editor->viewport());
    editor->clearFocus();
    const auto unfocused = rendering(editor->viewport());
    QRect bounds;
    for (int y = 0; y < focused.height(); ++y)
        for (int x = 0; x < focused.width(); ++x)
            if (focused.pixelColor(x, y).lightness() < 90 && unfocused.pixelColor(x, y).lightness() >= 90)
                bounds = bounds.united(QRect(x, y, 1, 1));
    editor->setFocus();
    return bounds;
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.resize(1100, 900);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 12, 4);
    check(canvas.setHeaderText(QStringLiteral("Page {PAGE} ")), "IME source fixture");
    canvas.show();
    canvas.beginHeaderFooterEditing(false, 0, 0);
    QApplication::processEvents();
    auto* editor = canvas.findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
    check(editor && editor->isVisible(), "IME editor unavailable");
    editor->moveCursor(QTextCursor::End);
    editor->setFocus();
    const auto before = canvas.snapshot();
    const int blank = darkPixels(editor->viewport());
    QInputMethodEvent composing(QStringLiteral("Compose"), {});
    QApplication::sendEvent(editor, &composing);
    QApplication::processEvents();
    check(canvas.snapshot().document == before.document && canvas.snapshot().revision == before.revision,
          "uncommitted input method composition mutated the document");
    check(darkPixels(editor->viewport()) > blank + 20,
          "input method preedit is invisible in the inline header/footer editor");
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("Compose"));
    QApplication::sendEvent(editor, &commit);
    check(canvas.headerText().startsWith(QStringLiteral("Page {PAGE} Compose\t")),
          "input method commit did not update the visible story");
    canvas.undo();
    check(canvas.snapshot().document == before.document, "one Undo did not restore the pre-composition story");
    editor->moveCursor(QTextCursor::End);
    const auto caret = caretInk(editor);
    QInputMethodQueryEvent query(Qt::ImCursorRectangle);
    QApplication::sendEvent(editor, &query);
    const auto candidate = query.value(Qt::ImCursorRectangle).toRectF();
    check(!caret.isEmpty() && std::abs(candidate.left() - caret.left()) <= 2 &&
              std::abs(candidate.top() - caret.top()) <= 2,
          "input method candidate geometry follows raw PAGE tokens instead of the rendered caret");
    canvas.endHeaderFooterEditing();
    const QString fields = QStringLiteral("{PAGE} {PAGE} {PAGE} {PAGE} {PAGE} {PAGE}");
    check(canvas.setHeaderText(fields + QStringLiteral("\t\t")), "visual field-navigation fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(editor, &home);
    check(editor->textCursor().position() == 0,
          "Home follows wrapped raw PAGE tokens instead of the single rendered line");
    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QApplication::sendEvent(editor, &end);
    check(editor->textCursor().position() == fields.size(),
          "End stops inside raw PAGE source instead of the rendered line end");
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(editor, &up);
    check(editor->textCursor().position() == 0, "Up follows phantom lines created by hidden PAGE source");
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(editor, &down);
    check(editor->textCursor().position() == fields.size(), "Down follows phantom lines created by hidden PAGE source");
    auto partial = editor->textCursor();
    partial.setPosition(2); partial.setPosition(4, QTextCursor::KeepAnchor);
    editor->setTextCursor(partial);
    check(editor->textCursor().selectionStart() == 0 && editor->textCursor().selectionEnd() == 6,
          "partial selection can split the hidden source of a native page field");
    const auto beforeRemoval = canvas.snapshot().document;
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(editor, &remove);
    check(editor->toPlainText() == fields.mid(6), "deleting a selected field leaves a broken raw token");
    canvas.undo();
    check(canvas.snapshot().document == beforeRemoval, "Undo did not restore the selected native page field");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("Hello world\t\t")), "composition cancellation fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    const auto selectHello = [editor] {
        auto replacement = editor->textCursor();
        replacement.setPosition(0); replacement.setPosition(5, QTextCursor::KeepAnchor);
        editor->setTextCursor(replacement);
    };
    selectHello();
    const auto beforeCancel = canvas.snapshot();
    QInputMethodEvent replacing(QStringLiteral("New"), {});
    QApplication::sendEvent(editor, &replacing);
    QInputMethodEvent cancel;
    QApplication::sendEvent(editor, &cancel);
    check(editor->toPlainText() == QStringLiteral("Hello world") &&
              canvas.snapshot().document == beforeCancel.document && canvas.snapshot().revision == beforeCancel.revision,
          "cancelling input method replacement leaves the inline source different from the saved story");
    selectHello();
    QApplication::sendEvent(editor, &replacing);
    editor->clearFocus();
    check(editor->toPlainText() == QStringLiteral("Hello world") && canvas.snapshot().document == beforeCancel.document,
          "leaving an uncommitted composition drops selected story text");
    editor->setFocus();
    selectHello();
    QApplication::sendEvent(editor, &replacing);
    QInputMethodEvent replacementCommit;
    replacementCommit.setCommitString(QStringLiteral("New"));
    QApplication::sendEvent(editor, &replacementCommit);
    if (canvas.headerText() != QStringLiteral("New world\t\t"))
        std::cerr << "Committed story: " << canvas.headerText().toStdString()
                  << "; widget: " << editor->toPlainText().toStdString() << '\n';
    check(canvas.headerText() == QStringLiteral("New world\t\t"), "committed composition did not replace the selected story text");
    canvas.undo();
    check(canvas.snapshot().document == beforeCancel.document, "Undo did not restore selected text after composition commit");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("One\nTwo\t\t")), "line-boundary fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    auto secondLine = editor->textCursor();
    secondLine.setPosition(4); editor->setTextCursor(secondLine);
    QApplication::sendEvent(editor, &home);
    check(editor->textCursor().position() == 4, "Home at a multiline boundary moves to the preceding line");
    QInputMethodQueryEvent secondLineQuery(Qt::ImCursorRectangle);
    QApplication::sendEvent(editor, &secondLineQuery);
    const double secondLineTop = secondLineQuery.value(Qt::ImCursorRectangle).toRectF().top();
    QApplication::sendEvent(editor, &end);
    check(editor->textCursor().position() == 7 && secondLineTop > 10,
          "multiline caret or End does not use the second rendered line");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QString(30, QLatin1Char('W')) + QStringLiteral("\t\t")), "soft-wrap fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    QMouseEvent wrappedClick(QEvent::MouseButtonPress, QPointF(1, 32), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &wrappedClick);
    const int wrappedStart = editor->textCursor().position();
    QApplication::sendEvent(editor, &home);
    check(wrappedStart > 0 && wrappedStart < 30 && editor->textCursor().position() == wrappedStart,
          "Home at a soft-wrap start moves to the preceding rendered line");
    auto firstWrappedLine = editor->textCursor();
    firstWrappedLine.setPosition(0); editor->setTextCursor(firstWrappedLine);
    QInputMethodQueryEvent firstWrappedQuery(Qt::ImCursorRectangle);
    QApplication::sendEvent(editor, &firstWrappedQuery);
    QApplication::sendEvent(editor, &end);
    QInputMethodQueryEvent wrappedEndQuery(Qt::ImCursorRectangle);
    QApplication::sendEvent(editor, &wrappedEndQuery);
    check(editor->textCursor().position() == wrappedStart &&
              std::abs(wrappedEndQuery.value(Qt::ImCursorRectangle).toRectF().top() -
                       firstWrappedQuery.value(Qt::ImCursorRectangle).toRectF().top()) <= 2,
          "End at a soft-wrap boundary paints the caret on the following line");
    QApplication::sendEvent(editor, &home);
    check(editor->textCursor().position() == 0,
          "Home after End at a soft-wrap boundary chooses the following line");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("{PAGE}\t\t")), "input-method field replacement fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    editor->moveCursor(QTextCursor::End);
    const auto beforeFieldReplacement = canvas.snapshot().document;
    QInputMethodEvent replaceFieldTail;
    replaceFieldTail.setCommitString(QStringLiteral("x"), -1, 1);
    QApplication::sendEvent(editor, &replaceFieldTail);
    check(editor->toPlainText() == QStringLiteral("x") && canvas.headerText() == QStringLiteral("x\t\t"),
          "input-method replacement can split a native PAGE field");
    canvas.undo();
    check(canvas.snapshot().document == beforeFieldReplacement,
          "input-method field replacement and Undo are not one transaction");
    std::cout << "Header/footer IME tests passed\n";
}
