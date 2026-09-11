#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"

#include <QApplication>
#include <QStringList>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString paragraphText(const docxstudio::app::DocumentCanvas& canvas) {
    const auto snapshot = canvas.snapshot();
    const auto& text = snapshot.document.paragraphs().front().text();
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

QStringList paragraphTexts(const docxstudio::app::DocumentCanvas& canvas) {
    QStringList result;
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(QString::fromUtf16(
            paragraph.text().data(),
            static_cast<qsizetype>(paragraph.text().size())));
    }
    return result;
}

docxstudio::core::Document formattedBoundaryDocument() {
    auto paragraph = docxstudio::core::Paragraph::create(u"Ablue");
    check(static_cast<bool>(paragraph), "could not create formatted bridge paragraph");
    const auto id = paragraph.value().id();
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create formatted bridge document");
    docxstudio::core::CharacterFormatDelta delta;
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xff336699U);
    delta.highlight_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xffffcc00U);
    delta.baseline = docxstudio::core::PropertyDelta<
        docxstudio::core::BaselinePosition>::set(
            docxstudio::core::BaselinePosition::superscript);
    const auto result = document.value().applyCharacterFormat(
        {{id, 1}, {id, 5}}, delta);
    check(static_cast<bool>(result), "could not format bridge test range");
    return std::move(document.value());
}

docxstudio::core::Document shiftedFormattingDocument() {
    auto paragraph = docxstudio::core::Paragraph::create(u"AABBBB");
    check(static_cast<bool>(paragraph), "could not create shifted-format paragraph");
    const auto id = paragraph.value().id();
    auto document = docxstudio::core::Document::create(
        std::vector<docxstudio::core::Paragraph>{std::move(paragraph.value())});
    check(static_cast<bool>(document), "could not create shifted-format document");
    docxstudio::core::CharacterFormatDelta delta;
    delta.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xff336699U);
    const auto result = document.value().applyCharacterFormat(
        {{id, 2}, {id, 6}}, delta);
    check(static_cast<bool>(result), "could not format shifted test range");
    return std::move(document.value());
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("Helo world"));
    canvas.markSaved();

    const QString documentId = QStringLiteral("document:test");
    const auto before = canvas.snapshot();
    const std::string blockId = before.document.paragraphs().front().id().toString();
    QString summary;
    QString error;

    const auto read = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", before.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    check(error.isEmpty(), "bounded document read failed");
    check(read.at("revision") == before.revision.value(), "read returned wrong revision");
    check(read.at("blocks").size() == 1, "read returned wrong block count");
    check(read.at("blocks").at(0).at("text") == "Helo world", "read returned wrong text");
    check(read.at("blocks").at(0).at("id") == blockId, "read returned unstable block id");

    docxstudio::app::DocumentCanvas formattingCanvas(spelling);
    formattingCanvas.setDocument(formattedBoundaryDocument());
    const auto formattingBase = formattingCanvas.snapshot();
    const std::string formattingBlockId =
        formattingBase.document.paragraphs().front().id().toString();
    const auto formattingRead = docxstudio::app::invokeEditorTool(
        formattingCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    check(error.isEmpty(), "formatted Codex read failed");
    const auto& style = formattingRead.at("blocks").at(0)
                            .at("attributes").at("characterRuns").at(0)
                            .at("style");
    check(style.at("foregroundColor") == "#336699" &&
              style.at("highlightColor") == "#FFCC00" &&
              style.at("verticalAlign") == "superscript",
          "Codex read omitted color, highlight, or vertical alignment");

    const auto colorReplacement = docxstudio::app::invokeEditorTool(
        formattingCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", formattingBase.revision.value()},
         {"summary", "Replace the colored word"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", formattingBlockId}, {"start", 1}, {"end", 5}}},
            {"text", "navy"}}}}},
        summary, error);
    check(error.isEmpty() && !colorReplacement.empty(),
          "Codex could not preview a colored boundary replacement");
    check(formattingCanvas.acceptPreview(error),
          "Codex colored boundary preview could not be accepted");
    check(paragraphText(formattingCanvas) == QStringLiteral("Anavy"),
          "Codex colored boundary replacement produced wrong text");
    const auto replacedFormat = formattingCanvas.snapshot().document.paragraphs()
                                    .front().characterFormatAt(2);
    check(replacedFormat.foreground_argb == 0xff336699U &&
              replacedFormat.highlight_argb == 0xffffcc00U &&
              replacedFormat.baseline ==
                  docxstudio::core::BaselinePosition::superscript,
          "Codex replace_text inherited the preceding black run");
    check(!formattingCanvas.hasNonTextChanges(),
          "format-preserving Codex replacement was classified as a format edit");

    docxstudio::app::DocumentCanvas sequentialCanvas(spelling);
    sequentialCanvas.setDocument(shiftedFormattingDocument());
    const auto sequentialBase = sequentialCanvas.snapshot();
    const std::string sequentialBlockId =
        sequentialBase.document.paragraphs().front().id().toString();
    const auto sequentialPreview = docxstudio::app::invokeEditorTool(
        sequentialCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", sequentialBase.revision.value()},
         {"summary", "Apply sequential replacements"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", sequentialBlockId}, {"start", 0}, {"end", 2}}},
            {"text", "AAAA"}},
           {{"kind", "replace_text"},
            {"target", {{"blockId", sequentialBlockId}, {"start", 4}, {"end", 8}}},
            {"text", "X"}}}}},
        summary, error);
    check(error.isEmpty() && !sequentialPreview.empty(),
          "sequential shifted-offset preview was rejected");
    check(sequentialCanvas.acceptPreview(error),
          "sequential shifted-offset preview could not be accepted");
    check(paragraphText(sequentialCanvas) == QStringLiteral("AAAAX"),
          "sequential shifted-offset replacement produced wrong text");
    check(sequentialCanvas.snapshot().document.paragraphs().front()
              .characterFormatAt(5).foreground_argb == 0xff336699U,
          "sequential replacement resolved formatting against the stale base");
    check(!sequentialCanvas.hasNonTextChanges(),
          "format-preserving sequential replacements were classified as format edits");

    docxstudio::app::DocumentCanvas pageBreakCanvas(spelling);
    pageBreakCanvas.insertText(QStringLiteral("abcdef"));
    pageBreakCanvas.markSaved();
    const auto pageBreakBase = pageBreakCanvas.snapshot();
    const std::string pageBreakBlockId =
        pageBreakBase.document.paragraphs().front().id().toString();
    const auto pageBreakPreview = docxstudio::app::invokeEditorTool(
        pageBreakCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", pageBreakBase.revision.value()},
         {"summary", "Insert a page break in the paragraph"},
         {"operations",
          {{{"kind", "insert_page_break"},
            {"target", {{"blockId", pageBreakBlockId}, {"start", 3}}}}}}},
        summary, error);
    check(error.isEmpty() && !pageBreakPreview.empty(),
          "mid-paragraph page-break preview was rejected");
    check(pageBreakCanvas.acceptPreview(error),
          "mid-paragraph page-break preview could not be accepted");
    check(paragraphTexts(pageBreakCanvas) ==
              QStringList{QStringLiteral("abc"), QStringLiteral("def")},
          "Codex page break did not split at the requested UTF-16 offset");
    check(pageBreakCanvas.snapshot().document.paragraphs().back()
              .format().page_break_before == true,
          "Codex page break did not format the split-off paragraph");
    pageBreakCanvas.undo();
    check(paragraphTexts(pageBreakCanvas) == QStringList{QStringLiteral("abcdef")},
          "Codex page break was not one undo transaction");

    const auto unsupportedScope = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "viewport"}},
        summary, error);
    check(unsupportedScope.empty(),
          "unsupported viewport scope fell through to a document read");
    check(error.contains(QStringLiteral("Unsupported editor read scope")),
          "unsupported viewport scope did not return a clear error");

    docxstudio::app::DocumentCanvas unicodeCanvas(spelling);
    unicodeCanvas.insertText(QString::fromUtf8("A😀BC"));
    unicodeCanvas.selectAll();
    unicodeCanvas.toggleBold();
    const auto truncatedRead = docxstudio::app::invokeEditorTool(
        unicodeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "selection"},
         {"includeFormatting", true},
         {"maxCharacters", 2}},
        summary, error);
    check(error.isEmpty(), "UTF-16 boundary read failed");
    check(truncatedRead.at("truncated") == true,
          "bounded UTF-16 read was not marked truncated");
    const auto& truncatedBlock = truncatedRead.at("blocks").at(0);
    check(truncatedBlock.at("text") == "A",
          "bounded read split a UTF-16 surrogate pair");
    check(truncatedBlock.at("attributes").at("characterRuns").at(0).at("end") == 1,
          "formatting range was not clipped to returned text");
    check(truncatedBlock.at("attributes").at("selectionStart") == 0 &&
              truncatedBlock.at("attributes").at("selectionEnd") == 1,
          "selection range was not clipped to returned text");

    const auto preview = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", before.revision.value()},
         {"summary", "Correct spelling and emphasize the greeting"},
         {"operations",
          {{{"kind", "replace_text"},
            {"target", {{"blockId", blockId}, {"start", 0}, {"end", 4}}},
            {"text", "Hello"}},
           {{"kind", "set_text_style"},
            {"target", {{"blockId", blockId}, {"start", 0}, {"end", 5}}},
            {"style", {{"bold", true}}}}}}},
        summary, error);
    check(error.isEmpty(), "semantic preview creation failed");
    check(preview.at("committed") == false, "tool committed a preview");
    check(!preview.at("previewId").get<std::string>().empty(), "preview has no id");
    check(canvas.hasPreview(), "canvas did not retain the preview branch");
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "preview changed the authoritative document");
    check(!canvas.isModified(), "preview marked the live document modified");
    check(!canvas.hasNonTextChanges(),
          "preview marked the live document as having formatting changes");

    check(canvas.acceptPreview(error), "preview could not be accepted");
    check(paragraphText(canvas) == QStringLiteral("Hello world"),
          "accepted semantic preview has wrong text");
    check(canvas.snapshot().document.paragraphs().front()
              .characterFormatAt(1).bold.value_or(false),
          "accepted semantic preview lost formatting");
    check(canvas.hasNonTextChanges(),
          "accepted formatting preview was not recorded as a non-text change");
    canvas.undo();
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "semantic preview was not one undo transaction");
    canvas.markSaved();

    const auto equationBase = canvas.snapshot();
    const auto equation = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", equationBase.revision.value()},
         {"summary", "Insert a fraction"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", "\\frac{ a }{b}"},
            {"display", true}}}}},
        summary, error);
    check(error.isEmpty(), "safe equation preview was rejected");
    check(!equation.empty(), "safe equation preview was not created");
    check(equation.at("warnings").empty(),
          "semantic equation preview returned a stale marker warning");
    check(canvas.hasPreview(), "semantic equation preview was not retained");
    check(paragraphText(canvas) == QStringLiteral("Helo world"),
          "semantic equation preview changed the authoritative document");
    check(canvas.acceptPreview(error), "safe equation preview could not be accepted");
    const auto equationSnapshot = canvas.snapshot();
    const auto& equationParagraph = equationSnapshot.document.paragraphs().front();
    check(equationParagraph.text().front() ==
              docxstudio::core::kInlineObjectReplacementCharacter,
          "accepted equation did not occupy one semantic inline-object position");
    check(equationParagraph.equations().size() == 1,
          "accepted equation has no semantic atom");
    const auto& equationAtom = equationParagraph.equations().front();
    check(equationAtom.utf16_offset == 0 &&
              equationAtom.canonical_latex == "\\frac{a}{b}" &&
              equationAtom.display,
          "equation source was not canonicalized or its display flag was lost");
    check(canvas.hasNonTextChanges(),
          "semantic equation preview was not classified as a non-text change");

    const auto equationRead = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"scope", "document"},
         {"includeFormatting", false}},
        summary, error);
    check(error.isEmpty(), "semantic equation read failed");
    const auto& equationBlock = equationRead.at("blocks").at(0);
    const QString equationText =
        QString::fromStdString(equationBlock.at("text").get<std::string>());
    check(equationText.size() == QStringLiteral("Helo world").size() + 1 &&
              equationText.front().unicode() ==
                  docxstudio::core::kInlineObjectReplacementCharacter,
          "equation read did not retain offset-safe U+FFFC text");
    const auto& equationMetadata =
        equationBlock.at("attributes").at("equations");
    check(equationMetadata.size() == 1,
          "equation read omitted semantic metadata");
    check(equationMetadata.at(0).at("id") == equationAtom.id.toString() &&
              equationMetadata.at(0).at("utf16Offset") == 0 &&
              equationMetadata.at(0).at("canonicalLatex") ==
                  "\\frac{a}{b}" &&
              equationMetadata.at(0).at("display") == true,
          "equation read returned incorrect semantic metadata");
    canvas.undo();
    check(paragraphText(canvas) == QStringLiteral("Helo world") &&
              canvas.snapshot().document.paragraphs().front().equations().empty(),
          "semantic equation insertion was not one undo transaction");

    const auto oversized = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Oversized equation"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", std::string(8U * 1024U + 1U, 'x')}}}}},
        summary, error);
    check(oversized.empty(), "oversized LaTeX equation unexpectedly succeeded");
    check(error.contains(QStringLiteral("exceeds"), Qt::CaseInsensitive),
          "oversized LaTeX equation did not return a bounded-input error");

    const auto forbidden = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Unsafe equation"},
         {"operations",
          {{{"kind", "insert_equation"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"latex", "\\input{/etc/passwd}"}}}}},
        summary, error);
    check(forbidden.empty(), "forbidden LaTeX command unexpectedly succeeded");
    check(error.contains(QStringLiteral("Forbidden"), Qt::CaseInsensitive),
          "forbidden LaTeX command did not return a useful error");

    const auto stale = docxstudio::app::invokeEditorTool(
        canvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data(),
                            static_cast<qsizetype>(docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", documentId.toStdString()},
         {"baseRevision", before.revision.value()},
         {"summary", "Stale edit"},
         {"operations",
          {{{"kind", "insert_text"},
            {"target", {{"blockId", blockId}, {"start", 0}}},
            {"text", "X"}}}}},
        summary, error);
    check(stale.empty(), "stale preview unexpectedly succeeded");
    check(error.contains(QStringLiteral("REVISION_CONFLICT")),
          "stale preview did not report a revision conflict");

    std::cout << "editor tool bridge tests passed\n";
    return 0;
}
