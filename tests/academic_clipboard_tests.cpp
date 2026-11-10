#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/core/academic.h"
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void key(QWidget& widget, int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, value, modifiers);
    QApplication::sendEvent(&widget, &event);
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace docxstudio;
    app::SpellChecker spelling;
    core::Document document;
    const auto first = document.paragraphs().front().id();
    check(bool(document.insertText({first, 0}, u"Before after")), "clipboard prose fixture");
    const auto note = core::NodeId::generate();
    check(bool(document.setNote({note, core::NoteKind::footnote, u"The complete note story"})) &&
          bool(document.insertField({first, 7}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, note})) &&
          bool(document.insertEquation({first, 8}, "\\frac{α}{2}")), "clipboard atom fixture");
    const auto second = core::NodeId::generate();
    check(bool(document.splitParagraph({first, document.paragraphs().front().text().size()}, second)) &&
          bool(document.insertText({second, 0}, u"Second paragraph")) &&
          bool(document.insertField({second, 16}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, note})), "second clipboard paragraph");
    app::DocumentCanvas canvas(spelling);
    canvas.setDocument(document);
    canvas.selectAll();
    canvas.cut();
    check(canvas.snapshot().document.notes().empty(), "cut should remove selected notes");
    const auto cutDocument = canvas.snapshot().document;
    canvas.paste();
    const auto pasted = canvas.snapshot().document;
    check(pasted.paragraphs().size() == 2 && pasted.notes().size() == 1 &&
          pasted.notes().front().text == u"The complete note story" &&
          pasted.notes().front().id != note, "mixed cut/paste lost the independent note story");
    check(pasted.paragraphs()[0].equations().size() == 1 &&
          pasted.paragraphs()[0].equations()[0].canonical_latex == "\\frac{α}{2}" &&
          pasted.paragraphs()[0].text() == document.paragraphs()[0].text() &&
          pasted.paragraphs()[1].text() == document.paragraphs()[1].text(), "mixed paste flattened or reordered atoms/prose");
    check(pasted.paragraphs()[0].fields()[0].target_id == pasted.paragraphs()[1].fields()[0].target_id,
          "repeated note references should share one copied note story");
    canvas.undo();
    check(canvas.snapshot().document == cutDocument, "mixed paste must be one undo transaction");
    canvas.undo();
    check(canvas.snapshot().document == document, "cut undo must restore exact source identities");
    canvas.selectAll();
    canvas.copy();
    key(canvas, Qt::Key_Right);
    canvas.paste();
    check(canvas.snapshot().document.notes().size() == 2, "mixed copy/paste must clone notes independently");

    auto structural = document;
    check(bool(structural.setCaption(second, core::CaptionMetadata{core::CaptionLabel::figure, first})), "structural clipboard fixture");
    canvas.setDocument(structural);
    canvas.selectAll();
    QApplication::clipboard()->setText(QStringLiteral("unchanged clipboard"));
    canvas.cut();
    check(canvas.snapshot().document == structural && QApplication::clipboard()->text() == QStringLiteral("unchanged clipboard"),
          "unsupported structural cut must leave document and clipboard unchanged");
    for (int role = 0; role < 3; ++role) {
        core::Document semantic;
        const auto paragraph = semantic.paragraphs().front().id();
        check(bool(semantic.insertText({paragraph, 0}, u"Leading prose")), "single semantic prose fixture");
        std::size_t prefix = 0;
        if (role == 0) check(bool(semantic.setParagraphStyle({paragraph}, std::string("Heading1"))), "single heading role");
        else {
            if (role == 1) check(bool(semantic.setCaption(paragraph, core::CaptionMetadata{core::CaptionLabel::figure, paragraph})), "single caption role");
            else check(bool(semantic.setBibliographyEntry(paragraph, true)), "single bibliography role");
            check(bool(semantic.insertText({paragraph, 0}, role == 1 ? u": " : u". ")) &&
                  bool(semantic.insertField({paragraph, 0}, {core::NodeId::generate(), 0,
                    role == 1 ? core::FieldKind::caption_number : core::FieldKind::bibliography_number, paragraph})), "single semantic prefix");
            prefix = 3;
        }
        canvas.setDocument(semantic);
        check(canvas.navigateToParagraph(paragraph), "navigate to semantic prose");
        for (std::size_t index = 0; index < prefix; ++index) key(canvas, Qt::Key_Right);
        for (int index = 0; index < 7; ++index) key(canvas, Qt::Key_Right, Qt::ShiftModifier);
        canvas.cut();
        check(canvas.snapshot().document.paragraphs().front().text() != semantic.paragraphs().front().text(),
              "ordinary prose cut within a semantic paragraph was incorrectly blocked");
        canvas.paste();
        const auto restored = canvas.snapshot().document.paragraphs().front();
        check(restored.id() == paragraph && restored.text() == semantic.paragraphs().front().text() &&
              restored.caption() == semantic.paragraphs().front().caption() &&
              restored.isBibliographyEntry() == semantic.paragraphs().front().isBibliographyEntry() &&
              restored.styleId() == semantic.paragraphs().front().styleId(), "single-paragraph prose move lost its semantic role");
        if (role != 0) {
            check(canvas.navigateToParagraph(paragraph), "navigate to number prefix");
            key(canvas, Qt::Key_Right, Qt::ShiftModifier);
            const auto beforePrefix = canvas.snapshot().document;
            canvas.cut();
            check(canvas.snapshot().document == beforePrefix, "number prefix cut must remain non-destructive");
        }
    }
    auto heading = core::Paragraph::create(u"Linked heading").value();
    auto prose = core::Paragraph::create(u"See ").value();
    auto linked = core::Document::create({heading, prose}).value();
    check(bool(linked.setParagraphStyle({heading.id()}, std::string("Heading1"))) &&
          bool(linked.insertField({prose.id(), 4}, {core::NodeId::generate(), 0, core::FieldKind::cross_reference, heading.id()})), "linked clipboard fixture");
    canvas.setDocument(linked);
    check(canvas.navigateToParagraph(prose.id()), "navigate to copied reference");
    for (int index = 0; index < 4; ++index) key(canvas, Qt::Key_Right);
    key(canvas, Qt::Key_Right, Qt::ShiftModifier);
    canvas.copy();
    app::DocumentCanvas destination(spelling);
    destination.paste();
    check(destination.snapshot().document.paragraphs().front().fields().empty() &&
          destination.snapshot().document.paragraphs().front().text() == u"Linked heading",
          "cross-document reference must use visible text when its target is absent");
    const auto beforeMalformed = destination.snapshot().document;
    auto* malformed = new QMimeData;
    const QJsonObject payload{{"version", 2}, {"type", "range"}, {"paragraphs", QJsonArray{QJsonArray{QJsonObject{{"equation", "x"}, {"display", "wrong type"}}}}}};
    malformed->setData(QStringLiteral("application/x-owl-docs-atom+json"), QJsonDocument(payload).toJson());
    QApplication::clipboard()->setMimeData(malformed);
    destination.paste();
    check(destination.snapshot().document == beforeMalformed, "malformed range payload must be rejected atomically");
    std::cout << "Academic clipboard tests passed\n";
}
