#include "docxstudio/core/document_session.h"
#include "docxstudio/core/academic.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace docxstudio::core;

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void equationReplacement() {
    Document document;
    const auto paragraph = document.paragraphs().front().id();
    const auto equation = NodeId::generate();
    check(bool(document.insertText({paragraph, 0}, u"beforeafter")), "fixture text");
    check(bool(document.insertEquation({paragraph, 6}, "x", false, equation)), "fixture equation");
    DocumentSession session(document);
    const auto initial = session.snapshot();
    const std::vector<Operation> edits{ReplaceEquation{equation, "x^{2}", true}};
    check(bool(session.applyBatch(initial.revision, edits)), "replace equation");
    const auto changed = session.snapshot();
    const auto* atom = changed.document.findEquation(equation);
    check(atom && atom->canonical_latex == "x^{2}" && atom->display && atom->utf16_offset == 6,
          "replacement retains identity and offset");
    check(changed.document.paragraphs().front().text() == u"before\ufffcafter", "replacement retains surrounding text");
    check(bool(session.undo(changed.revision)), "undo replacement");
    check(session.snapshot().document == initial.document, "undo restores exact equation");
}
void academicAtoms() {
    Document document;
    const auto paragraph = document.paragraphs().front().id();
    const auto note = NodeId::generate();
    const auto field = NodeId::generate();
    check(bool(document.setNote({note, NoteKind::footnote, u"A note"})), "create note");
    check(bool(document.insertText({paragraph, 0}, u"ab")), "academic fixture");
    check(bool(document.insertField({paragraph, 1}, {field, 0, FieldKind::note_reference, note})), "insert note reference");
    check(document.findField(field) && document.findField(field)->utf16_offset == 1, "field identity");
    check(bool(document.insertEquation({paragraph, 0}, "x")), "insert before field");
    check(document.findField(field)->utf16_offset == 2, "equation shifts field");
    const auto second = NodeId::generate();
    check(bool(document.splitParagraph({paragraph, 2}, second)), "split at field");
    check(document.findParagraph(second)->fieldAt(0)->id == field, "field moves on split");
    check(bool(document.mergeWithNext(paragraph)), "merge at field");
    check(document.findField(field)->utf16_offset == 2, "field moves on merge");
    check(bool(document.deleteRange({{paragraph, 2}, {paragraph, 3}})), "delete field");
    check(!document.findField(field) && !document.findNote(note), "delete cascades note");
}
void resolutionAndPreviews() {
    auto heading = Paragraph::create(u"Introduction").value();
    auto caption = Paragraph::create(u"An owl").value();
    auto bibliography = Paragraph::create(u"Rabadi. Example paper. 2026.").value();
    auto document = Document::create({heading, caption, bibliography}).value();
    check(bool(document.setParagraphStyle({heading.id()}, "Heading1")), "heading style");
    check(bool(document.setCaption(caption.id(), CaptionMetadata{CaptionLabel::figure, NodeId::generate()})), "caption metadata");
    check(bool(document.insertField({caption.id(), 0}, {NodeId::generate(), 0, FieldKind::caption_number, caption.id()})), "caption prefix atom");
    check(bool(document.insertText({caption.id(), 1}, u": ")), "caption separator");
    check(bool(document.setBibliographyEntry(bibliography.id(), true)), "bibliography metadata");
    const auto citation = NodeId::generate();
    check(bool(document.insertField({heading.id(), 12}, {citation, 0, FieldKind::citation, bibliography.id(), u"p. 42"})), "citation");
    const auto reference = NodeId::generate();
    check(bool(document.insertField({heading.id(), 13}, {reference, 0, FieldKind::cross_reference, caption.id()})), "cross reference");
    auto resolved = resolveAcademic(document);
    check(resolved.fieldText(citation) == u"[1, p. 42]", "manual citation resolves number and locator");
    check(resolved.fieldText(reference) == u"Figure 1", "caption reference resolves sequence");
    check(bool(document.replaceField(reference, {reference, 0, FieldKind::cross_reference,
        caption.id(), {}, CrossReferenceFormat::full_text})), "full caption reference");
    check(resolveAcademic(document).fieldText(reference) == u"Figure 1: An owl", "full caption reference has exactly one prose separator");
    check(resolved.toc_entries.size() == 1 && resolved.toc_entries.front().text == u"Introduction", "TOC entries use semantic headings");
    const auto toc = NodeId::generate();
    check(bool(document.insertToc(heading.id(), {toc, 3})), "insert TOC block");
    check(document.bodyBlocks().front().kind == BodyBlockKind::table_of_contents, "TOC occupies real body order");
    check(!document.configureToc(toc, 0), "reject invalid TOC level");
    DocumentSession session(document);
    const auto base = session.snapshot();
    const auto preview = session.createPreview(base.revision).value();
    const std::vector<Operation> operations{SetNote{{NodeId::generate(), NoteKind::endnote, u"Preview only"}}};
    check(bool(session.applyPreviewBatch(preview.id, preview.revision, operations)), "academic preview");
    check(session.snapshot().document.notes().empty(), "preview remains isolated");
    check(bool(session.discardPreview(preview.id)), "discard academic preview");
}
void unicodeEquationCopy() {
    Document document;
    const auto paragraph = document.paragraphs().front().id();
    check(bool(document.insertEquation({paragraph, 0}, "α+β")), "Unicode equation fixture");
    check(academicParagraphText(document.paragraphs().front(), resolveAcademic(document)) == u"\\(α+β\\)",
          "equation text fallback preserves UTF-8 scalar values");
}
void semanticParagraphIdentity() {
    auto empty = Paragraph::create(u"").value();
    auto caption = Paragraph::create(u"An owl").value();
    auto reference = Paragraph::create(u"See ").value();
    auto document = Document::create({empty, caption, reference}).value();
    check(bool(document.setCaption(caption.id(), CaptionMetadata{CaptionLabel::figure, NodeId::generate()})), "semantic caption fixture");
    check(bool(document.insertField({caption.id(), 0}, {NodeId::generate(), 0, FieldKind::caption_number, caption.id()})), "semantic caption prefix");
    const auto field = NodeId::generate();
    check(bool(document.insertField({reference.id(), 4}, {field, 0, FieldKind::cross_reference, caption.id()})), "semantic reference fixture");
    check(bool(document.mergeWithNext(empty.id())), "merge caption into neutral paragraph");
    check(document.findParagraph(empty.id())->caption().has_value() && document.findField(field)->target_id == empty.id(),
          "merge retains surviving caption role and reference graph");
    const auto moved = NodeId::generate();
    check(bool(document.splitParagraph({empty.id(), 0}, moved)), "split before complete caption");
    check(!document.findParagraph(empty.id())->caption() && document.findParagraph(moved)->caption() &&
          document.findField(field)->target_id == moved && resolveAcademic(document).fieldText(field) == u"Figure 1",
          "split at zero moves complete caption identity and references");
    const auto toc = NodeId::generate();
    check(bool(document.insertToc(moved, {toc, 3})), "range TOC fixture");
    const auto before = document;
    check(!document.deleteRange({{empty.id(), 0}, {moved, 1}}) && document == before,
          "text deletion across generated TOC is rejected atomically");
    check(!document.setNote({moved, NoteKind::footnote, u"Duplicate identity"}), "note ID cannot collide with paragraph target");
}
}

int main() { equationReplacement(); academicAtoms(); resolutionAndPreviews(); unicodeEquationCopy(); semanticParagraphIdentity(); std::cout << "Academic core tests passed\n"; }
