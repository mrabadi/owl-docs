#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace docxstudio;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void structuralNumbering(app::SpellChecker& spelling, const std::string& kind) {
    std::cout << "Numbering structural route: " << kind << std::endl;
    app::DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("First\nSecond\nThird"));
    canvas.selectAll(); canvas.toggleNumbering(); canvas.markSaved();
    const auto original = canvas.snapshot();
    const auto middle = original.document.paragraphs()[1].id();
    const auto third = original.document.paragraphs()[2].id();
    check(original.document.paragraphs()[0].format().list_id && original.document.paragraphs()[1].format().list_id ==
        original.document.paragraphs()[0].format().list_id, "fixture is not one native numbered run");
    codex::Json operation{{"kind", kind}, {"target", {{"blockId", middle.toString()}, {"start", 0}}}};
    if (kind != "insert_page_break") operation["text"] = "Inserted source";
    const QString documentId = QStringLiteral("numbering-structural");
    QString summary, error;
    const auto preview = [&] {
        error.clear();
        const auto result = app::invokeEditorTool(canvas, documentId, QStringLiteral("editor_v1_preview"),
            {{"documentId", documentId.toStdString()}, {"baseRevision", canvas.snapshot().revision.value()},
                {"summary", "Insert inside a native numbered list"}, {"operations", codex::Json::array({operation})}}, summary, error);
        if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
        check(error.isEmpty() && !result.empty() && canvas.hasPreview(), "numbered structural edit did not create a preview");
        check(canvas.snapshot().document == original.document && canvas.snapshot().revision == original.revision &&
            !canvas.isModified(), "numbered structural preview mutated live content/history");
        const auto visible = canvas.findNextHit(QStringLiteral("Second"));
        check(visible && visible->previewId, "numbered structural edit was not searchable on its isolated branch");
    };
    preview(); canvas.discardPreview();
    check(canvas.snapshot().document == original.document && !canvas.isModified(), "discard changed the native numbered run");
    preview(); check(canvas.acceptPreview(error), "accept numbered structural edit");
    const auto accepted = canvas.snapshot();
    app::ListEditRequest refresh; refresh.kind = app::ListEditRequest::Kind::refresh_numbering;
    refresh.numberingBaseline = &original.document;
    std::vector<core::Operation> pending;
    check(canvas.planListOperations(accepted.document, refresh, pending, error), "shared numbered-list planner rejected structural result");
    check(pending.empty(), "structural Bridge route skipped shared native numbering refresh");
    check(accepted.document.findParagraph(third)->text().starts_with(u"4."), "structural result differs from native planner numbering");
    check(accepted.revision.value() == original.revision.value() + 1, "numbering refresh was committed separately");
    canvas.undo();
    check(canvas.snapshot().document == original.document && !canvas.isModified(), "one Undo did not restore numbered source identities/formatting");
    canvas.redo();
    check(canvas.snapshot().document == accepted.document, "Redo did not restore structural edit and native numbering together");
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv); app::SpellChecker spelling;
    for (const std::string kind : {"insert_page_break", "insert_caption", "insert_bibliography_entry"})
        if (argc == 1 || kind == argv[1]) structuralNumbering(spelling, kind);
    std::cout << "Codex structural numbering tests passed\n";
}
