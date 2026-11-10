#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QClipboard>
#include <QBuffer>
#include <QKeyEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::vector<std::uint8_t> png(const QColor& color) {
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(color);
    QByteArray data;
    QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly);
    check(image.save(&buffer, "PNG"), "story picture encoding");
    return {reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()};
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace docxstudio;
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.resize(1000, 1000);
    canvas.insertText(QStringLiteral("Body remains unchanged"));
    canvas.selectAll();
    check(canvas.setHeaderText(QStringLiteral("Header words\t\t")), "story clipboard fixture");
    const auto bodyBefore = canvas.snapshot().document.paragraphs();
    canvas.show();
    if (application.arguments().contains(QStringLiteral("--keyboard-only"))) {
        check(canvas.setHeaderText(QStringLiteral("\t\t")), "keyboard story picture fixture");
        canvas.beginHeaderFooterEditing(false, 0, 0);
        const auto beforePicture = canvas.snapshot().document;
        check(canvas.insertHeaderFooterImage(png(Qt::red), QStringLiteral("Red")), "keyboard story picture insert");
        const auto insertedPicture = canvas.snapshot().document;
        auto* target = canvas.findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
        target->selectAll();
        QApplication::clipboard()->setText(QStringLiteral("Sentinel"));
        QKeyEvent copyPicture(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(target, &copyPicture);
        check(QApplication::clipboard()->mimeData()->hasFormat(QStringLiteral("application/x-owl-docs-inline-image-v2")),
              "Ctrl+C bypasses the story picture clipboard lifecycle");
        QKeyEvent undoPicture(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QApplication::sendEvent(target, &undoPicture);
        check(canvas.snapshot().document == beforePicture, "Ctrl+Z bypasses the core story picture Undo transaction");
        QKeyEvent redoPicture(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QApplication::sendEvent(target, &redoPicture);
        check(canvas.snapshot().document == insertedPicture, "Ctrl+Shift+Z bypasses the core story picture Redo transaction");
        target->moveCursor(QTextCursor::End);
        target->insertPlainText(QStringLiteral("Text"));
        const auto insertedText = canvas.snapshot().document;
        QApplication::sendEvent(target, &undoPicture);
        check(canvas.snapshot().document == insertedPicture, "story text keyboard Undo does not use the core history");
        QApplication::sendEvent(target, &redoPicture);
        check(canvas.snapshot().document == insertedText, "story text keyboard Redo does not use the core history");
        auto pictureSelection = target->textCursor(); pictureSelection.setPosition(0);
        pictureSelection.setPosition(1, QTextCursor::KeepAnchor); target->setTextCursor(pictureSelection);
        QKeyEvent cutPicture(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier);
        QApplication::sendEvent(target, &cutPicture);
        check(canvas.snapshot().document.headerImages().empty() &&
              QApplication::clipboard()->mimeData()->hasFormat(QStringLiteral("application/x-owl-docs-inline-image-v2")),
              "Ctrl+X loses the story picture payload");
        QKeyEvent pastePicture(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QApplication::sendEvent(target, &pastePicture);
        check(canvas.snapshot().document.headerImages().size() == 1 &&
              canvas.snapshot().document.headerImages().front().encoded_payload == insertedPicture.headerImages().front().encoded_payload,
              "Ctrl+V fails to restore a cut story picture payload");
        return 0;
    }
    canvas.beginHeaderFooterEditing(false, 0, 0);
    auto* editor = canvas.findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
    check(editor, "story clipboard editor");
    canvas.selectAll();
    check(editor->textCursor().selectedText() == QStringLiteral("Header words") &&
          canvas.selectedText() == QStringLiteral("Header words"),
          "Select All or clipboard state targets hidden body text during header editing");
    canvas.copy();
    check(QApplication::clipboard()->text() == QStringLiteral("Header words"),
          "Copy copies the hidden body selection instead of the inline story selection");
    canvas.cut();
    check(canvas.headerText() == QStringLiteral("\t\t") && editor->toPlainText().isEmpty() &&
          QApplication::clipboard()->text() == QStringLiteral("Header words") &&
          canvas.snapshot().document.paragraphs() == bodyBefore,
          "Cut changes the hidden body instead of the selected header source");
    QApplication::clipboard()->setText(QStringLiteral("Page {PAGE}"));
    canvas.pasteTextOnly();
    check(canvas.headerText() == QStringLiteral("Page {PAGE}\t\t") &&
          editor->toPlainText() == QStringLiteral("Page {PAGE}") &&
          canvas.snapshot().document.paragraphs() == bodyBefore,
          "Paste Text Only changes the hidden body instead of the selected header source");
    canvas.endHeaderFooterEditing();
    canvas.beginHeaderFooterEditing(true, 2, 0);
    QApplication::clipboard()->setText(QStringLiteral("Footer"));
    canvas.pasteTextOnly();
    canvas.selectAll(); canvas.copy(); canvas.cut();
    check(canvas.footerText() == QStringLiteral("\t\t") && QApplication::clipboard()->text() == QStringLiteral("Footer") &&
          canvas.snapshot().document.paragraphs() == bodyBefore,
          "footer clipboard commands lose the active alignment region");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("Left\tMiddle\t")), "adjacent story picture fixture");
    canvas.beginHeaderFooterEditing(false, 2, 0);
    check(canvas.insertHeaderFooterImage(png(Qt::red), QStringLiteral("Red")) &&
          canvas.insertHeaderFooterImage(png(Qt::blue), QStringLiteral("Blue")), "adjacent story pictures insert");
    const auto withPictures = canvas.snapshot().document;
    check(withPictures.headerImages().size() == 2, "adjacent story pictures missing");
    auto* right = canvas.findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor2"));
    auto cursor = right->textCursor(); cursor.setPosition(0); right->setTextCursor(cursor);
    QKeyEvent removeFirst(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(right, &removeFirst);
    const auto remaining = canvas.snapshot().document;
    check(remaining.headerImages().size() == 1 && remaining.headerImages().front().id == withPictures.headerImages().back().id &&
          remaining.headerImages().front().encoded_payload == withPictures.headerImages().back().encoded_payload,
          "deleting an adjacent story picture loses or rejects the surviving picture identity");
    canvas.undo();
    check(canvas.snapshot().document == withPictures, "adjacent picture deletion Undo does not restore exact story");
    cursor = right->textCursor(); cursor.setPosition(0); cursor.setPosition(1, QTextCursor::KeepAnchor); right->setTextCursor(cursor);
    canvas.copy();
    check(QApplication::clipboard()->mimeData()->hasFormat(QStringLiteral("application/x-owl-docs-inline-image-v2")),
          "copying a header/footer picture loses its native image payload");
    canvas.cut();
    check(canvas.snapshot().document.headerImages().size() == 1 &&
          canvas.snapshot().document.headerImages().front().id == withPictures.headerImages().back().id &&
          QApplication::clipboard()->mimeData()->hasFormat(QStringLiteral("application/x-owl-docs-inline-image-v2")),
          "cutting a story picture loses the clipboard payload or changes its neighbor");
    canvas.paste();
    const auto pasted = canvas.snapshot().document;
    check(pasted.headerImages().size() == 2 && pasted.headerImages().front().encoded_payload == withPictures.headerImages().front().encoded_payload &&
          pasted.headerImages().back().encoded_payload == withPictures.headerImages().back().encoded_payload,
          "pasting a cut story picture fails to preserve both picture payloads");
    canvas.selectAll();
    const auto beforeMixedCut = canvas.snapshot();
    canvas.cut();
    check(canvas.snapshot().document == beforeMixedCut.document && canvas.snapshot().revision == beforeMixedCut.revision,
          "mixed or multiple story picture Cut deletes data that its clipboard cannot preserve");
    std::cout << "Header/footer clipboard tests passed\n";
}
