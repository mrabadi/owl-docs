#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"
#include <QApplication>
#include <cstdlib>
#include <iostream>
#include <set>

using namespace docxstudio;
namespace {
void check(bool result, const char* message) {
    if (!result) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("Body"));
    const auto before = canvas.snapshot();
    const auto paragraph = before.document.paragraphs().front().id().toString();
    QString summary, error;
    auto preview = app::invokeEditorTool(canvas, QStringLiteral("academic-doc"),
        QString::fromUtf8(codex::kEditorPreviewTool.data(), static_cast<qsizetype>(codex::kEditorPreviewTool.size())),
        {{"documentId", "academic-doc"}, {"baseRevision", before.revision.value()}, {"summary", "Insert a footnote"},
         {"operations", {{{"kind", "insert_note"}, {"target", {{"blockId", paragraph}, {"start", 4}}},
                           {"noteKind", "footnote"}, {"text", "A bounded note"}}}}}, summary, error);
    if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
    check(!preview.empty() && error.isEmpty() && canvas.hasPreview(), "Codex creates academic preview");
    check(canvas.snapshot().document == before.document, "academic preview never mutates authoritative document");
    check(canvas.acceptPreview(error), "accept academic preview");
    const auto accepted = canvas.snapshot();
    check(accepted.document.notes().size() == 1 && accepted.document.paragraphs().front().fields().size() == 1,
          "accepted note has its semantic reference");
    const auto read = app::invokeEditorTool(canvas, QStringLiteral("academic-doc"),
        QString::fromUtf8(codex::kEditorReadTool.data(), static_cast<qsizetype>(codex::kEditorReadTool.size())),
        {{"documentId", "academic-doc"}, {"scope", "document"}}, summary, error);
    check(error.isEmpty() && read.at("blocks").size() == 2, "Codex reads body and bounded note story");
    check(read.at("blocks").at(0).at("attributes").at("fields").size() == 1, "Codex reads field metadata with offset-safe text");
    check(read.at("blocks").at(1).at("type") == "note", "Codex note read has explicit block type");
    canvas.undo();
    check(canvas.snapshot().document == before.document, "accepted academic preview is one undo transaction");
    auto heading = core::Paragraph::create(u"Introduction").value();
    auto caption = core::Paragraph::create(u"Owl anatomy").value();
    auto entry = core::Paragraph::create(u"Rabadi. A source. 2026.").value();
    auto prose = core::Paragraph::create(u"See ").value();
    auto academic = core::Document::create({heading, caption, entry, prose}).value();
    check(bool(academic.setParagraphStyle({heading.id()}, "Heading1")), "heading fixture");
    check(bool(academic.setCaption(caption.id(), core::CaptionMetadata{core::CaptionLabel::figure, prose.id()})), "caption fixture");
    check(bool(academic.setBibliographyEntry(entry.id(), true)), "source entry fixture");
    const auto equation = core::NodeId::generate();
    check(bool(academic.insertEquation({prose.id(), 4}, "x", false, equation)), "equation fixture");
    const auto advancedNote = core::NodeId::generate();
    check(bool(academic.setNote({advancedNote, core::NoteKind::endnote, u"Initial note"})), "advanced note fixture");
    check(bool(academic.insertField({prose.id(), 5}, {core::NodeId::generate(), 0, core::FieldKind::note_reference, advancedNote})), "advanced note reference");
    canvas.setDocument(academic);
    const auto advancedBase = canvas.snapshot();
    preview = app::invokeEditorTool(canvas, QStringLiteral("academic-doc"),
        QString::fromUtf8(codex::kEditorPreviewTool.data(), static_cast<qsizetype>(codex::kEditorPreviewTool.size())),
        {{"documentId", "academic-doc"}, {"baseRevision", advancedBase.revision.value()}, {"summary", "Add academic references"},
         {"operations", {
            {{"kind", "replace_equation"}, {"equationId", equation.toString()}, {"latex", "x^2"}, {"display", true}},
            {{"kind", "insert_cross_reference"}, {"target", {{"blockId", prose.id().toString()}, {"start", 0}}}, {"targetId", caption.id().toString()}},
            {{"kind", "insert_citation"}, {"target", {{"blockId", prose.id().toString()}, {"start", 1}}}, {"entryId", entry.id().toString()}, {"locator", "p. 42"}},
            {{"kind", "insert_toc"}, {"target", {{"blockId", heading.id().toString()}, {"start", 0}}}, {"maxLevel", 3}},
            {{"kind", "insert_caption"}, {"target", {{"blockId", prose.id().toString()}, {"start", 2}}}, {"label", "Table"}, {"text", "A second caption"}},
            {{"kind", "insert_bibliography_entry"}, {"target", {{"blockId", prose.id().toString()}, {"start", 2}}}, {"text", "A second source"}}
         }}}, summary, error);
    if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
    check(!preview.empty() && error.isEmpty(), "Codex previews equations, references, citations, captions and TOC together");
    check(canvas.snapshot().document == advancedBase.document, "compound academic preview remains isolated");
    check(canvas.acceptPreview(error), "accept compound academic preview");
    const auto advanced = canvas.snapshot();
    check(advanced.document.findEquation(equation)->canonical_latex == "x^{2}" && advanced.document.findEquation(equation)->display,
          "Codex revises equation without replacing its identity");
    check(advanced.document.tablesOfContents().size() == 1 && advanced.document.paragraphs().size() == 8,
          "Codex creates real TOC and independent caption/source paragraphs");
    canvas.undo();
    check(canvas.snapshot().document == advancedBase.document, "compound academic preview is one undo transaction");
    canvas.setDocument(advanced.document);
    const auto tocId = advanced.document.tablesOfContents().front().id;
    const auto propose = [&](const codex::Json& operations) {
        return app::invokeEditorTool(canvas, QStringLiteral("academic-doc"),
            QString::fromUtf8(codex::kEditorPreviewTool.data(), static_cast<qsizetype>(codex::kEditorPreviewTool.size())),
            {{"documentId", "academic-doc"}, {"baseRevision", canvas.snapshot().revision.value()},
             {"summary", "Update academic settings"}, {"operations", operations}}, summary, error);
    };
    check(!propose({{{"kind", "configure_toc"}, {"tocId", tocId.toString()}, {"maxLevel", 9}},
                    {{"kind", "set_note_text"}, {"noteId", advancedNote.toString()}, {"text", "Updated note"}}}).empty(),
          "Codex previews note editing and TOC configuration");
    check(canvas.acceptPreview(error) && canvas.snapshot().document.findToc(tocId)->max_level == 9 &&
          canvas.snapshot().document.findNote(advancedNote)->text == u"Updated note", "academic settings apply together");
    check(!propose({{{"kind", "delete_toc"}, {"tocId", tocId.toString()}}}).empty() && canvas.acceptPreview(error) &&
          canvas.snapshot().document.tablesOfContents().empty(), "Codex deletes a generated TOC explicitly");
    const auto valid = canvas.snapshot();
    check(propose({{{"kind", "insert_note"}, {"target", {{"blockId", prose.id().toString()}, {"start", 0}}},
                    {"text", "Bad kind"}, {"noteKind", true}}}).empty() && !error.isEmpty() && !canvas.hasPreview() &&
          canvas.snapshot().document == valid.document, "malformed academic request returns an error without a partial preview");
    std::set<std::string> advertised;
    for (const auto& tool : codex::editorV1ToolDefinitions()) if (tool.name == codex::kEditorPreviewTool) {
        for (const auto& operation : tool.inputSchema.at("$defs").at("operation").at("oneOf"))
            advertised.insert(operation.at("properties").at("kind").at("const").get<std::string>());
    }
    for (const auto* operation : {"replace_equation", "insert_note", "set_note_text", "insert_caption",
        "insert_bibliography_entry", "insert_cross_reference", "insert_citation", "insert_toc", "configure_toc", "delete_toc"})
        check(advertised.contains(operation), "dynamic tool catalog advertises every implemented academic operation");
    std::cout << "Academic tool tests passed\n";
}
