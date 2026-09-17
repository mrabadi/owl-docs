#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QStringList>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
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

std::vector<std::uint8_t> bridgePng() {
    QImage image(7, 5, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#336699")));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not encode bridge image fixture");
    return {
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size()};
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

    docxstudio::app::DocumentCanvas emptyFormatCanvas(spelling);
    emptyFormatCanvas.setFontFamily(QStringLiteral("DejaVu Serif"));
    emptyFormatCanvas.setFontPointSize(19.0);
    emptyFormatCanvas.setForeground(QColor(QStringLiteral("#3157a4")));
    emptyFormatCanvas.toggleBold();
    const auto emptyFormatSnapshot = emptyFormatCanvas.snapshot();
    error.clear();
    const auto emptyFormatRead = docxstudio::app::invokeEditorTool(
        emptyFormatCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", emptyFormatSnapshot.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    const auto& paragraphMarkStyle = emptyFormatRead.at("blocks")
                                         .at(0)
                                         .at("attributes")
                                         .at("paragraphMarkStyle");
    check(error.isEmpty() &&
              emptyFormatRead.at("blocks").at(0).at("text") == "" &&
              paragraphMarkStyle.at("fontFamily") == "DejaVu Serif" &&
              paragraphMarkStyle.at("fontSizePoints") == 19.0 &&
              paragraphMarkStyle.at("foregroundColor") == "#3157A4" &&
              paragraphMarkStyle.at("bold") == true,
          "editor read omitted durable empty-paragraph insertion formatting");

    const QString readTool = QString::fromLatin1(
        docxstudio::codex::kEditorReadTool.data(),
        static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size()));
    const auto expectRejectedRead =
        [&](docxstudio::codex::Json arguments, const QString& expectedError,
            const char* failureMessage) {
            arguments["documentId"] = documentId.toStdString();
            const auto rejected = docxstudio::app::invokeEditorTool(
                canvas, documentId, readTool, arguments, summary, error);
            check(rejected.empty() && error.contains(expectedError),
                  failureMessage);
        };
    expectRejectedRead(
        {{"scope", "document"}, {"expectedRevision", "zero"}},
        QStringLiteral("expectedRevision"),
        "read silently ignored a malformed expected revision");
    expectRejectedRead(
        {{"scope", 7}}, QStringLiteral("scope"),
        "read accepted a non-string scope");
    expectRejectedRead(
        {{"scope", "document"}, {"includeFormatting", "yes"}},
        QStringLiteral("includeFormatting"),
        "read accepted a non-boolean formatting flag");
    expectRejectedRead(
        {{"scope", "document"}, {"maxCharacters", 0}},
        QStringLiteral("maxCharacters"),
        "read silently replaced a zero character limit");
    expectRejectedRead(
        {{"scope", "document"}, {"maxCharacters", 200001}},
        QStringLiteral("maxCharacters"),
        "read silently clamped an oversized character limit");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", "not-an-array"}},
        QStringLiteral("blockIds"),
        "read silently ignored a malformed blockIds value");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", {42}}},
        QStringLiteral("blockIds"),
        "read silently ignored a non-string block ID");
    expectRejectedRead(
        {{"scope", "blocks"}, {"blockIds", {"not-a-node-id"}}},
        QStringLiteral("invalid stable ID"),
        "read silently ignored an invalid stable block ID");

    const auto toolDefinitions =
        docxstudio::codex::editorV1ToolDefinitions();
    const auto previewDefinition = std::find_if(
        toolDefinitions.begin(), toolDefinitions.end(), [](const auto& tool) {
            return tool.name == docxstudio::codex::kEditorPreviewTool;
        });
    check(previewDefinition != toolDefinitions.end(),
          "editor tool catalog omitted the preview tool");
    const auto& styleIdSchema = previewDefinition->inputSchema
                                    .at("$defs")
                                    .at("paragraphStyle")
                                    .at("properties")
                                    .at("styleId");
    check(styleIdSchema.at("oneOf").size() == 2 &&
              styleIdSchema.at("oneOf").at(0).at("maxLength") == 1024 &&
              styleIdSchema.at("oneOf").at(1).at("type") == "null" &&
              docxstudio::codex::kEditorToolCatalogVersion ==
                  "editor.v1.catalog.6",
          "editor tool schema does not advertise bounded style set/clear");
    check(previewDefinition->inputSchema.dump().find(
              "insert_excalidraw_figure") != std::string::npos &&
              previewDefinition->inputSchema.dump().find(
              "replace_excalidraw_figure") != std::string::npos,
          "editor tool schema omitted editable Excalidraw operations");
    check(previewDefinition->inputSchema.dump().find("insert_image") ==
              std::string::npos,
          "editor tool schema advertises unavailable image insertion");

    auto headingParagraph = docxstudio::core::Paragraph::restore(
        u"Built-in heading", docxstudio::core::NodeId{701, 702}, {},
        std::string("Heading2"));
    auto customStyleParagraph = docxstudio::core::Paragraph::restore(
        u"Imported custom", docxstudio::core::NodeId{703, 704}, {},
        std::string("Firm.Custom-β"));
    auto plainParagraph = docxstudio::core::Paragraph::restore(
        u"Plain paragraph", docxstudio::core::NodeId{705, 706}, {});
    check(headingParagraph && customStyleParagraph && plainParagraph,
          "could not create editor style-read paragraphs");
    auto styleReadDocument = docxstudio::core::Document::create(
        {std::move(headingParagraph.value()),
         std::move(customStyleParagraph.value()),
         std::move(plainParagraph.value())});
    check(static_cast<bool>(styleReadDocument),
          "could not create editor style-read document");
    docxstudio::app::DocumentCanvas styleReadCanvas(spelling);
    styleReadCanvas.setDocument(std::move(styleReadDocument.value()));
    const auto styleReadSnapshot = styleReadCanvas.snapshot();
    const auto styleRead = docxstudio::app::invokeEditorTool(
        styleReadCanvas, documentId, readTool,
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", styleReadSnapshot.revision.value()},
         {"scope", "document"},
         {"includeFormatting", false}},
        summary, error);
    check(error.isEmpty() && styleRead.at("blocks").size() == 3,
          "semantic style document read failed");
    const auto& headingAttributes =
        styleRead.at("blocks").at(0).at("attributes");
    const auto& customAttributes =
        styleRead.at("blocks").at(1).at("attributes");
    const auto& plainAttributes =
        styleRead.at("blocks").at(2).at("attributes");
    check(headingAttributes.at("styleId") == "Heading2" &&
              headingAttributes.at("styleDisplayName") == "Heading 2" &&
              headingAttributes.at("outlineLevel") == 1,
          "editor read omitted recognized paragraph-style metadata");
    check(customAttributes.at("styleId") == "Firm.Custom-β" &&
              !customAttributes.contains("styleDisplayName") &&
              !customAttributes.contains("outlineLevel"),
          "editor read remapped or embellished an unknown custom style ID");
    check(!plainAttributes.contains("styleId") &&
              !plainAttributes.contains("styleDisplayName") &&
              !plainAttributes.contains("outlineLevel"),
          "editor read invented style metadata for an unstyled paragraph");

    const auto outlineStyleRead = docxstudio::app::invokeEditorTool(
        styleReadCanvas, documentId, readTool,
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", styleReadSnapshot.revision.value()},
         {"scope", "outline"},
         {"includeFormatting", false}},
        summary, error);
    check(error.isEmpty() &&
              outlineStyleRead.at("blocks").at(0).at("attributes")
                  .at("styleId") == "Heading2" &&
              outlineStyleRead.at("blocks").at(0).at("attributes")
                  .at("outlineLevel") == 1 &&
              outlineStyleRead.at("blocks").at(1).at("attributes")
                  .at("styleId") == "Firm.Custom-β",
          "outline read omitted semantic paragraph-style metadata");

    const QString previewTool = QString::fromLatin1(
        docxstudio::codex::kEditorPreviewTool.data(),
        static_cast<qsizetype>(
            docxstudio::codex::kEditorPreviewTool.size()));
    docxstudio::app::DocumentCanvas styleMutationCanvas(spelling);
    styleMutationCanvas.insertText(QStringLiteral("Heading candidate"));
    styleMutationCanvas.markSaved();
    const auto styleBase = styleMutationCanvas.snapshot();
    const auto styleBlockId =
        styleBase.document.paragraphs().front().id().toString();
    const auto headingPreview = docxstudio::app::invokeEditorTool(
        styleMutationCanvas, documentId, previewTool,
        {{"documentId", documentId.toStdString()},
         {"baseRevision", styleBase.revision.value()},
         {"summary", "Apply Heading 1"},
         {"operations",
          {{{"kind", "set_paragraph_style"},
            {"target", {{"blockId", styleBlockId}, {"start", 0}}},
            {"style",
             {{"styleId", "Heading1"}, {"alignment", "right"}}}}}}},
        summary, error);
    check(error.isEmpty() && !headingPreview.empty() &&
              styleMutationCanvas.hasPreview(),
          "recognized paragraph style preview was rejected");
    const auto beforeHeadingAccept = styleMutationCanvas.snapshot();
    check(beforeHeadingAccept.revision == styleBase.revision &&
              !beforeHeadingAccept.document.paragraphs().front().styleId() &&
              beforeHeadingAccept.document.paragraphs().front()
                  .format().empty(),
          "paragraph style preview changed the authoritative document");
    check(styleMutationCanvas.acceptPreview(error),
          "recognized paragraph style preview could not be accepted");
    const auto styledSnapshot = styleMutationCanvas.snapshot();
    const auto& styledParagraph = styledSnapshot.document.paragraphs().front();
    check(styledSnapshot.revision.value() == styleBase.revision.value() + 1 &&
              styledParagraph.styleId() ==
                  std::optional<std::string>("Heading1") &&
              styledParagraph.format().alignment ==
                  docxstudio::core::ParagraphAlignment::right &&
              styledParagraph.format().space_before_emu == 152400 &&
              styledParagraph.format().space_after_emu == 76200 &&
              styledParagraph.characterFormatAt(1).bold == true &&
              styledParagraph.characterFormatAt(1)
                      .font_size_half_points == 32 &&
              styledParagraph.paragraphMarkCharacterFormat().bold == true &&
              styledParagraph.styleProvenance() &&
              styledParagraph.styleProvenance()
                  ->paragraph_overrides.alignment &&
              styleMutationCanvas.hasNonTextChanges(),
          "recognized paragraph style did not apply identity, baseline, and explicit override atomically");
    styleMutationCanvas.undo();
    const auto headingUndone = styleMutationCanvas.snapshot();
    check(!headingUndone.document.paragraphs().front().styleId() &&
              headingUndone.document.paragraphs().front().format().empty() &&
              headingUndone.document.paragraphs().front()
                  .characterFormats().empty() &&
              headingUndone.document.paragraphs().front()
                  .paragraphMarkCharacterFormat().empty(),
          "paragraph style acceptance was not one undo transaction");

    const auto formattingOnlyBase = styleMutationCanvas.snapshot();
    const auto formattingOnlyPreview =
        docxstudio::app::invokeEditorTool(
            styleMutationCanvas, documentId, previewTool,
            {{"documentId", documentId.toStdString()},
             {"baseRevision", formattingOnlyBase.revision.value()},
             {"summary", "Center the paragraph"},
             {"operations",
              {{{"kind", "set_paragraph_style"},
                {"target", {{"blockId", styleBlockId}, {"start", 0}}},
                {"style", {{"alignment", "center"}}}}}}},
            summary, error);
    check(error.isEmpty() && !formattingOnlyPreview.empty() &&
              styleMutationCanvas.acceptPreview(error),
          "legacy formatting-only paragraph-style operation regressed");
    const auto formattingOnlyApplied = styleMutationCanvas.snapshot();
    check(formattingOnlyApplied.document.paragraphs().front().styleId() ==
                  std::optional<std::string>{"Normal"} &&
              formattingOnlyApplied.document.paragraphs().front()
                  .styleProvenance() &&
              formattingOnlyApplied.document.paragraphs().front()
                      .format().alignment ==
                  docxstudio::core::ParagraphAlignment::center,
          "formatting-only paragraph style did not atomically initialize Normal provenance");
    styleMutationCanvas.undo();
    check(!styleMutationCanvas.snapshot().document.paragraphs().front()
                   .styleId() &&
              !styleMutationCanvas.snapshot().document.paragraphs().front()
                   .styleProvenance(),
          "undo did not restore the implicit Normal paragraph identity");

    const auto customBase = styleMutationCanvas.snapshot();
    const auto customPreview = docxstudio::app::invokeEditorTool(
        styleMutationCanvas, documentId, previewTool,
        {{"documentId", documentId.toStdString()},
         {"baseRevision", customBase.revision.value()},
         {"summary", "Retain an imported custom style"},
         {"operations",
          {{{"kind", "set_paragraph_style"},
            {"target", {{"blockId", styleBlockId}, {"start", 0}}},
            {"style", {{"styleId", "Firm.Custom-β"}}}}}}},
        summary, error);
    check(error.isEmpty() && !customPreview.empty() &&
              styleMutationCanvas.acceptPreview(error),
          "unknown custom paragraph-style identity was rejected");
    const auto customStyled = styleMutationCanvas.snapshot();
    check(customStyled.document.paragraphs().front().styleId() ==
              std::optional<std::string>("Firm.Custom-β") &&
              customStyled.document.paragraphs().front().format().empty() &&
              customStyled.document.paragraphs().front()
                  .characterFormats().empty(),
          "unknown custom style ID was remapped or given invented formatting");

    const auto clearBase = styleMutationCanvas.snapshot();
    const auto clearPreview = docxstudio::app::invokeEditorTool(
        styleMutationCanvas, documentId, previewTool,
        {{"documentId", documentId.toStdString()},
         {"baseRevision", clearBase.revision.value()},
         {"summary", "Clear the paragraph style identity"},
         {"operations",
          {{{"kind", "set_paragraph_style"},
            {"target", {{"blockId", styleBlockId}, {"start", 0}}},
            {"style", {{"styleId", nullptr}}}}}}},
        summary, error);
    check(error.isEmpty() && !clearPreview.empty() &&
              styleMutationCanvas.snapshot().document.paragraphs().front()
                      .styleId() ==
                  std::optional<std::string>("Firm.Custom-β") &&
              styleMutationCanvas.acceptPreview(error) &&
              !styleMutationCanvas.snapshot().document.paragraphs().front()
                   .styleId(),
          "paragraph-style clear was not previewed and applied safely");
    styleMutationCanvas.undo();
    check(styleMutationCanvas.snapshot().document.paragraphs().front()
              .styleId() ==
              std::optional<std::string>("Firm.Custom-β"),
          "paragraph-style clear was not one undo transaction");

    const auto expectRejectedStyleId =
        [&](const docxstudio::codex::Json& styleId,
            const QString& expectedError, const char* failureMessage) {
            const auto rejected = docxstudio::app::invokeEditorTool(
                styleMutationCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision",
                  styleMutationCanvas.snapshot().revision.value()},
                 {"summary", "Reject malformed paragraph style"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", styleBlockId}, {"start", 0}}},
                    {"style", {{"styleId", styleId}}}}}}},
                summary, error);
            check(rejected.empty() && error.contains(expectedError),
                  failureMessage);
        };
    expectRejectedStyleId(
        "", QStringLiteral("cannot be empty"),
        "editor preview accepted an empty paragraph-style ID");
    expectRejectedStyleId(
        "Heading\n1", QStringLiteral("valid visible UTF-8"),
        "editor preview accepted a control character in a paragraph-style ID");
    expectRejectedStyleId(
        std::string(docxstudio::core::kMaximumParagraphStyleIdBytes + 1U,
                    'x'),
        QStringLiteral("size limit"),
        "editor preview accepted an oversized paragraph-style ID");
    expectRejectedStyleId(
        42, QStringLiteral("string or null"),
        "editor preview accepted a non-string paragraph-style ID");

    const auto expectRejectedParagraphStyle =
        [&](docxstudio::codex::Json style, const QString& expectedError,
            const char* failureMessage) {
            error.clear();
            const auto beforeRejection = styleMutationCanvas.snapshot();
            const auto rejected = docxstudio::app::invokeEditorTool(
                styleMutationCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", beforeRejection.revision.value()},
                 {"summary", "Reject malformed paragraph formatting"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", styleBlockId}, {"start", 0}}},
                    {"style", std::move(style)}}}}},
                summary, error);
            check(rejected.empty() && error.contains(expectedError) &&
                      !styleMutationCanvas.hasPreview() &&
                      styleMutationCanvas.snapshot().revision ==
                          beforeRejection.revision,
                  failureMessage);
        };
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"mysteryProperty", true}},
        QStringLiteral("unsupported property"),
        "editor preview accepted an unknown paragraph-style property");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"alignment", 7}},
        QStringLiteral("alignment must be a string"),
        "editor preview accepted a non-string paragraph alignment");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"alignment", "distributed"}},
        QStringLiteral("left, center, right, or justify"),
        "editor preview mapped an unknown paragraph alignment to left");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"lineSpacing", "single"}},
        QStringLiteral("lineSpacing must be a number"),
        "editor preview ignored a non-number line spacing");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"},
         {"lineSpacing", std::numeric_limits<double>::quiet_NaN()}},
        QStringLiteral("lineSpacing is outside"),
        "editor preview accepted non-finite line spacing");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"lineSpacing", 20.01}},
        QStringLiteral("lineSpacing is outside"),
        "editor preview accepted line spacing above its schema maximum");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"spaceBeforePoints", -0.5}},
        QStringLiteral("spaceBeforePoints is outside"),
        "editor preview accepted negative space-before formatting");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"spaceBeforePoints", 10000.01}},
        QStringLiteral("spaceBeforePoints is outside"),
        "editor preview accepted space-before above its schema maximum");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"},
         {"spaceAfterPoints", std::numeric_limits<double>::infinity()}},
        QStringLiteral("spaceAfterPoints is outside"),
        "editor preview accepted non-finite space-after formatting");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"spaceAfterPoints", "12"}},
        QStringLiteral("spaceAfterPoints must be a number"),
        "editor preview ignored a non-number space-after value");
    expectRejectedParagraphStyle(
        {{"styleId", "Heading1"}, {"keepWithNext", 0}},
        QStringLiteral("keepWithNext must be a boolean"),
        "editor preview ignored a non-boolean keep-with-next value");

    {
        docxstudio::app::DocumentCanvas implicitNormalCanvas(spelling);
        implicitNormalCanvas.insertText(QStringLiteral("Implicit Normal"));
        const auto implicitBase = implicitNormalCanvas.snapshot();
        const auto implicitBlockId =
            implicitBase.document.paragraphs().front().id().toString();
        check(!implicitBase.document.paragraphs().front().styleId() &&
                  !implicitBase.document.paragraphs().front()
                       .styleProvenance(),
              "implicit-Normal Codex fixture unexpectedly had style provenance");

        const auto explicitNormalPreview =
            docxstudio::app::invokeEditorTool(
                implicitNormalCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", implicitBase.revision.value()},
                 {"summary", "Assign explicit Normal identity"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", implicitBlockId}, {"start", 0}}},
                    {"style", {{"styleId", "Normal"}}}}}}},
                summary, error);
        check(error.isEmpty() && !explicitNormalPreview.empty() &&
                  implicitNormalCanvas.acceptPreview(error),
              "Codex could not assign explicit Normal identity");
        const auto explicitNormalSnapshot = implicitNormalCanvas.snapshot();
        check(explicitNormalSnapshot.document.paragraphs().front().styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  !explicitNormalSnapshot.document.paragraphs().front()
                       .styleProvenance(),
              "identity-only Normal request created meaningless provenance");

        const auto explicitClearPreview =
            docxstudio::app::invokeEditorTool(
                implicitNormalCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", explicitNormalSnapshot.revision.value()},
                 {"summary", "Keep body paragraph separate from the next"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", implicitBlockId}, {"start", 0}}},
                    {"style", {{"keepWithNext", false}}}}}}},
                summary, error);
        check(error.isEmpty() && !explicitClearPreview.empty() &&
                  implicitNormalCanvas.acceptPreview(error),
              "formatting-only Codex request did not initialize Normal provenance");
        const auto implicitStyled = implicitNormalCanvas.snapshot();
        const auto& explicitNormal =
            implicitStyled.document.paragraphs().front();
        check(explicitNormal.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  explicitNormal.styleProvenance() &&
                  explicitNormal.styleProvenance()
                      ->paragraph_overrides.keep_with_next &&
                  explicitNormal.format().keep_with_next == false,
              "equal-to-Normal explicit override was not recorded as direct");

        const auto headingAfterClear =
            docxstudio::app::invokeEditorTool(
                implicitNormalCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", implicitStyled.revision.value()},
                 {"summary", "Apply Heading 1 without changing the override"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", implicitBlockId}, {"start", 0}}},
                    {"style", {{"styleId", "Heading1"}}}}}}},
                summary, error);
        check(error.isEmpty() && !headingAfterClear.empty() &&
                  implicitNormalCanvas.acceptPreview(error),
              "Codex could not transition provenance-initialized Normal to Heading 1");
        const auto headingWithClearSnapshot = implicitNormalCanvas.snapshot();
        const auto& headingWithClear =
            headingWithClearSnapshot.document.paragraphs().front();
        check(headingWithClear.styleId() ==
                      std::optional<std::string>{"Heading1"} &&
                  headingWithClear.format().keep_with_next == false &&
                  headingWithClear.styleProvenance() &&
                  headingWithClear.styleProvenance()
                      ->paragraph_overrides.keep_with_next &&
                  headingWithClear.format().space_before_emu == 152400 &&
                  headingWithClear.characterFormatAt(1).bold == true,
              "later style transition erased an equal-to-baseline Codex override");
    }

    {
        docxstudio::app::DocumentCanvas directTextCanvas(spelling);
        directTextCanvas.insertText(QStringLiteral("Plain text"));
        const auto directTextBase = directTextCanvas.snapshot();
        const auto directTextBlockId =
            directTextBase.document.paragraphs().front().id().toString();
        const auto directTextPreview = docxstudio::app::invokeEditorTool(
            directTextCanvas, documentId, previewTool,
            {{"documentId", documentId.toStdString()},
             {"baseRevision", directTextBase.revision.value()},
             {"summary", "Keep explicit body text formatting"},
             {"operations",
              {{{"kind", "set_text_style"},
                {"target",
                 {{"blockId", directTextBlockId},
                  {"start", 0},
                  {"end", 10}}},
                {"style",
                 {{"bold", false},
                  {"foregroundColor", "#000000"}}}}}}},
            summary, error);
        check(error.isEmpty() && !directTextPreview.empty() &&
                  directTextCanvas.acceptPreview(error),
              "direct text formatting did not initialize implicit Normal provenance");
        const auto directTextStyled = directTextCanvas.snapshot();
        const auto& directNormal =
            directTextStyled.document.paragraphs().front();
        const auto directMask = directNormal.styleOverrideMaskAt(1);
        check(directNormal.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  directNormal.styleProvenance() && directMask.bold &&
                  directMask.foreground_argb &&
                  directNormal.characterFormatAt(1).bold == false &&
                  directNormal.characterFormatAt(1).foreground_argb ==
                      0xff000000U,
              "equal-to-Normal text properties were not recorded as direct");

        const auto directHeadingPreview =
            docxstudio::app::invokeEditorTool(
                directTextCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", directTextStyled.revision.value()},
                 {"summary", "Apply Heading 1 around direct text formatting"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", directTextBlockId}, {"start", 0}}},
                    {"style", {{"styleId", "Heading1"}}}}}}},
                summary, error);
        check(error.isEmpty() && !directHeadingPreview.empty() &&
                  directTextCanvas.acceptPreview(error),
              "Codex could not style direct text after lazy provenance initialization");
        const auto directHeadingSnapshot = directTextCanvas.snapshot();
        const auto& directHeading =
            directHeadingSnapshot.document.paragraphs().front();
        check(directHeading.characterFormatAt(1).bold == false &&
                  directHeading.characterFormatAt(1).foreground_argb ==
                      0xff000000U &&
                  directHeading.characterFormatAt(1)
                          .font_size_half_points == 32,
              "Heading transition erased explicit black/bold-off text formatting");
    }

    {
        docxstudio::app::DocumentCanvas replacementDirectCanvas(spelling);
        replacementDirectCanvas.insertText(QStringLiteral("AoldZ"));
        const auto replacementDirectBase =
            replacementDirectCanvas.snapshot();
        const auto replacementDirectBlockId =
            replacementDirectBase.document.paragraphs().front().id()
                .toString();
        const auto directSourcePreview =
            docxstudio::app::invokeEditorTool(
                replacementDirectCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", replacementDirectBase.revision.value()},
                 {"summary", "Keep replacement source explicitly plain"},
                 {"operations",
                  {{{"kind", "set_text_style"},
                    {"target",
                     {{"blockId", replacementDirectBlockId},
                      {"start", 1},
                      {"end", 4}}},
                    {"style",
                     {{"bold", false},
                      {"foregroundColor", "#000000"}}}}}}},
                summary, error);
        check(error.isEmpty() && !directSourcePreview.empty() &&
                  replacementDirectCanvas.acceptPreview(error),
              "could not create an explicitly black/bold-off replacement source");
        const auto directSourceSnapshot =
            replacementDirectCanvas.snapshot();
        const auto& directSource =
            directSourceSnapshot.document.paragraphs().front();
        check(directSource.styleOverrideMaskAt(1).empty() &&
                  directSource.styleOverrideMaskAt(2).bold &&
                  directSource.styleOverrideMaskAt(2).foreground_argb &&
                  directSource.styleOverrideMaskAt(5).empty(),
              "replacement fixture did not isolate direct source provenance");

        const auto directReplacementPreview =
            docxstudio::app::invokeEditorTool(
                replacementDirectCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", directSourceSnapshot.revision.value()},
                 {"summary", "Replace only the explicitly plain span"},
                 {"operations",
                  {{{"kind", "replace_text"},
                    {"target",
                     {{"blockId", replacementDirectBlockId},
                      {"start", 1},
                      {"end", 4}}},
                    {"text", "new"}}}}},
                summary, error);
        check(error.isEmpty() && !directReplacementPreview.empty() &&
                  replacementDirectCanvas.acceptPreview(error),
              "Codex could not replace a partial direct-format span");
        const auto replacedDirectSnapshot =
            replacementDirectCanvas.snapshot();
        const auto& replacedDirect =
            replacedDirectSnapshot.document.paragraphs().front();
        check(paragraphText(replacementDirectCanvas) ==
                      QStringLiteral("AnewZ") &&
                  replacedDirect.styleOverrideMaskAt(1).empty() &&
                  replacedDirect.styleOverrideMaskAt(2).bold &&
                  replacedDirect.styleOverrideMaskAt(2).foreground_argb &&
                  replacedDirect.styleOverrideMaskAt(4).bold &&
                  replacedDirect.styleOverrideMaskAt(4).foreground_argb &&
                  replacedDirect.styleOverrideMaskAt(5).empty(),
              "Codex partial replacement lost or spread source directness");

        const auto replacementHeadingPreview =
            docxstudio::app::invokeEditorTool(
                replacementDirectCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", replacedDirectSnapshot.revision.value()},
                 {"summary", "Apply Heading 1 after the partial replacement"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", replacementDirectBlockId},
                      {"start", 0}}},
                    {"style", {{"styleId", "Heading1"}}}}}}},
                summary, error);
        check(error.isEmpty() && !replacementHeadingPreview.empty() &&
                  replacementDirectCanvas.acceptPreview(error),
              "Codex could not style the paragraph after a partial replacement");
        const auto replacementHeadingSnapshot =
            replacementDirectCanvas.snapshot();
        const auto& replacementHeading =
            replacementHeadingSnapshot.document.paragraphs().front();
        check(replacementHeading.characterFormatAt(1).bold == true &&
                  replacementHeading.characterFormatAt(1).foreground_argb ==
                      0xffe95420U &&
                  replacementHeading.characterFormatAt(2).bold == false &&
                  replacementHeading.characterFormatAt(2).foreground_argb ==
                      0xff000000U &&
                  replacementHeading.characterFormatAt(5).bold == true &&
                  replacementHeading.characterFormatAt(5).foreground_argb ==
                      0xffe95420U,
              "style transition erased replacement directness or changed adjacent inheritance");
    }

    {
        auto customParagraph = docxstudio::core::Paragraph::restore(
            u"Custom", docxstudio::core::NodeId::generate(), {},
            std::string{"Firm.Custom"});
        check(static_cast<bool>(customParagraph),
              "could not create provenance-free custom style fixture");
        auto customDocument = docxstudio::core::Document::create(
            {std::move(customParagraph.value())});
        check(static_cast<bool>(customDocument),
              "could not create custom style document");
        docxstudio::app::DocumentCanvas customDirectCanvas(spelling);
        customDirectCanvas.setDocument(std::move(customDocument.value()));
        const auto customDirectBase = customDirectCanvas.snapshot();
        const auto customDirectBlockId =
            customDirectBase.document.paragraphs().front().id().toString();
        const auto customDirectPreview = docxstudio::app::invokeEditorTool(
            customDirectCanvas, documentId, previewTool,
            {{"documentId", documentId.toStdString()},
             {"baseRevision", customDirectBase.revision.value()},
             {"summary", "Format custom-style text"},
             {"operations",
              {{{"kind", "set_text_style"},
                {"target",
                 {{"blockId", customDirectBlockId},
                  {"start", 0},
                  {"end", 6}}},
                {"style", {{"bold", false}}}}}}},
            summary, error);
        check(error.isEmpty() && !customDirectPreview.empty() &&
                  customDirectCanvas.acceptPreview(error) &&
                  customDirectCanvas.snapshot().document.paragraphs().front()
                          .styleId() ==
                      std::optional<std::string>{"Firm.Custom"} &&
                  !customDirectCanvas.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "direct formatting invented provenance for an unknown custom style");
    }

    const auto staleStyle = docxstudio::app::invokeEditorTool(
        styleMutationCanvas, documentId, previewTool,
        {{"documentId", documentId.toStdString()},
         {"baseRevision", customBase.revision.value()},
         {"summary", "Reject stale style change"},
         {"operations",
          {{{"kind", "set_paragraph_style"},
            {"target", {{"blockId", styleBlockId}, {"start", 0}}},
            {"style", {{"styleId", "Heading3"}}}}}}},
        summary, error);
    check(staleStyle.empty() &&
              error.contains(QStringLiteral("REVISION_CONFLICT")),
          "stale paragraph-style preview did not report a revision conflict");

    auto preservedStyleParagraph = docxstudio::core::Paragraph::create(
        u"Red inherited");
    check(static_cast<bool>(preservedStyleParagraph),
          "could not create Codex style-preservation paragraph");
    auto preservedStyleDocument = docxstudio::core::Document::create(
        {std::move(preservedStyleParagraph.value())});
    check(static_cast<bool>(preservedStyleDocument),
          "could not create Codex style-preservation document");
    const auto preservedStyleId =
        preservedStyleDocument.value().paragraphs().front().id();
    docxstudio::core::CharacterFormatDelta directRed;
    directRed.foreground_argb =
        docxstudio::core::PropertyDelta<std::uint32_t>::set(0xffcc0000U);
    check(static_cast<bool>(preservedStyleDocument.value().applyCharacterFormat(
              {{preservedStyleId, 0}, {preservedStyleId, 3}}, directRed)),
          "could not create a direct red span for Codex style testing");

    docxstudio::app::DocumentCanvas preservedStyleCanvas(spelling);
    preservedStyleCanvas.setEditorDefaults(
        QStringLiteral("DejaVu Sans"), 13.5, 4);
    preservedStyleCanvas.setDocument(
        std::move(preservedStyleDocument.value()));
    const auto preservedStyleBlockId = preservedStyleId.toString();
    const auto invokeAndAcceptStyle =
        [&](const std::string& styleId,
            const char* failureMessage) {
            error.clear();
            const auto beforeStylePreview = preservedStyleCanvas.snapshot();
            const auto preview = docxstudio::app::invokeEditorTool(
                preservedStyleCanvas, documentId, previewTool,
                {{"documentId", documentId.toStdString()},
                 {"baseRevision", beforeStylePreview.revision.value()},
                 {"summary", "Apply a native paragraph style"},
                 {"operations",
                  {{{"kind", "set_paragraph_style"},
                    {"target",
                     {{"blockId", preservedStyleBlockId}, {"start", 0}}},
                    {"style", {{"styleId", styleId}}}}}}},
                summary, error);
            check(error.isEmpty() && !preview.empty() &&
                      preservedStyleCanvas.acceptPreview(error),
                  failureMessage);
        };

    invokeAndAcceptStyle(
        "Heading1",
        "Codex could not apply a provenance-aware Heading 1 style");
    const auto headingThroughCodex = preservedStyleCanvas.snapshot();
    const auto& headingThroughCodexParagraph =
        headingThroughCodex.document.paragraphs().front();
    check(headingThroughCodexParagraph.styleId() ==
                  std::optional<std::string>{"Heading1"} &&
              headingThroughCodexParagraph.characterFormatAt(1)
                      .foreground_argb == 0xffcc0000U &&
              headingThroughCodexParagraph.characterFormatAt(1).bold == true &&
              headingThroughCodexParagraph.characterFormatAt(5)
                      .foreground_argb == 0xffe95420U &&
              headingThroughCodexParagraph.characterFormatAt(5).bold == true &&
              headingThroughCodexParagraph.characterFormatAt(5)
                      .font_size_half_points == 32,
          "Codex style application clobbered a direct span or skipped inherited Heading 1 properties");

    invokeAndAcceptStyle(
        "Normal",
        "Codex could not return a styled paragraph to configured Normal defaults");
    const auto normalThroughCodex = preservedStyleCanvas.snapshot();
    const auto& normalThroughCodexParagraph =
        normalThroughCodex.document.paragraphs().front();
    check(normalThroughCodexParagraph.styleId() ==
                  std::optional<std::string>{"Normal"} &&
              normalThroughCodexParagraph.characterFormatAt(1)
                      .foreground_argb == 0xffcc0000U &&
              normalThroughCodexParagraph.characterFormatAt(1).font_family ==
                  std::optional<std::string>{"DejaVu Sans"} &&
              normalThroughCodexParagraph.characterFormatAt(1)
                      .font_size_half_points == 27 &&
              normalThroughCodexParagraph.characterFormatAt(1).bold == false &&
              normalThroughCodexParagraph.characterFormatAt(5)
                      .foreground_argb == 0xff000000U &&
              normalThroughCodexParagraph.characterFormatAt(5).font_family ==
                  std::optional<std::string>{"DejaVu Sans"} &&
              normalThroughCodexParagraph.characterFormatAt(5)
                      .font_size_half_points == 27 &&
              normalThroughCodexParagraph.characterFormatAt(5).bold == false,
          "Codex Normal style lost a direct color or ignored configured font defaults");

    invokeAndAcceptStyle(
        "NoSpacing",
        "Codex could not apply No Spacing with configured defaults");
    const auto noSpacingThroughCodex = preservedStyleCanvas.snapshot();
    const auto& noSpacingThroughCodexParagraph =
        noSpacingThroughCodex.document.paragraphs().front();
    check(noSpacingThroughCodexParagraph.styleId() ==
                  std::optional<std::string>{"NoSpacing"} &&
              noSpacingThroughCodexParagraph.characterFormatAt(1)
                      .foreground_argb == 0xffcc0000U &&
              noSpacingThroughCodexParagraph.characterFormatAt(1)
                      .font_family ==
                  std::optional<std::string>{"DejaVu Sans"} &&
              noSpacingThroughCodexParagraph.characterFormatAt(1)
                      .font_size_half_points == 27 &&
              noSpacingThroughCodexParagraph.characterFormatAt(5)
                      .foreground_argb == 0xff000000U &&
              noSpacingThroughCodexParagraph.characterFormatAt(5)
                      .font_family ==
                  std::optional<std::string>{"DejaVu Sans"} &&
              noSpacingThroughCodexParagraph.characterFormatAt(5)
                      .font_size_half_points == 27,
          "Codex No Spacing lost direct formatting or configured defaults");

    docxstudio::app::DocumentCanvas imageCanvas(spelling);
    imageCanvas.insertText(QStringLiteral("A"));
    const auto png = bridgePng();
    check(imageCanvas.insertInlineImage(png, QStringLiteral("chart.png")),
          "could not insert bridge metadata image");
    const docxstudio::core::ImageLayout bridgeLayout{
        docxstudio::core::ImagePlacement::square,
        100, 200, 300, 400, false};
    check(imageCanvas.setSelectedImageLayout(bridgeLayout),
          "could not set bridge metadata image layout");
    const auto imageSnapshot = imageCanvas.snapshot();
    const auto& imageAtom =
        imageSnapshot.document.paragraphs().front().images().front();
    error.clear();
    const auto imageRead = docxstudio::app::invokeEditorTool(
        imageCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", imageSnapshot.revision.value()},
         {"scope", "document"},
         {"includeFormatting", true}},
        summary, error);
    const auto& imageBlock = imageRead.at("blocks").at(0);
    const auto& images = imageBlock.at("attributes").at("images");
    check(error.isEmpty() && imageBlock.at("text") == "A\xef\xbf\xbc" &&
              images.size() == 1 &&
              images.at(0).at("id") == imageAtom.id.toString() &&
              images.at(0).at("utf16Offset") == 1 &&
              images.at(0).at("accessibleName") == "chart.png" &&
              images.at(0).at("mimeType") == "image/png" &&
              images.at(0).at("widthEmu") == imageAtom.width_emu &&
              images.at(0).at("heightEmu") == imageAtom.height_emu &&
              images.at(0).at("layout").at("placement") == "square" &&
              images.at(0).at("layout").at("distanceTopEmu") == 100 &&
              images.at(0).at("layout").at("distanceRightEmu") == 200 &&
              images.at(0).at("layout").at("distanceBottomEmu") == 300 &&
              images.at(0).at("layout").at("distanceLeftEmu") == 400 &&
              images.at(0).at("layout").at("moveWithText") == false &&
              images.at(0).at("encodedBytes") == png.size() &&
              !images.at(0).contains("bytes") &&
              !images.at(0).contains("path"),
          "bounded editor read omitted image metadata or exposed payload/path data");

    auto searchFirst = docxstudio::core::Paragraph::create(u"Alpha \U0001f600 alpha");
    auto searchLast = docxstudio::core::Paragraph::create(u"tail ALPHA");
    check(static_cast<bool>(searchFirst) && static_cast<bool>(searchLast),
          "could not create search paragraphs");
    const auto firstSearchId = searchFirst.value().id();
    const auto lastSearchId = searchLast.value().id();
    auto searchDocument = docxstudio::core::Document::create(
        {std::move(searchFirst.value()), std::move(searchLast.value())});
    check(static_cast<bool>(searchDocument), "could not create search document");
    auto searchTable = docxstudio::core::Table::create(2, 2, true);
    check(static_cast<bool>(searchTable), "could not create search table");
    const auto searchTableId = searchTable.value().id();
    const auto searchCell00Id = searchTable.value().cell(0, 0)->id;
    const auto searchCell10Id = searchTable.value().cell(1, 0)->id;
    check(static_cast<bool>(searchDocument.value().insertTable(
              lastSearchId, std::move(searchTable.value()))),
          "could not insert search table");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 0, 0, u"alpha table")),
          "could not set first search table cell");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 0, 1, u"xalpha")),
          "could not set embedded-word search table cell");
    check(static_cast<bool>(searchDocument.value().setTableCellText(
              searchTableId, 1, 0, u"ALPHA")),
          "could not set final search table cell");

    docxstudio::app::DocumentCanvas searchCanvas(spelling);
    searchCanvas.setDocument(std::move(searchDocument.value()));
    searchCanvas.markSaved();
    const auto searchBefore = searchCanvas.snapshot();
    const auto search = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value()},
         {"query", "alpha"},
         {"caseSensitive", false},
         {"wholeWord", true},
         {"maxResults", 500}},
        summary, error);
    check(error.isEmpty(), "editor search failed");
    check(search.at("revision") == searchBefore.revision.value(),
          "search returned wrong revision");
    check(search.at("results").size() == 5,
          "whole-word search returned wrong result count");
    check(search.at("truncated") == false,
          "complete search was marked truncated");
    const auto& searchResults = search.at("results");
    check(searchResults.at(0).at("blockId") == firstSearchId.toString() &&
              searchResults.at(0).at("blockType") == "paragraph" &&
              searchResults.at(0).at("start") == 0 &&
              searchResults.at(0).at("end") == 5,
          "first paragraph search result is incorrect");
    check(searchResults.at(1).at("blockId") == firstSearchId.toString() &&
              searchResults.at(1).at("start") == 9 &&
              searchResults.at(1).at("end") == 14,
          "search offsets are not UTF-16 offsets");
    check(searchResults.at(2).at("blockId") == searchCell00Id.toString() &&
              searchResults.at(2).at("tableId") == searchTableId.toString() &&
              searchResults.at(2).at("cellId") == searchCell00Id.toString() &&
              searchResults.at(2).at("row") == 0 &&
              searchResults.at(2).at("column") == 0,
          "table-cell search result omitted stable coordinates or identifiers");
    check(searchResults.at(3).at("cellId") == searchCell10Id.toString() &&
              searchResults.at(3).at("row") == 1 &&
              searchResults.at(3).at("column") == 0,
          "table-cell search order is not deterministic row-major order");
    check(searchResults.at(4).at("blockId") == lastSearchId.toString(),
          "search did not resume body order after the table");

    const auto tableCellRead = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorReadTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value()},
         {"scope", "blocks"},
         {"blockIds", {firstSearchId.toString(), lastSearchId.toString()}},
         {"tableCellTargets",
          {{{"tableId", searchResults.at(2).at("tableId")},
            {"cellId", searchResults.at(2).at("cellId")}},
           {{"tableId", searchResults.at(3).at("tableId")},
            {"cellId", searchResults.at(3).at("cellId")}}}}},
        summary, error);
    check(error.isEmpty() && tableCellRead.at("blocks").size() == 4,
          "typed table-cell read failed");
    const auto& readBlocks = tableCellRead.at("blocks");
    check(readBlocks.at(0).at("id") == firstSearchId.toString() &&
              readBlocks.at(1).at("id") == searchCell00Id.toString() &&
              readBlocks.at(1).at("type") == "tableCell" &&
              readBlocks.at(1).at("text") == "alpha table" &&
              readBlocks.at(1).at("attributes").at("tableId") ==
                  searchTableId.toString() &&
              readBlocks.at(1).at("attributes").at("row") == 0 &&
              readBlocks.at(1).at("attributes").at("column") == 0 &&
              readBlocks.at(2).at("id") == searchCell10Id.toString() &&
              readBlocks.at(3).at("id") == lastSearchId.toString(),
          "table-cell read did not preserve body/row-major order or identity");
    const auto searchAfter = searchCanvas.snapshot();
    check(searchAfter.revision == searchBefore.revision &&
              searchAfter.document == searchBefore.document &&
              !searchCanvas.isModified(),
          "read-only search mutated the document");

    const auto boundedSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"wholeWord", true},
         {"maxResults", 3}},
        summary, error);
    check(error.isEmpty() && boundedSearch.at("results").size() == 3 &&
              boundedSearch.at("truncated") == true,
          "bounded search did not report truncation");

    const auto caseSensitiveSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"caseSensitive", true},
         {"wholeWord", true}},
        summary, error);
    check(error.isEmpty() && caseSensitiveSearch.at("results").size() == 2,
          "case-sensitive whole-word search returned wrong results");

    const auto wrongDocumentSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", "document:other"}, {"query", "alpha"}},
        summary, error);
    check(wrongDocumentSearch.empty() && error.contains(QStringLiteral("documentId")),
          "search accepted a document capability mismatch");

    const auto staleSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"expectedRevision", searchBefore.revision.value() + 1},
         {"query", "alpha"}},
        summary, error);
    check(staleSearch.empty() && error.contains(QStringLiteral("REVISION_CONFLICT")),
          "stale search did not report a revision conflict");

    const auto emptySearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", ""}},
        summary, error);
    check(emptySearch.empty() && error.contains(QStringLiteral("nonempty")),
          "search accepted an empty query");

    const auto invalidUtf8Search = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", std::string("\xc3\x28", 2)}},
        summary, error);
    check(invalidUtf8Search.empty() &&
              error.contains(QStringLiteral("valid UTF-8")),
          "search silently replaced malformed UTF-8");

    const auto oversizedSearch = docxstudio::app::invokeEditorTool(
        searchCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "alpha"},
         {"maxResults", 501}},
        summary, error);
    check(oversizedSearch.empty() && error.contains(QStringLiteral("500")),
          "search accepted an unbounded result count");

    const QString surrogateText =
        QString::fromUtf8("\xF0\x9F\x98\x80") + QString(79, QLatin1Char('x')) +
        QStringLiteral("needle") + QString(79, QLatin1Char('y')) +
        QString::fromUtf8("\xF0\x9F\x98\x80");
    auto surrogateParagraph = docxstudio::core::Paragraph::create(
        surrogateText.toStdU16String());
    check(static_cast<bool>(surrogateParagraph),
          "could not create surrogate-context paragraph");
    auto surrogateDocument = docxstudio::core::Document::create(
        {std::move(surrogateParagraph.value())});
    check(static_cast<bool>(surrogateDocument),
          "could not create surrogate-context document");
    docxstudio::app::DocumentCanvas surrogateCanvas(spelling);
    surrogateCanvas.setDocument(std::move(surrogateDocument.value()));
    const auto surrogateSearch = docxstudio::app::invokeEditorTool(
        surrogateCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "needle"}},
        summary, error);
    check(error.isEmpty() && surrogateSearch.at("results").size() == 1,
          "surrogate-context search failed");
    const auto& surrogateMatch = surrogateSearch.at("results").at(0);
    const QString surrogateContext = QString::fromStdString(
        surrogateMatch.at("context").get<std::string>());
    check(surrogateMatch.at("start") == 81 && surrogateMatch.at("end") == 87 &&
              surrogateMatch.at("contextStart") == 2 &&
              surrogateMatch.at("contextEnd") == 166,
          "search returned incorrect UTF-16 context offsets");
    check(surrogateContext.size() == 164 &&
              surrogateContext.front() == QLatin1Char('x') &&
              surrogateContext.back() == QLatin1Char('y'),
          "search context split a UTF-16 surrogate pair");

    const QString astralQuery =
        QString::fromUtf8("\xF0\x9F\x98\x80").repeated(600);
    auto astralParagraph = docxstudio::core::Paragraph::create(
        astralQuery.toStdU16String());
    check(static_cast<bool>(astralParagraph),
          "could not create astral-query paragraph");
    auto astralDocument = docxstudio::core::Document::create(
        {std::move(astralParagraph.value())});
    check(static_cast<bool>(astralDocument),
          "could not create astral-query document");
    docxstudio::app::DocumentCanvas astralCanvas(spelling);
    astralCanvas.setDocument(std::move(astralDocument.value()));
    const auto astralSearch = docxstudio::app::invokeEditorTool(
        astralCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", astralQuery.toStdString()}},
        summary, error);
    check(error.isEmpty() && astralSearch.at("results").size() == 1 &&
              astralSearch.at("results").at(0).at("end") == 1200,
          "schema-valid astral query was not counted as Unicode code points");
    const QString tooManyAstral =
        QString::fromUtf8("\xF0\x9F\x98\x80").repeated(1025);
    const auto excessiveCodePointSearch =
        docxstudio::app::invokeEditorTool(
            astralCanvas, documentId,
            QString::fromLatin1(
                docxstudio::codex::kEditorSearchTool.data(),
                static_cast<qsizetype>(
                    docxstudio::codex::kEditorSearchTool.size())),
            {{"documentId", documentId.toStdString()},
             {"query", tooManyAstral.toStdString()}},
            summary, error);
    check(excessiveCodePointSearch.empty() &&
              error.contains(QStringLiteral("1024")),
          "search accepted more Unicode code points than its schema limit");

    const char32_t familyCodePoints[] = {
        0x1f468, 0x200d, 0x1f469, 0x200d, 0x1f467, 0x200d, 0x1f466};
    const QString family = QString::fromUcs4(familyCodePoints, 7);
    const QString graphemeText =
        QStringLiteral("A") + QString(90, QChar(0x0301)) +
        QStringLiteral("needle") + family.repeated(8);
    auto graphemeParagraph = docxstudio::core::Paragraph::create(
        graphemeText.toStdU16String());
    check(static_cast<bool>(graphemeParagraph),
          "could not create grapheme-context paragraph");
    auto graphemeDocument = docxstudio::core::Document::create(
        {std::move(graphemeParagraph.value())});
    check(static_cast<bool>(graphemeDocument),
          "could not create grapheme-context document");
    docxstudio::app::DocumentCanvas graphemeCanvas(spelling);
    graphemeCanvas.setDocument(std::move(graphemeDocument.value()));
    const auto graphemeSearch = docxstudio::app::invokeEditorTool(
        graphemeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "needle"}},
        summary, error);
    check(error.isEmpty() && graphemeSearch.at("results").size() == 1,
          "grapheme-context search failed");
    const auto& graphemeMatch = graphemeSearch.at("results").at(0);
    const qsizetype graphemeContextStart =
        static_cast<qsizetype>(graphemeMatch.at("contextStart").get<std::size_t>());
    const qsizetype graphemeContextEnd =
        static_cast<qsizetype>(graphemeMatch.at("contextEnd").get<std::size_t>());
    QTextBoundaryFinder originalGraphemes(QTextBoundaryFinder::Grapheme,
                                          graphemeText);
    originalGraphemes.setPosition(graphemeContextStart);
    check(originalGraphemes.isAtBoundary() && graphemeContextStart == 91,
          "search context began inside a combining sequence");
    originalGraphemes.setPosition(graphemeContextEnd);
    check(originalGraphemes.isAtBoundary() &&
              graphemeContextEnd >= 97 &&
              graphemeContextEnd < graphemeText.size(),
          "search context ended inside an emoji ZWJ sequence");
    const QString graphemeContext = QString::fromStdString(
        graphemeMatch.at("context").get<std::string>());
    check(graphemeContext.startsWith(QStringLiteral("needle")) &&
              !graphemeContext.endsWith(QChar(0x200d)),
          "grapheme-safe context omitted the match or retained a dangling joiner");

    const auto partialGraphemeSearch = docxstudio::app::invokeEditorTool(
        graphemeCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()}, {"query", "A"}},
        summary, error);
    check(error.isEmpty() && partialGraphemeSearch.at("results").empty(),
          "search returned an edit range that splits a combining sequence");

    const QString adversarialWord(30000, QLatin1Char('a'));
    auto adversarialParagraph = docxstudio::core::Paragraph::create(
        adversarialWord.toStdU16String());
    check(static_cast<bool>(adversarialParagraph),
          "could not create adversarial search paragraph");
    auto adversarialDocument = docxstudio::core::Document::create(
        {std::move(adversarialParagraph.value())});
    check(static_cast<bool>(adversarialDocument),
          "could not create adversarial search document");
    docxstudio::app::DocumentCanvas adversarialCanvas(spelling);
    adversarialCanvas.setDocument(std::move(adversarialDocument.value()));
    QElapsedTimer searchTimer;
    searchTimer.start();
    const auto adversarialSearch = docxstudio::app::invokeEditorTool(
        adversarialCanvas, documentId,
        QString::fromLatin1(docxstudio::codex::kEditorSearchTool.data(),
                            static_cast<qsizetype>(
                                docxstudio::codex::kEditorSearchTool.size())),
        {{"documentId", documentId.toStdString()},
         {"query", "a"},
         {"wholeWord", true}},
        summary, error);
    check(error.isEmpty() && adversarialSearch.at("results").empty() &&
              !adversarialSearch.at("truncated").get<bool>(),
          "adversarial whole-word search returned an incorrect result");
    check(searchTimer.elapsed() < 3000,
          "whole-word search rebuilt boundary tables per candidate match");

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
