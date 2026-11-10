#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/core/academic.h"
#include <QApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
QByteArray pdfText(docxstudio::app::DocumentCanvas& canvas, const QString& path) {
    QString error;
    check(canvas.exportPdf(path, error), "academic PDF could not export");
    QProcess process;
    process.start(QStringLiteral("pdftotext"), {QStringLiteral("-layout"), path, QStringLiteral("-")});
    check(process.waitForFinished(10000) && process.exitCode() == 0, "pdftotext failed");
    return process.readAllStandardOutput();
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace docxstudio;
    app::SpellChecker spelling;
    core::Document document;
    const auto paragraph = document.paragraphs()[0].id();
    check(bool(document.insertText({paragraph, 0}, u"Academic anchor")), "body fixture failed");
    const core::NodeId noteId = core::NodeId::generate();
    check(bool(document.setNote({noteId, core::NoteKind::footnote, u"FootnoteEvidence"})), "note fixture failed");
    check(bool(document.insertField({paragraph, 15}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, noteId})), "note reference failed");
    app::DocumentCanvas canvas(spelling);
    canvas.setDocument(document);
    QTemporaryDir output;
    check(output.isValid(), "temporary output unavailable");
    auto text = pdfText(canvas, output.filePath(QStringLiteral("note.pdf")));
    check(text.contains("Academic anchor") && text.contains("FootnoteEvidence"), "screen/PDF layout omitted footnote text");
    const core::NodeId second = core::NodeId::generate();
    check(bool(document.splitParagraph({paragraph, document.paragraphs()[0].text().size()}, second)), "heading split failed");
    check(bool(document.insertText({second, 0}, u"Research Heading")), "heading fixture failed");
    check(bool(document.setParagraphStyle({second}, std::string("Heading1"))), "heading style failed");
    check(bool(document.insertToc(paragraph, {core::NodeId::generate(), 3})), "TOC fixture failed");
    canvas.setDocument(document);
    text = pdfText(canvas, output.filePath(QStringLiteral("toc.pdf")));
    check(text.count("Research Heading") == 2 && text.contains("Contents"), "TOC did not render resolved heading and page number");
    std::u16string longText;
    for (int i=0; i<90; ++i) longText += u"FootnoteContinuationLine\n";
    longText += u"FootnoteFinalEvidence";
    check(bool(document.setNote({noteId, core::NoteKind::footnote, longText})), "long note fixture failed");
    canvas.setDocument(document);
    canvas.setPageSizePoints(360, 360);
    canvas.setMarginsPoints(36, 36, 36, 36);
    text = pdfText(canvas, output.filePath(QStringLiteral("long-note.pdf")));
    check(canvas.pageCount() > 2 && text.contains("FootnoteFinalEvidence") && text.count("FootnoteContinuationLine") == 90,
          "long footnote overflow was clipped or duplicated");
    core::Document competing;
    const auto anchor = competing.paragraphs()[0].id();
    check(bool(competing.insertText({anchor, 0}, u"Two anchors")), "competing anchors failed");
    const auto longId = core::NodeId::generate();
    const auto shortId = core::NodeId::generate();
    check(bool(competing.setNote({longId, core::NoteKind::footnote, longText})) &&
          bool(competing.setNote({shortId, core::NoteKind::footnote, u"SecondNoteFirstLine"})), "competing notes failed");
    check(bool(competing.insertField({anchor, 11}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, longId})) &&
          bool(competing.insertField({anchor, 12}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, shortId})), "competing references failed");
    canvas.setDocument(competing);
    text = pdfText(canvas, output.filePath(QStringLiteral("competing-notes.pdf")));
    check(text.split('\f').front().contains("SecondNoteFirstLine"),
          "a long footnote displaced another note's first line away from its anchor page");
    core::Document dense;
    const auto denseAnchor = dense.paragraphs()[0].id();
    for (std::size_t index=0; index<19; ++index) {
        const auto id = core::NodeId::generate();
        const auto number = std::to_string(index);
        std::u16string noteText = u"DenseNote";
        noteText.append(number.begin(), number.end());
        check(bool(dense.setNote({id, core::NoteKind::footnote, noteText})) &&
            bool(dense.insertField({denseAnchor, index}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, id})), "dense reference fixture failed");
    }
    canvas.setDocument(dense);
    text = pdfText(canvas, output.filePath(QStringLiteral("dense-notes.pdf")));
    check(text.count("DenseNote") == 19, "dense footnote references silently dropped first-line note content");
    core::Document references;
    const auto referenceParagraph = references.paragraphs()[0].id();
    const auto headingTarget = core::NodeId::generate();
    std::u16string headingText;
    for (int index = 0; index < 60; ++index) headingText += u"LongReferenceWord ";
    check(bool(references.splitParagraph({referenceParagraph, 0}, headingTarget)) &&
          bool(references.insertText({headingTarget, 0}, headingText)) &&
          bool(references.setParagraphStyle({headingTarget}, std::string("Heading1"))) &&
          bool(references.insertField({referenceParagraph, 0}, {core::NodeId::generate(), 0,
              core::FieldKind::cross_reference, headingTarget, {}, core::CrossReferenceFormat::full_text})),
          "long reference fixture failed");
    canvas.setDocument(references);
    text = pdfText(canvas, output.filePath(QStringLiteral("long-reference.pdf")));
    check(text.count("LongReferenceWord") == 120,
          "full-text references overflow page margins instead of wrapping their resolved text");
    std::cout << "academic layout tests passed\n";
}
