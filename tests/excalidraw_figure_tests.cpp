#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPointer>
#include <QTemporaryDir>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QByteArray png(const QColor& color, int width, int height) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not create PNG fixture");
    return bytes;
}

std::vector<std::uint8_t> bytes(const QByteArray& value) {
    return {
        reinterpret_cast<const std::uint8_t*>(value.constData()),
        reinterpret_cast<const std::uint8_t*>(value.constData()) +
            value.size()};
}

QByteArray atomBytes(const docxstudio::core::ImageAtom& image) {
    const auto payload = image.encoded_payload.bytes();
    return QByteArray(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<qsizetype>(payload.size()));
}

QByteArray supportedScene(int count = 1, int labelLength = 12) {
    QJsonArray elements;
    for (int index = 0; index < count; ++index) {
        const QString id = QStringLiteral("shape-%1").arg(index);
        const QString labelId = QStringLiteral("label-%1").arg(index);
        elements.append(QJsonObject{{"id", id}, {"type", "rectangle"}, {"x", index * 120}, {"y", 20},
            {"width", 100}, {"height", 60}, {"roughness", 0}, {"fillStyle", "solid"},
            {"strokeColor", "#222624"}, {"backgroundColor", "#ffffff"}, {"strokeWidth", 2},
            {"opacity", 100}, {"angle", 0}, {"groupIds", QJsonArray{}},
            {"boundElements", QJsonArray{QJsonObject{{"id", labelId}, {"type", "text"}}}}});
        elements.append(QJsonObject{{"id", labelId}, {"type", "text"}, {"containerId", id}, {"x", index * 120 + 10},
            {"y", 30}, {"width", 80}, {"height", 20}, {"text", "Visible wrap"},
            {"originalText", QString(labelLength, QLatin1Char('x'))}, {"fontFamily", 2}, {"fontSize", 20},
            {"textAlign", "center"}, {"verticalAlign", "middle"}, {"autoResize", true}, {"lineHeight", 1.15},
            {"roughness", 1}, {"fillStyle", "hachure"}, {"strokeWidth", 1}, {"strokeColor", "#222624"}, {"opacity", 100}});
    }
    return QJsonDocument(QJsonObject{{"type", "excalidraw"}, {"version", 2}, {"elements", elements},
        {"appState", QJsonObject{}}, {"files", QJsonObject{}}}).toJson(QJsonDocument::Compact);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("OwlDocsExcalidrawFigureTests"));

    const QByteArray sceneOne = QByteArrayLiteral(
        R"({"type":"excalidraw","version":2,"source":"owl-docs","elements":[{"id":"one","type":"rectangle"}],"appState":{},"files":{}})");
    const QByteArray sceneTwo = QByteArrayLiteral(
        R"({"type":"excalidraw","version":2,"source":"owl-docs","elements":[{"id":"two","type":"ellipse"}],"appState":{},"files":{}})");
    QString error;
    const QByteArray first =
        docxstudio::app::pngWithExcalidrawScene(
            png(Qt::red, 80, 40), sceneOne, error);
    check(!first.isEmpty() && error.isEmpty(),
          "could not embed first figure scene");
    check(docxstudio::app::excalidrawSceneFromPng(first) == sceneOne,
          "embedded figure scene did not round-trip");
    QImage decoded;
    check(decoded.loadFromData(first, "PNG") &&
              decoded.size() == QSize(80, 40),
          "private metadata made the PNG unrenderable");

    const QByteArray second =
        docxstudio::app::pngWithExcalidrawScene(
            first, sceneTwo, error);
    check(!second.isEmpty() &&
              docxstudio::app::excalidrawSceneFromPng(second) == sceneTwo &&
              second.count("owl-docs.excalidraw.v1") == 1,
          "editing did not replace the prior scene exactly once");
    check(docxstudio::app::pngWithExcalidrawScene(
              png(Qt::black, 4, 4), QByteArrayLiteral("{}"), error).isEmpty(),
          "invalid figure scene was accepted");

    // The renderer is a process boundary. Exercise the actual tool dispatcher
    // with an inert result so normalization and transaction tests stay local.
    {
        docxstudio::app::SpellChecker toolSpelling;
        docxstudio::app::DocumentCanvas toolCanvas(toolSpelling);
        const auto before = toolCanvas.snapshot();
        docxstudio::codex::Json normalized;
        int renderCalls = 0;
        const docxstudio::app::ExcalidrawRenderer renderer =
            [&](const QJsonArray& skeleton, QString& renderError)
                -> std::optional<docxstudio::app::ExcalidrawFigureEditor::Result> {
                ++renderCalls;
                normalized = docxstudio::codex::Json::parse(
                    QJsonDocument(skeleton).toJson(QJsonDocument::Compact).toStdString());
                renderError.clear();
                return docxstudio::app::ExcalidrawFigureEditor::Result{first, sceneOne};
            };
        const docxstudio::codex::Json arrow{
            {"id", "process-arrow"}, {"type", "arrow"}, {"x", 0}, {"y", 0},
            {"points", {{0, 0}, {120, 0}}},
            {"label", {{"text", "Continue"}, {"fontSize", 18}}}};
        const docxstudio::codex::Json operation{
            {"kind", "insert_excalidraw_figure"},
            {"target", {{"blockId", before.document.paragraphs().front().id().toString()}, {"start", 0}}},
            {"figure", {{"accessibleName", "Process arrow"},
                {"elements", docxstudio::codex::Json::array({arrow})}}}};
        const docxstudio::codex::Json request{
            {"documentId", "document:figure-normalization"},
            {"baseRevision", before.revision.value()},
            {"summary", "A labeled process arrow"},
            {"operations", docxstudio::codex::Json::array({operation})}};
        QString summary;
        error.clear();
        const auto result = docxstudio::app::invokeEditorTool(
            toolCanvas, QStringLiteral("document:figure-normalization"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            request, summary, error, renderer);
        check(error.isEmpty() && !result.empty() && renderCalls == 1 &&
                  normalized.at(0).contains("label") &&
                  normalized.at(0).at("label").at("text") == "Continue" &&
                  normalized.at(0).at("label").at("fontSize") == 18,
              "Codex discarded the advertised arrow label before rendering");
        check(toolCanvas.snapshot().revision == before.revision && toolCanvas.hasPreview(),
              "figure generation mutated the document before acceptance");
        toolCanvas.discardPreview();
        auto invalidLine = request;
        invalidLine["operations"][0]["figure"]["elements"][0]["type"] = "line";
        const auto rejectedLine = docxstudio::app::invokeEditorTool(
            toolCanvas, QStringLiteral("document:figure-normalization"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            invalidLine, summary, error, renderer);
        check(rejectedLine.empty() && error.contains(QStringLiteral("line")) &&
                  renderCalls == 1 && !toolCanvas.hasPreview(),
              "unsupported line label was silently discarded instead of rejected before rendering");
        auto missingFigure = request;
        missingFigure["operations"][0]["kind"] = "replace_excalidraw_figure";
        missingFigure["operations"][0].erase("target");
        missingFigure["operations"][0]["imageId"] = docxstudio::core::NodeId::generate().toString();
        const auto rejectedFigure = docxstudio::app::invokeEditorTool(
            toolCanvas, QStringLiteral("document:figure-normalization"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            missingFigure, summary, error, renderer);
        check(rejectedFigure.empty() && error.contains(QStringLiteral("no longer exists")) &&
                  renderCalls == 1 && !toolCanvas.hasPreview() &&
                  toolCanvas.snapshot().document == before.document,
              "missing figure target was rendered before its identity was validated");
        QPointer<docxstudio::app::DocumentCanvas> closingCanvas =
            new docxstudio::app::DocumentCanvas(toolSpelling);
        auto closingRequest = request;
        closingRequest["operations"][0]["target"]["blockId"] =
            closingCanvas->snapshot().document.paragraphs().front().id().toString();
        const auto closedResult = docxstudio::app::invokeEditorTool(
            *closingCanvas, QStringLiteral("document:figure-normalization"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            closingRequest, summary, error,
            [&](const QJsonArray&, QString& renderError)
                -> std::optional<docxstudio::app::ExcalidrawFigureEditor::Result> {
                delete closingCanvas.data();
                renderError.clear();
                return docxstudio::app::ExcalidrawFigureEditor::Result{first, sceneOne};
            });
        check(!closingCanvas && closedResult.empty() &&
                  error.contains(QStringLiteral("closed")),
              "closing the document during figure rendering did not cancel its preview safely");
        bool sessionCurrent = true;
        const auto canceledResult = docxstudio::app::invokeEditorTool(
            toolCanvas, QStringLiteral("document:figure-normalization"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            request, summary, error,
            [&](const QJsonArray&, QString& renderError)
                -> std::optional<docxstudio::app::ExcalidrawFigureEditor::Result> {
                sessionCurrent = false;
                renderError.clear();
                return docxstudio::app::ExcalidrawFigureEditor::Result{first, sceneOne};
            }, [&] { return sessionCurrent; });
        check(canceledResult.empty() && error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive) &&
                  !toolCanvas.hasPreview() && toolCanvas.snapshot().document == before.document,
              "a disconnected Codex session created a late figure preview");
    }

    {
        docxstudio::app::SpellChecker placementSpelling;
        docxstudio::app::DocumentCanvas placement(placementSpelling);
        check(placement.insertTable(1, 1, false), "could not create figure-placement table");
        const auto before = placement.snapshot();
        const auto tableId = before.document.tables().front().id();
        const auto cellId = before.document.findTable(tableId)->cell(0, 0)->id;
        const auto imageId = docxstudio::core::NodeId::generate();
        const docxstudio::codex::Json figure{
            {"accessibleName", "A native table figure"}, {"widthPoints", 72}, {"heightPoints", 36},
            {"elements", {{{"id", "box"}, {"type", "rectangle"}, {"x", 0}, {"y", 0}, {"width", 80}, {"height", 40}}}}};
        docxstudio::codex::Json request{
            {"documentId", "document:figure-placement"}, {"baseRevision", before.revision.value()},
            {"summary", "Create a figure in the cell"},
            {"operations", {{{"kind", "insert_excalidraw_figure"}, {"imageId", imageId.toString()},
                {"target", {{"blockId", cellId.toString()}, {"start", 0}}}, {"figure", figure}}}}};
        int renders = 0;
        const docxstudio::app::ExcalidrawRenderer renderer =
            [&](const QJsonArray&, QString& renderError)
                -> std::optional<docxstudio::app::ExcalidrawFigureEditor::Result> {
                ++renders;
                renderError.clear();
                return docxstudio::app::ExcalidrawFigureEditor::Result{first, sceneOne};
            };
        QString summary;
        auto result = docxstudio::app::invokeEditorTool(placement,
            QStringLiteral("document:figure-placement"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()), request, summary, error, renderer);
        check(error.isEmpty() && !result.empty() && placement.hasPreview() &&
                  placement.snapshot().document == before.document,
              "Codex cannot create an editable figure inside a table cell");
        placement.discardPreview();
        check(placement.snapshot().document == before.document, "discard changed the figure's table cell");
        result = docxstudio::app::invokeEditorTool(placement,
            QStringLiteral("document:figure-placement"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()), request, summary, error, renderer);
        check(!result.empty() && placement.acceptPreview(error), "could not accept a table figure");
        check(placement.snapshot().document.findTable(tableId)->cell(0, 0)->images.front().id == imageId,
              "figure insertion ignored its declared stable image identity");
        placement.undo();
        check(placement.snapshot().document == before.document, "table figure insertion was not one Undo");

        for (const bool footer : {false, true}) {
            docxstudio::app::DocumentCanvas story(placementSpelling);
            auto source = story.snapshot().document;
            check(bool(footer ? source.setFooterText(u"A\tB\tC") : source.setHeaderText(u"A\tB\tC")),
                  "could not create story figure fixture");
            story.setDocument(source);
            const auto storyBefore = story.snapshot();
            const auto storyImageId = docxstudio::core::NodeId::generate();
            request["baseRevision"] = storyBefore.revision.value();
            request["operations"][0]["imageId"] = storyImageId.toString();
            request["operations"][0]["target"] = {{"story", footer ? "footer" : "header"}, {"start", 2}};
            result = docxstudio::app::invokeEditorTool(story,
                QStringLiteral("document:figure-placement"),
                QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()), request, summary, error, renderer);
            check(error.isEmpty() && !result.empty() && story.snapshot().document == source && story.acceptPreview(error),
                  "Codex cannot create a native figure in the header/footer story");
            check(story.snapshot().document.findImage(storyImageId) &&
                      (footer ? story.snapshot().document.footerImages() : story.snapshot().document.headerImages()).front().utf16_offset == 2,
                  "story figure lost its identity or chosen UTF-16 position");
            story.undo();
            check(story.snapshot().document == source, "story figure insertion was not one Undo");
            request["baseRevision"] = story.snapshot().revision.value();
            request["operations"][0]["target"]["start"] = 200;
            const int previousRenders = renders;
            result = docxstudio::app::invokeEditorTool(story,
                QStringLiteral("document:figure-placement"),
                QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()), request, summary, error, renderer);
            check(result.empty() && !error.isEmpty() && renders == previousRenders && story.snapshot().document == source,
                  "invalid story figure anchor reached the renderer or changed its story");
        }
    }

    {
        docxstudio::app::SpellChecker readSpelling;
        docxstudio::app::DocumentCanvas readable(readSpelling);
        const auto scene = supportedScene();
        const auto embedded = docxstudio::app::pngWithExcalidrawScene(png(Qt::white, 100, 60), scene, error);
        check(readable.insertInlineImage(bytes(embedded), QStringLiteral("Readable figure")), "could not insert readable scene");
        const auto imageId = readable.snapshot().document.paragraphs().front().images().front().id;
        QString summary;
        auto read = docxstudio::app::invokeEditorTool(readable, QStringLiteral("document:figure-read"),
            QString::fromLatin1(docxstudio::codex::kEditorReadTool.data()),
            {{"documentId", "document:figure-read"}, {"scope", "images"}, {"imageIds", {imageId.toString()}}}, summary, error);
        check(error.isEmpty() && !read.empty() && read.at("blocks").size() == 1, "targeted figure read failed");
        const auto attributes = read.at("blocks").front().at("attributes");
        check(attributes.value("editableElementsComplete", false) && attributes.at("editableElements").size() == 1 &&
                  attributes.at("editableElements").front().at("label").at("id") == "label-0" &&
                  attributes.at("editableElements").front().at("label").at("text") == std::string(12, 'x'),
              "figure read loses bound-label identity/source text or hides skeleton completeness");
        check(attributes.contains("nativeScene") && !attributes.value("nativeSceneTruncated", true) &&
                  attributes.at("nativeScene").at("elements").size() == 2,
              "targeted figure read does not expose bounded native source for safe edits");
        bool retainedLabelId = false;
        bool retainedBinding = false;
        auto editable = attributes.at("editableElements");
        editable.push_back({{"id", "connected-arrow"}, {"type", "arrow"}, {"x", 100}, {"y", 50},
            {"points", {{0, 0}, {80, 0}}}, {"start", {{"id", "shape-0"}}}});
        const auto normalizedRead = docxstudio::app::invokeEditorTool(readable, QStringLiteral("document:figure-read"),
            QString::fromLatin1(docxstudio::codex::kEditorPreviewTool.data()),
            {{"documentId", "document:figure-read"}, {"baseRevision", readable.snapshot().revision.value()},
             {"summary", "Copy the supported editable skeleton"},
             {"operations", {{{"kind", "insert_excalidraw_figure"},
                 {"target", {{"blockId", readable.snapshot().document.paragraphs().front().id().toString()}, {"start", 1}}},
                 {"figure", {{"elements", editable}}}}}}}, summary, error,
            [&](const QJsonArray& skeleton, QString& renderError)
                -> std::optional<docxstudio::app::ExcalidrawFigureEditor::Result> {
                retainedLabelId = skeleton.at(0).toObject().value("label").toObject().value("id") == "label-0";
                retainedBinding = skeleton.at(1).toObject().value("start").toObject().value("id") == "shape-0";
                renderError.clear(); return docxstudio::app::ExcalidrawFigureEditor::Result{embedded, scene};
            });
        check(!normalizedRead.empty() && retainedLabelId && retainedBinding,
              "read-to-edit normalization discarded bound-label or connector identity");
        readable.discardPreview();

        // Multiple moderately sized sources share a single context budget.
        auto largeDocument = readable.snapshot().document;
        const auto paragraphId = largeDocument.paragraphs().front().id();
        const auto largeScene = supportedScene(50, 2000);
        const auto largePng = docxstudio::app::pngWithExcalidrawScene(png(Qt::white, 100, 60), largeScene, error);
        for (std::size_t index = 1; index < 5; ++index)
            check(bool(largeDocument.insertImage({paragraphId, index}, docxstudio::core::EncodedImagePayload(bytes(largePng)),
                docxstudio::core::ImageFormat::png, "Large figure", 914400, 548640)), "could not insert context-budget scene");
        readable.setDocument(largeDocument);
        read = docxstudio::app::invokeEditorTool(readable, QStringLiteral("document:figure-read"),
            QString::fromLatin1(docxstudio::codex::kEditorReadTool.data()),
            {{"documentId", "document:figure-read"}, {"scope", "images"}}, summary, error);
        std::size_t contextBytes = 0;
        bool explicitlyTruncated = false;
        for (const auto& block : read.at("blocks")) {
            const auto& image = block.at("attributes");
            contextBytes += image.at("editableElements").dump().size();
            explicitlyTruncated = explicitlyTruncated || image.value("editableElementsTruncated", false);
            check(!image.contains("nativeScene"), "untargeted figure read duplicates whole native scenes");
        }
        check(error.isEmpty() && contextBytes <= 256U * 1024U && explicitlyTruncated,
              "figure context is unbounded across multiple pictures or truncation is hidden");

        QJsonArray unknownElements;
        for (int index = 0; index < 128; ++index) unknownElements.append(QJsonObject{
            {"id", QStringLiteral("unknown-%1-").arg(index) + QString(230, QLatin1Char('q'))}, {"type", "freedraw"}});
        const auto unknownScene = QJsonDocument(QJsonObject{{"elements", unknownElements}}).toJson(QJsonDocument::Compact);
        const auto unknownPng = docxstudio::app::pngWithExcalidrawScene(png(Qt::white, 2, 2), unknownScene, error);
        docxstudio::core::Document unknownDocument;
        const auto unknownParagraph = unknownDocument.paragraphs().front().id();
        for (std::size_t index = 0; index < 8; ++index)
            check(bool(unknownDocument.insertImage({unknownParagraph, index}, docxstudio::core::EncodedImagePayload(bytes(unknownPng)),
                docxstudio::core::ImageFormat::png, "Unsupported scene", 914400, 914400)), "could not insert unsupported context fixture");
        readable.setDocument(unknownDocument);
        read = docxstudio::app::invokeEditorTool(readable, QStringLiteral("document:figure-read"),
            QString::fromLatin1(docxstudio::codex::kEditorReadTool.data()),
            {{"documentId", "document:figure-read"}, {"scope", "images"}}, summary, error);
        contextBytes = 0;
        bool reasonsTruncated = false;
        for (const auto& block : read.at("blocks")) {
            const auto& image = block.at("attributes");
            contextBytes += image.at("editableElements").dump().size() + image.at("editableElementsUnsupported").dump().size();
            reasonsTruncated = reasonsTruncated || image.value("editableElementsUnsupportedTruncated", false);
        }
        check(error.isEmpty() && contextBytes <= 256U * 1024U && reasonsTruncated,
              "unsupported figure reasons bypass the shared read context budget");
    }

    const auto boundedDisplay =
        docxstudio::app::fitExcalidrawFigureDisplaySize(
            QSize(1000, 200), 300.0, 300.0);
    check(boundedDisplay &&
              std::abs(boundedDisplay->width() - 300.0) < 0.001 &&
              std::abs(boundedDisplay->height() - 60.0) < 0.001,
          "Codex figure dimensions did not preserve the exported raster aspect ratio");
    const auto heightOnlyDisplay =
        docxstudio::app::fitExcalidrawFigureDisplaySize(
            QSize(100, 200), std::nullopt, 180.0);
    check(heightOnlyDisplay &&
              std::abs(heightOnlyDisplay->width() - 90.0) < 0.001 &&
              std::abs(heightOnlyDisplay->height() - 180.0) < 0.001,
          "height-only Codex figure sizing did not preserve aspect ratio");

    docxstudio::app::SpellChecker anchorSpelling;
    docxstudio::app::DocumentCanvas anchorCanvas(anchorSpelling);
    const auto anchorSnapshot = anchorCanvas.snapshot();
    const auto anchorParagraph =
        anchorSnapshot.document.paragraphs().front().id().toString();
    QString anchorSummary;
    const auto anchorResult = docxstudio::app::invokeEditorTool(
        anchorCanvas, QStringLiteral("document:anchor-regression"),
        QString::fromLatin1(
            docxstudio::codex::kEditorPreviewTool.data(),
            static_cast<qsizetype>(
                docxstudio::codex::kEditorPreviewTool.size())),
        {{"documentId", "document:anchor-regression"},
         {"baseRevision", anchorSnapshot.revision.value()},
         {"summary", "Anchor regression"},
         {"operations",
          {{{"type", "insert_excalidraw_figure"},
            {"target", {{"blockId", anchorParagraph}, {"offset", 0}}},
            {"elements", docxstudio::codex::Json::array()}}}}},
        anchorSummary, error);
    check(anchorResult.empty() &&
              error.contains(QStringLiteral("between 1 and 128")),
          "figure shorthand did not accept an empty paragraph offset anchor");
    error.clear();

    // The end-to-end renderer needs permission to create its own bubblewrap
    // network namespace. Managed CI/sandbox runners can opt in when that
    // kernel facility is available; ordinary unit runs still cover PNG scene
    // embedding and the document lifecycle below.
    if (qEnvironmentVariableIsSet("OWL_DOCS_TEST_LOCAL_EXCALIDRAW_RENDERER")) {
        const QJsonArray skeleton{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("codex-box")},
                        {QStringLiteral("type"), QStringLiteral("rectangle")},
                        {QStringLiteral("x"), 20},
                        {QStringLiteral("y"), 20},
                        {QStringLiteral("width"), 220},
                        {QStringLiteral("height"), 100},
                        {QStringLiteral("backgroundColor"),
                         QStringLiteral("#E7F5FF")},
                        {QStringLiteral("label"),
                         QJsonObject{{QStringLiteral("text"),
                                      QStringLiteral("Codex figure")}}}}};
        const auto rendered =
            docxstudio::app::renderProfessionalExcalidrawSkeleton(
                skeleton, error);
        if (!rendered)
            std::cerr << "Renderer error: " << error.toStdString() << '\n';
        check(rendered.has_value() && error.isEmpty(),
              "bundled renderer could not create a Codex Excalidraw figure");
        check(rendered && !QImage::fromData(rendered->png, "PNG").isNull() &&
                  docxstudio::app::excalidrawSceneFromPng(rendered->png) ==
                      rendered->scene,
              "Codex figure render did not return an editable PNG");
        const auto renderedScene = rendered
            ? QJsonDocument::fromJson(rendered->scene).object()
            : QJsonObject{};
        const auto renderedElements =
            renderedScene.value(QStringLiteral("elements")).toArray();
        check(!renderedElements.isEmpty() &&
                  renderedElements.first().toObject()
                          .value(QStringLiteral("roughness")).toInt(-1) == 0,
              "Codex figure renderer did not enforce Professional mode");

        docxstudio::app::SpellChecker bridgeSpelling;
        docxstudio::app::DocumentCanvas bridgeCanvas(bridgeSpelling);
        const auto bridgeBefore = bridgeCanvas.snapshot();
        const QString bridgeDocumentId =
            QStringLiteral("document:excalidraw-tool-test");
        QString previewSummary;
        const auto toolResult = docxstudio::app::invokeEditorTool(
            bridgeCanvas, bridgeDocumentId,
            QString::fromLatin1(
                docxstudio::codex::kEditorPreviewTool.data(),
                static_cast<qsizetype>(
                    docxstudio::codex::kEditorPreviewTool.size())),
            {{"documentId", bridgeDocumentId.toStdString()},
             {"baseRevision", bridgeBefore.revision.value()},
             {"summary", "Insert an editable process figure"},
             {"operations",
              {{{"type", "insert_excalidraw_figure"},
                {"elements",
                 {{{"type", "rectangle"},
                   {"x", 10}, {"y", 10},
                   {"width", 180}, {"height", 80},
                   {"backgroundColor", "#E7F5FF"},
                   {"label", {{"text", "Professional"}}}},
                  {{"type", "arrow"},
                   {"x", 190}, {"y", 50},
                   {"width", 80}, {"height", 0}}}},
                {"width", 400},
                {"height", 180},
                {"accessibleName", "Codex process figure"}}}}},
            previewSummary, error);
        check(error.isEmpty() && toolResult.value("committed", true) == false &&
                  bridgeCanvas.hasPreview(),
              "Codex Excalidraw operation did not create a review preview");
        check(bridgeCanvas.acceptPreview(error),
              "Codex Excalidraw preview could not be accepted");
        const auto bridgeSnapshot = bridgeCanvas.snapshot();
        const auto& bridgeImages =
            bridgeSnapshot.document.paragraphs().front().images();
        check(bridgeImages.size() == 1 &&
                  docxstudio::app::excalidrawSceneFromPng(
                      atomBytes(bridgeImages.front()))
                      .has_value(),
              "accepted Codex preview was not a native editable figure");
        const QImage insertedPreview =
            QImage::fromData(atomBytes(bridgeImages.front()), "PNG");
        const double rasterAspect =
            static_cast<double>(insertedPreview.width()) /
            static_cast<double>(insertedPreview.height());
        const double displayAspect =
            static_cast<double>(bridgeImages.front().width_emu) /
            static_cast<double>(bridgeImages.front().height_emu);
        check(std::abs(rasterAspect - displayAspect) < 0.001,
              "first Codex figure preview was stretched instead of preserving its aspect ratio");
    }

    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    check(canvas.insertInlineImage(bytes(first),
                                   QStringLiteral("Editable figure")),
          "could not insert editable figure");
    check(canvas.selectedExcalidrawScene() == sceneOne,
          "inserted figure was not recognized as editable");
    int editRequests = 0;
    QObject::connect(
        &canvas,
        &docxstudio::app::DocumentCanvas::editExcalidrawFigureRequested,
        [&editRequests] { ++editRequests; });
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &enter);
    check(editRequests == 1,
          "Enter did not activate the selected editable figure");
    const auto initialRevision = canvas.snapshot().revision;
    check(canvas.replaceSelectedExcalidrawFigure(bytes(second)),
          "could not replace selected figure");
    check(canvas.snapshot().revision.value() == initialRevision.value() + 1 &&
              canvas.selectedExcalidrawScene() == sceneTwo,
          "figure edit was not one document transaction");
    canvas.undo();
    check(canvas.selectedExcalidrawScene() == sceneOne,
          "undo did not restore the prior editable scene");
    canvas.redo();
    check(canvas.selectedExcalidrawScene() == sceneTwo,
          "redo did not restore the edited scene");

    std::string recoveryError;
    const auto recovery = docxstudio::app::RecoveryCodec::encode(
        {canvas.snapshot().document, {}}, recoveryError);
    check(recovery.has_value(),
          "recovery could not serialize an editable figure");
    const auto restored = docxstudio::app::RecoveryCodec::decode(
        *recovery, recoveryError);
    check(restored.has_value(),
          "recovery could not restore an editable figure");
    const auto& restoredImage =
        restored->document.paragraphs().front().images().front();
    check(docxstudio::app::excalidrawSceneFromPng(
              atomBytes(restoredImage)) == sceneTwo,
          "recovery lost the editable scene");

    QTemporaryDir directory;
    check(directory.isValid(), "could not create DOCX test directory");
    docxstudio::ooxml::NewInlineImage authoredImage;
    authoredImage.format = docxstudio::raster::Format::png;
    authoredImage.name = "Excalidraw figure";
    authoredImage.accessible_name = "Editable figure";
    authoredImage.width_emu = 1'016'000;
    authoredImage.height_emu = 508'000;
    authoredImage.bytes = bytes(second);
    docxstudio::ooxml::NewRun imageRun{
        "", {}, std::nullopt, std::move(authoredImage)};
    docxstudio::ooxml::NewParagraph paragraph;
    paragraph.runs.push_back(std::move(imageRun));
    const auto path =
        std::filesystem::path(directory.filePath(
            QStringLiteral("figure.docx")).toStdString());
    const auto saved =
        docxstudio::ooxml::DocxDocument::writeNew(path, {paragraph});
    check(saved.saved, "could not write figure DOCX");
    docxstudio::ooxml::Error openError;
    const auto opened =
        docxstudio::ooxml::DocxDocument::open(path, &openError);
    check(opened && !opened->paragraphs().empty(),
          "could not reopen figure DOCX");
    const auto& fragment =
        opened->paragraphs().front().runs.front().fragments.front();
    check(fragment.inline_image.has_value() &&
              fragment.inline_image->bytes == bytes(second),
          "DOCX round-trip changed figure PNG bytes");
    const auto roundTripped = QByteArray(
        reinterpret_cast<const char*>(
            fragment.inline_image->bytes.data()),
        static_cast<qsizetype>(fragment.inline_image->bytes.size()));
    check(docxstudio::app::excalidrawSceneFromPng(
              roundTripped) == sceneTwo,
          "DOCX round-trip lost editable figure source");

    docxstudio::app::MainWindow window;
    const auto* action =
        window.findChild<QAction*>(QStringLiteral("insert.excalidraw"));
    check(action && action->text().contains(
              QStringLiteral("Excalidraw"), Qt::CaseInsensitive),
          "Insert Excalidraw Figure command is missing");
    check(window.findChild<QAction*>(
              QStringLiteral("picture.editExcalidraw")) != nullptr,
          "Picture-ribbon Edit Figure command is missing");

    std::cout << "Excalidraw figure tests passed\n";
    return 0;
}
