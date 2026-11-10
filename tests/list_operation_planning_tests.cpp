#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/app/EditorToolBridge.h"

#include <QApplication>
#include <cstdlib>
#include <iostream>

using namespace docxstudio;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
core::Document replayPlan(core::Document document, const std::vector<core::Operation>& operations) {
    for (const auto& operation : operations) check(bool(core::applyDocumentOperation(document, operation)), "captured UI operation did not replay");
    return document;
}
void equivalent(const core::Document& actual, const core::Document& expected) {
    check(actual.paragraphs().size() == expected.paragraphs().size(), "list planner changed paragraph count");
    for (std::size_t index = 0; index < actual.paragraphs().size(); ++index) {
        const auto& left = actual.paragraphs()[index];
        const auto& right = expected.paragraphs()[index];
        auto leftFormat = left.format(), rightFormat = right.format();
        leftFormat.list_id.reset(); rightFormat.list_id.reset();
        check(left.id() == right.id() && left.text() == right.text() && leftFormat == rightFormat &&
            left.characterFormats() == right.characterFormats() && left.paragraphMarkCharacterFormat() == right.paragraphMarkCharacterFormat() && left.styleId() == right.styleId() && left.styleProvenance() == right.styleProvenance(),
            "planned list differs from direct UI text/format/provenance behavior");
    }
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ListPlanning"));
    app::SpellChecker spelling;
    app::DocumentCanvas ui(spelling), planner(spelling);
    ui.setEditorDefaults(QStringLiteral("DejaVu Sans"), 11, 4);
    planner.setEditorDefaults(QStringLiteral("DejaVu Sans"), 11, 4);
    ui.insertText(QStringLiteral("First\nSecond\nThird"));
    const auto original = ui.snapshot().document;
    const auto plannerBefore = planner.snapshot();
    app::ListEditRequest request;
    for (const auto& paragraph : original.paragraphs()) request.blockIds.push_back(paragraph.id());
    std::vector<core::Operation> operations;
    QString error;
    check(planner.planListOperations(original, request, operations, error) && !operations.empty(), "list creation planning failed");
    auto proposed = replayPlan(original, operations);
    ui.selectAll(); ui.toggleNumbering();
    equivalent(proposed, ui.snapshot().document);
    check(planner.snapshot().revision == plannerBefore.revision && planner.snapshot().document == plannerBefore.document && original.paragraphs().front().text() == u"First", "planning mutated the visible canvas or input snapshot");
    const auto listId = proposed.paragraphs().front().format().list_id;
    check(listId && proposed.paragraphs()[1].format().list_id == listId && proposed.paragraphs()[2].format().list_id == listId, "planned list did not share one semantic identity");
    operations.clear();
    check(planner.planListOperations(proposed, request, operations, error) && operations.empty(), "setting an existing list kind toggled it off or recreated its identity");
    const auto middle = original.paragraphs()[1].id();
    request.kind = app::ListEditRequest::Kind::set_level;
    request.blockIds = {middle}; request.level = 2;
    check(planner.planListOperations(proposed, request, operations, error) && !operations.empty(), "absolute list level planning failed");
    proposed = replayPlan(proposed, operations);
    check(ui.navigateToParagraph(middle) && ui.changeListLevel(false), "direct level UI fixture");
    equivalent(proposed, ui.snapshot().document);
    request.kind = app::ListEditRequest::Kind::set_layout;
    request.layout.levels[1] = {5, 3}; operations.clear();
    check(planner.planListOperations(proposed, request, operations, error), "per-list layout planning failed");
    proposed = replayPlan(proposed, operations);
    check(ui.setCurrentListLayout(request.layout), "direct list layout UI fixture");
    equivalent(proposed, ui.snapshot().document);
    request.kind = app::ListEditRequest::Kind::set_kind;
    request.listKind = QStringLiteral("none"); operations.clear();
    check(planner.planListOperations(proposed, request, operations, error), "list removal planning failed");
    proposed = replayPlan(proposed, operations);
    ui.toggleNumbering();
    equivalent(proposed, ui.snapshot().document);
    request.blockIds.clear();
    for (const auto& paragraph : original.paragraphs()) request.blockIds.push_back(paragraph.id());
    request.listKind = QStringLiteral("bullets"); operations.clear();
    check(planner.planListOperations(proposed, request, operations, error), "bullet conversion planning failed");
    proposed = replayPlan(proposed, operations);
    ui.selectAll(); ui.toggleBullets();
    equivalent(proposed, ui.snapshot().document);
    request.kind = app::ListEditRequest::Kind::refresh_numbering;
    request.blockIds.clear(); operations.clear();
    check(planner.planListOperations(proposed, request, operations, error) && operations.empty(), "refreshing numbers modified or rejected bullet lists");
    auto literal = core::Paragraph::create(u"1. ordinary literal prose");
    check(bool(literal), "literal paragraph fixture");
    auto literalDocument = core::Document::create({std::move(literal.value())});
    check(bool(literalDocument) && planner.planListOperations(literalDocument.value(), request, operations, error) && operations.empty() && !literalDocument.value().paragraphs().front().format().list_id,
        "refresh adopted unrelated literal numbering into semantic lists");
    request.kind = app::ListEditRequest::Kind::set_kind;
    request.blockIds = {original.paragraphs().front().id(), original.paragraphs().back().id()};
    check(!planner.planListOperations(original, request, operations, error) && operations.empty(), "noncontiguous list targets were accepted");
    app::DocumentCanvas reordered(spelling);
    reordered.insertText(QStringLiteral("First\nSecond\nThird"));
    reordered.selectAll(); reordered.toggleNumbering();
    const auto numbered = reordered.snapshot();
    const auto first = numbered.document.paragraphs()[0].id();
    const auto second = numbered.document.paragraphs()[1].id();
    QString summary;
    const auto preview = app::invokeEditorTool(reordered, QStringLiteral("reorder"), QStringLiteral("editor_v1_preview"),
        {{"documentId", "reorder"}, {"baseRevision", numbered.revision.value()}, {"summary", "Reorder numbered items"},
            {"operations", codex::Json::array({{{"kind", "move_block"}, {"blockId", second.toString()}, {"beforeBlockId", first.toString()}}})}}, summary, error);
    check(error.isEmpty() && !preview.empty() && reordered.acceptPreview(error), "move numbered paragraph through agent preview");
    const auto moved = reordered.snapshot();
    check(moved.document.paragraphs()[0].id() == second && moved.document.paragraphs()[0].text().starts_with(u"1.") &&
              moved.document.paragraphs()[1].id() == first && moved.document.paragraphs()[1].text().starts_with(u"2."),
          "agent structural edits leave numbered lists in their previous order");
    reordered.undo();
    check(reordered.snapshot().document == numbered.document, "numbered paragraph move and resequencing are not one Undo");
    std::cout << "List operation planning tests passed\n";
}
