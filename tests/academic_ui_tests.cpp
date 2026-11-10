#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/core/academic.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <cstdlib>
#include <iostream>

namespace { void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
} }
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace docxstudio;
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("Writer"));
    const auto initial = canvas.snapshot().document;
    check(canvas.insertNote(QStringLiteral("Research note"), core::NoteKind::footnote), "note insertion failed");
    check(canvas.snapshot().document.notes().size() == 1, "note insertion did not create note story");
    canvas.undo();
    check(canvas.snapshot().document == initial, "note insertion is not one undoable transaction");
    canvas.redo();
    check(canvas.insertCaption(core::CaptionLabel::figure, QStringLiteral("A useful figure")), "caption insertion failed");
    check(canvas.insertBibliographyEntry(QStringLiteral("Author. Research source. 2026.")), "bibliography insertion failed");
    auto snapshot = canvas.snapshot();
    core::NodeId entry{};
    core::NodeId caption{};
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        if (paragraph.isBibliographyEntry()) entry = paragraph.id();
        if (paragraph.caption()) caption = paragraph.id();
    }
    check(entry.isValid() && caption.isValid(), "academic paragraphs lost metadata");
    check(canvas.insertCitation(entry, QStringLiteral("p. 42")) && canvas.insertCrossReference(caption), "citation/cross-reference insertion failed");
    snapshot = canvas.snapshot();
    const auto resolved = core::resolveAcademic(snapshot.document);
    bool citationFound = false;
    for (const auto& field : resolved.fields) if (field.text == u"[1, p. 42]") citationFound = true;
    check(citationFound, "citation did not resolve its manual entry");
    check(canvas.insertTableOfContents(3) && canvas.snapshot().document.tablesOfContents().size() == 1, "TOC insertion failed");
    core::Document linked;
    const auto beforeHeading = linked.paragraphs()[0].id();
    const auto heading = core::NodeId::generate();
    check(bool(linked.insertText({beforeHeading, 0}, u"Body before the heading")) &&
          bool(linked.splitParagraph({beforeHeading, 23}, heading)) &&
          bool(linked.insertText({heading, 0}, u"Linked heading")) &&
          bool(linked.setParagraphStyle({heading}, std::string("Heading1"))) &&
          bool(linked.insertToc(beforeHeading, {core::NodeId::generate(), 3})), "TOC link fixture failed");
    app::DocumentCanvas linkedCanvas(spelling);
    linkedCanvas.resize(900, 700);
    linkedCanvas.setDocument(linked);
    linkedCanvas.show();
    QApplication::processEvents();
    const double scale = 96.0 / 72.0;
    const double pageWidth = linkedCanvas.pageWidthPoints() * scale;
    const double pageLeft = (std::max(pageWidth + 56.0, static_cast<double>(linkedCanvas.viewport()->width())) - pageWidth) / 2.0;
    const QPointF tocPoint(pageLeft + 85.0 * scale, 28.0 + 100.0 * scale);
    QMouseEvent tocClick(QEvent::MouseButtonPress, tocPoint, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(linkedCanvas.viewport(), &tocClick);
    check(linkedCanvas.selection().focus.paragraph_id == heading,
          "clicking a TOC entry does not navigate to its heading");
    app::DocumentCanvas clipboardCanvas(spelling);
    check(clipboardCanvas.insertNote(QStringLiteral("Copied note"), core::NoteKind::footnote), "clipboard note fixture failed");
    QKeyEvent select(QEvent::KeyPress, Qt::Key_Left, Qt::ShiftModifier);
    QApplication::sendEvent(&clipboardCanvas, &select);
    clipboardCanvas.copy();
    QKeyEvent collapse(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(&clipboardCanvas, &collapse);
    clipboardCanvas.paste();
    check(clipboardCanvas.snapshot().document.notes().size() == 2,
          "copy/paste flattened a note instead of creating an independent note reference");
    QKeyEvent selectNote(QEvent::KeyPress, Qt::Key_Left, Qt::ShiftModifier);
    QApplication::sendEvent(&clipboardCanvas, &selectNote);
    bool edited = false;
    QTimer::singleShot(0, &clipboardCanvas, [&] {
        auto* dialog = clipboardCanvas.findChild<QDialog*>(QStringLiteral("references.dialog"));
        check(dialog != nullptr, "F2 on a selected note did not open its editor");
        auto* text = dialog->findChild<QPlainTextEdit*>(QStringLiteral("references.text"));
        check(text && text->toPlainText() == QStringLiteral("Copied note"), "note editor did not select the referenced note");
        text->setPlainText(QStringLiteral("Edited copy"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        edited = true;
    });
    QKeyEvent editNote(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
    QApplication::sendEvent(&clipboardCanvas, &editNote);
    QApplication::processEvents();
    check(edited && clipboardCanvas.snapshot().document.notes()[0].text == u"Copied note" &&
              clipboardCanvas.snapshot().document.notes()[1].text == u"Edited copy", "editing the copied note changed its source note");
    std::cout << "academic UI tests passed\n";
}
