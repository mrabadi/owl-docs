#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/codex/editor_tools.hpp"

#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdlib>
#include <iostream>

using namespace docxstudio;
namespace {
using codex::Json;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
QByteArray raster(const QColor& color = Qt::white) {
    QImage image(200, 100, QImage::Format_ARGB32); image.fill(color);
    QByteArray result; QBuffer buffer(&result); buffer.open(QIODevice::WriteOnly);
    check(image.save(&buffer, "PNG"), "could not create patch raster"); return result;
}
std::vector<std::uint8_t> bytes(const QByteArray& source) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(source.constData());
    return {first, first + source.size()};
}
QByteArray payload(const core::ImageAtom& image) {
    const auto source = image.encoded_payload.bytes();
    return {reinterpret_cast<const char*>(source.data()), static_cast<qsizetype>(source.size())};
}
QJsonObject scene(bool complex = true) {
    QJsonObject box{{"id", "box"}, {"type", "rectangle"}, {"x", 20}, {"y", 20}, {"width", 160}, {"height", 60},
        {"roughness", 0}, {"fillStyle", "solid"}, {"strokeColor", "#222624"}, {"backgroundColor", "#ffffff"},
        {"strokeWidth", 2}, {"opacity", 100}, {"angle", 0}, {"groupIds", QJsonArray{}},
        {"boundElements", QJsonArray{QJsonObject{{"id", "label"}, {"type", "text"}}}}};
    QJsonObject label{{"id", "label"}, {"type", "text"}, {"containerId", "box"}, {"x", 30}, {"y", 30}, {"width", 140}, {"height", 20},
        {"text", "Original"}, {"originalText", "Original"}, {"fontFamily", 2}, {"fontSize", 20}, {"textAlign", "center"},
        {"verticalAlign", "middle"}, {"autoResize", true}, {"lineHeight", 1.15}, {"strokeColor", "#222624"}, {"opacity", 100}};
    QJsonArray elements{box, label};
    QJsonObject result{{"type", "excalidraw"}, {"version", 2}, {"elements", elements}, {"appState", QJsonObject{}}, {"files", QJsonObject{}}};
    if (complex) {
        box["groupIds"] = QJsonArray{"retained-group"}; box["customData"] = QJsonObject{{"owner", "human"}};
        QJsonObject untouched{{"id", "untouched"}, {"type", "freedraw"}, {"x", 220}, {"y", 20}, {"width", 30}, {"height", 40},
            {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{30, 40}}}, {"pressures", QJsonArray{}}, {"simulatePressure", true},
            {"strokeColor", "#222624"}, {"backgroundColor", "transparent"}, {"strokeWidth", 3}, {"fillStyle", "solid"},
            {"roughness", 0}, {"opacity", 100}, {"angle", 0}, {"groupIds", QJsonArray{}}, {"seed", 1},
            {"customData", QJsonObject{{"exact", "retained"}}}};
        result["elements"] = QJsonArray{box, label, untouched};
        result["files"] = QJsonObject{{"asset", QJsonObject{{"id", "asset"}, {"mimeType", "image/png"}, {"created", 1}, {"lastRetrieved", 1},
            {"dataURL", QStringLiteral("data:image/png;base64,") + QString::fromLatin1(raster(Qt::magenta).toBase64())}, {"custom", "retained"}}}};
        result["customRoot"] = "retained";
    }
    return result;
}
QByteArray encode(const QJsonObject& source) { return QJsonDocument(source).toJson(QJsonDocument::Compact); }
Json propose(app::DocumentCanvas& canvas, const Json& operation, QString& summary, QString& error,
             const app::ExcalidrawRenderer& renderer = {}, const app::ExcalidrawSceneRenderer& native = {},
             const std::function<bool()>& stillCurrent = {}) {
    const auto result = app::invokeEditorTool(canvas, QStringLiteral("document:figure-patch"),
        QString::fromLatin1(codex::kEditorPreviewTool.data()),
        {{"documentId", "document:figure-patch"}, {"baseRevision", canvas.snapshot().revision.value()},
         {"summary", "Edit the existing figure"}, {"operations", Json::array({operation})}}, summary, error, renderer, stillCurrent, native);
    if (result.empty() && !error.isEmpty()) std::cerr << "Tool: " << error.toStdString() << '\n';
    return result;
}
void nativePatches() {
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    const auto original = scene();
    const auto source = encode(original);
    QString error, summary;
    const auto embedded = app::pngWithExcalidrawScene(raster(), source, error);
    check(canvas.insertInlineImage(bytes(embedded), QStringLiteral("Human figure")), "could not insert native patch fixture");
    const auto before = canvas.snapshot();
    const auto imageId = before.document.paragraphs().front().images().front().id;
    int renders = 0;
    const app::ExcalidrawSceneRenderer renderer = [&](const QByteArray& edited, const QStringList& ids, QString& renderError)
        -> std::optional<app::ExcalidrawFigureEditor::Result> {
        ++renders; renderError.clear();
        auto result = QJsonDocument::fromJson(edited).object();
        check(ids == QStringList{QStringLiteral("label")}, "native renderer did not receive exact changed IDs");
        auto elements = result.value("elements").toArray();
        check(elements.at(1).toObject().value("originalText") == "A longer edited label", "native text patch omitted originalText");
        auto text = elements.at(1).toObject(); text["width"] = 180; text["text"] = "A longer\nedited label"; elements[1] = text;
        auto box = elements.at(0).toObject(); box["height"] = 100; box["groupIds"] = QJsonArray{"must-not-copy"}; elements[0] = box;
        auto untouched = elements.at(2).toObject(); untouched["x"] = 999; elements[2] = untouched;
        result["elements"] = elements; result["files"] = QJsonObject{};
        return app::ExcalidrawFigureEditor::Result{raster(Qt::green), encode(result)};
    };
    Json operation{{"kind", "patch_excalidraw_figure"}, {"imageId", imageId.toString()},
        {"edits", {{{"id", "label"}, {"changes", {{"text", "A longer edited label"}}}}}}};
    auto invalid = operation; invalid["imageId"] = core::NodeId::generate().toString();
    check(propose(canvas, invalid, summary, error, {}, renderer).empty() && renders == 0 && error.contains("no longer exists"),
          "native patch did not validate image identity before rendering");
    auto result = propose(canvas, operation, summary, error, {}, renderer);
    check(!result.empty() && error.isEmpty() && canvas.hasPreview() && canvas.snapshot().document == before.document,
          "native scene patch did not create an isolated review preview");
    canvas.discardPreview();
    check(canvas.snapshot().document == before.document, "discarding native scene patch changed source bytes");
    result = propose(canvas, operation, summary, error, {}, renderer);
    check(!result.empty() && canvas.acceptPreview(error), "could not accept native scene patch");
    const auto after = canvas.snapshot();
    const auto* image = after.document.findImage(imageId);
    check(image && image->accessible_name == "Human figure", "native patch changed image identity or accessible name");
    const auto patched = app::excalidrawSceneFromPng(payload(*image));
    check(patched.has_value(), "native patch did not embed its authoritative editable scene");
    const auto output = QJsonDocument::fromJson(*patched).object();
    check(output.value("files") == original.value("files") && output.value("customRoot") == original.value("customRoot") &&
              output.value("elements").toArray().at(2) == original.value("elements").toArray().at(2) &&
              output.value("elements").toArray().at(0).toObject().value("groupIds") == original.value("elements").toArray().at(0).toObject().value("groupIds") &&
              output.value("elements").toArray().at(0).toObject().value("height") == 100,
          "native patch copied unrelated renderer normalization or lost dependent geometry");
    canvas.undo(); check(canvas.snapshot().document == before.document, "native patch was not one Undo");
    canvas.redo(); check(canvas.snapshot().document == after.document, "native patch Redo lost source bytes");
    canvas.undo();
    bool currentSession = true;
    result = propose(canvas, operation, summary, error, {},
        [&](const QByteArray&, const QStringList&, QString& renderError) -> std::optional<app::ExcalidrawFigureEditor::Result> {
            currentSession = false; renderError.clear(); return app::ExcalidrawFigureEditor::Result{embedded, source};
        }, [&] { return currentSession; });
    check(result.empty() && error.contains("cancel") && !canvas.hasPreview() && canvas.snapshot().document == before.document,
          "disconnected Codex session created a late native figure preview");
    auto external = original;
    external["files"] = QJsonObject{{"remote", QJsonObject{{"dataURL", "https://example.invalid/image.png"}}}};
    auto externalDocument = before.document;
    const auto externalPng = app::pngWithExcalidrawScene(raster(), encode(external), error);
    check(bool(externalDocument.replaceImagePayload(imageId, core::EncodedImagePayload(bytes(externalPng)), core::ImageFormat::png,
        before.document.findImage(imageId)->width_emu, before.document.findImage(imageId)->height_emu)), "could not set remote asset fixture");
    canvas.setDocument(externalDocument);
    const int previousRenders = renders;
    check(propose(canvas, operation, summary, error, {}, renderer).empty() && error.contains("self-contained") &&
              renders == previousRenders && canvas.snapshot().document == externalDocument,
          "native patch launched a renderer for external scene assets");

    auto* closing = new app::DocumentCanvas(spelling); closing->setDocument(before.document);
    result = propose(*closing, operation, summary, error, {},
        [&](const QByteArray&, const QStringList&, QString& renderError) -> std::optional<app::ExcalidrawFigureEditor::Result> {
            delete closing; renderError.clear(); return app::ExcalidrawFigureEditor::Result{embedded, source};
        });
    check(result.empty() && error.contains("closed"), "native renderer accessed a closed document canvas");
}
void reconstruction() {
    app::SpellChecker spelling; app::DocumentCanvas canvas(spelling);
    const auto original = scene(false);
    QString summary, error;
    const auto sourcePng = app::pngWithExcalidrawScene(raster(), encode(original), error);
    check(canvas.insertInlineImage(bytes(sourcePng), QStringLiteral("Stable figure")), "could not create reconstruction fixture");
    const auto before = canvas.snapshot(); const auto imageId = before.document.paragraphs().front().images().front().id;
    Json operation{{"kind", "replace_excalidraw_figure"}, {"imageId", imageId.toString()},
        {"figure", {{"elements", {{{"id", "replacement"}, {"type", "rectangle"}, {"x", 20}, {"y", 20}, {"width", 80}, {"height", 40}}}}}}};
    int renders = 0;
    const app::ExcalidrawRenderer renderer = [&](const QJsonArray& skeleton, QString& renderError) -> std::optional<app::ExcalidrawFigureEditor::Result> {
        ++renders; renderError.clear(); auto output = original; output["elements"] = skeleton;
        const auto replacement = encode(output);
        return app::ExcalidrawFigureEditor::Result{app::pngWithExcalidrawScene(raster(Qt::blue), replacement, renderError), replacement};
    };
    check(propose(canvas, operation, summary, error, renderer).empty() && renders == 0 && error.contains("identit"),
          "partial skeleton silently replaced original scene/label identities");
    operation["reconstructScene"] = true;
    const auto result = propose(canvas, operation, summary, error, renderer);
    check(!result.empty() && canvas.snapshot().document == before.document && !result.at("warnings").empty() &&
              summary.contains("box") && summary.contains("label"), "explicit reconstruction has no visible specific loss report");
    check(canvas.acceptPreview(error) && canvas.snapshot().document.findImage(imageId)->accessible_name == "Stable figure",
          "figure reconstruction rewrote its accessible name without an explicit change");
    canvas.undo(); check(canvas.snapshot().document == before.document, "explicit reconstruction was not one Undo");
    const auto complexPng = app::pngWithExcalidrawScene(raster(), encode(scene()), error);
    auto complexDocument = before.document;
    check(bool(complexDocument.replaceImagePayload(imageId, core::EncodedImagePayload(bytes(complexPng)), core::ImageFormat::png,
        before.document.findImage(imageId)->width_emu, before.document.findImage(imageId)->height_emu)), "could not create complex reconstruction fixture");
    canvas.setDocument(complexDocument); operation["reconstructScene"] = false;
    const int previousRenders = renders;
    check(propose(canvas, operation, summary, error, renderer).empty() && renders == previousRenders && error.contains("patch"),
          "complex imported scene was destructively replaced without an explicit reconstruction");
}
void writeArtifact(const QString& path, const QByteArray& value) {
    QFile file(path); check(file.open(QIODevice::WriteOnly) && file.write(value) == value.size(), "could not retain figure artifact");
}
void realRenderer() {
    const QString executable = qEnvironmentVariable("OWL_DOCS_TEST_FIGURE_EDITOR_PATH");
    check(!executable.isEmpty(), "real gate requires explicit fresh helper path");
    const QString directory = QDir::current().absoluteFilePath(QStringLiteral("output/codex-review/0.6.5/figures"));
    check(QDir().mkpath(directory), "could not create figure review directory");
    app::MainWindow window; window.resize(1280, 860); window.show();
    auto* canvas = window.findChild<app::DocumentCanvas*>(); check(canvas, "could not find real figure canvas");
    QString error, summary;
    const app::ExcalidrawRenderer professional = [&](const QJsonArray& skeleton, QString& renderError) {
        return app::renderProfessionalExcalidrawSkeleton(skeleton, renderError, executable);
    };
    const app::ExcalidrawSceneRenderer native = [&](const QByteArray& source, const QStringList& ids, QString& renderError) {
        return app::renderProfessionalExcalidrawScene(source, ids, renderError, executable);
    };
    const auto paragraphId = canvas->snapshot().document.paragraphs().front().id();
    const auto generatedId = core::NodeId::generate();
    Json create{{"kind", "insert_excalidraw_figure"}, {"imageId", generatedId.toString()},
        {"target", {{"blockId", paragraphId.toString()}, {"start", 0}}},
        {"figure", {{"accessibleName", "Editable process"}, {"widthPoints", 300},
            {"elements", {{{"id", "process"}, {"type", "rectangle"}, {"x", 20}, {"y", 20}, {"width", 220}, {"height", 100},
                {"backgroundColor", "#E7F5FF"}, {"label", {{"id", "process-label"}, {"text", "Codex process"}}}}}}}}};
    create["figure"]["elements"].push_back({{"id", "flow"}, {"type", "arrow"}, {"x", 240}, {"y", 70},
        {"points", {{0, 0}, {120, 0}}}, {"label", {{"id", "flow-label"}, {"text", "Supports"}, {"fontSize", 18}}}});
    auto result = propose(*canvas, create, summary, error, professional);
    check(!result.empty() && canvas->hasPreview(), "fresh helper create did not enter review");
    check(canvas->grab().save(directory + QStringLiteral("/create-preview.png")), "could not retain create preview");
    canvas->discardPreview();
    check(!canvas->snapshot().document.findImage(generatedId), "discarding real create changed document");
    result = propose(*canvas, create, summary, error, professional);
    check(!result.empty() && canvas->acceptPreview(error), "could not accept fresh helper create");
    const auto created = canvas->snapshot();
    writeArtifact(directory + QStringLiteral("/created.png"), payload(*created.document.findImage(generatedId)));
    auto read = app::invokeEditorTool(*canvas, QStringLiteral("document:figure-patch"), QString::fromLatin1(codex::kEditorReadTool.data()),
        {{"documentId", "document:figure-patch"}, {"scope", "images"}, {"imageIds", {generatedId.toString()}}}, summary, error);
    const auto metadata = read.at("blocks").front().at("attributes");
    if (!metadata.value("editableElementsComplete", false)) std::cerr << "Incomplete real scene: " << metadata.dump() << '\n';
    check(metadata.value("editableElementsComplete", false), "fresh Professional source is not safely editable through its canonical read");
    auto editable = metadata.at("editableElements"); editable.at(0)["label"]["text"] = "Revised process";
    Json replace{{"kind", "replace_excalidraw_figure"}, {"imageId", generatedId.toString()}, {"figure", {{"elements", editable}}}};
    result = propose(*canvas, replace, summary, error, professional);
    check(!result.empty() && canvas->snapshot().document == created.document, "fresh helper replacement changed document before review");
    canvas->discardPreview(); check(canvas->snapshot().document == created.document, "real replacement discard changed bytes");
    result = propose(*canvas, replace, summary, error, professional);
    check(!result.empty() && canvas->acceptPreview(error), "could not accept actual helper replacement");
    const auto replaced = canvas->snapshot();
    canvas->undo(); check(canvas->snapshot().document == created.document, "real replacement was not one Undo");
    canvas->redo(); check(canvas->snapshot().document == replaced.document, "real replacement Redo lost source");
    writeArtifact(directory + QStringLiteral("/replaced.png"), payload(*replaced.document.findImage(generatedId)));

    auto original = scene();
    original["appState"] = QJsonObject{{"viewBackgroundColor", "#DDEEDD"}, {"exportWithDarkMode", false}};
    auto elements = original.value("elements").toArray();
    auto box = elements.at(0).toObject();
    box["boundElements"] = QJsonArray{QJsonObject{{"id", "label"}, {"type", "text"}}, QJsonObject{{"id", "connector"}, {"type", "arrow"}}};
    elements[0] = box;
    elements.append(QJsonObject{{"id", "embedded-image"}, {"type", "image"}, {"fileId", "asset"}, {"status", "saved"},
        {"x", 280}, {"y", 90}, {"width", 60}, {"height", 30}, {"scale", QJsonArray{1, 1}}, {"angle", 0}, {"opacity", 100}});
    elements.append(QJsonObject{{"id", "connector"}, {"type", "arrow"}, {"x", 180}, {"y", 50}, {"width", 80}, {"height", 0},
        {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{80, 0}}}, {"startBinding", QJsonObject{{"elementId", "box"}, {"focus", 0}, {"gap", 5}}},
        {"endBinding", QJsonValue::Null}, {"startArrowhead", QJsonValue::Null}, {"endArrowhead", "arrow"},
        {"strokeColor", "#222624"}, {"backgroundColor", "transparent"}, {"strokeWidth", 2}, {"roughness", 0}, {"fillStyle", "solid"}, {"opacity", 100}, {"angle", 0}});
    original["elements"] = elements;
    const auto originalSource = encode(original);
    const auto beforeRender = native(originalSource, {QStringLiteral("box")}, error);
    if (!beforeRender) std::cerr << "Native renderer: " << error.toStdString() << '\n';
    check(beforeRender.has_value(), "fresh helper cannot render native source");
    const auto beforePng = app::pngWithExcalidrawScene(beforeRender->png, originalSource, error);
    QImage beforeImage = QImage::fromData(beforePng);
    bool magenta = false;
    for (int y = 0; y < beforeImage.height(); ++y) for (int x = 0; x < beforeImage.width(); ++x)
        magenta = magenta || beforeImage.pixelColor(x, y) == QColor(Qt::magenta);
    check(beforeImage.pixelColor(0, 0) == QColor(QStringLiteral("#DDEEDD")) && magenta,
          "native PNG lost source background or embedded image assets");
    const QRect freehandRegion(230, 28, 20, 14);
    bool visibleFreehand = false;
    for (int y = freehandRegion.top(); y <= freehandRegion.bottom(); ++y) for (int x = freehandRegion.left(); x <= freehandRegion.right(); ++x) {
        const auto color = beforeImage.pixelColor(x, y);
        visibleFreehand = visibleFreehand || (color.red() < 100 && color.green() < 100 && color.blue() < 100);
    }
    check(visibleFreehand, "native fixture did not render its unsupported freehand drawing");
    check(canvas->insertInlineImage(bytes(beforePng), QStringLiteral("Human native source")), "could not insert rendered native source");
    const auto beforePatch = canvas->snapshot();
    core::NodeId nativeId;
    for (const auto& image : beforePatch.document.paragraphs().front().images())
        if (image.accessible_name == "Human native source") nativeId = image.id;
    check(nativeId.isValid() && nativeId != generatedId, "could not identify newly inserted native source by its stable image identity");
    const std::string longLabel = "A longer academic figure label that wraps across several lines and expands its container while keeping the embedded asset, custom background, connected arrow, native groups and unsupported drawing unchanged.";
    Json patch{{"kind", "patch_excalidraw_figure"}, {"imageId", nativeId.toString()},
        {"edits", {{{"id", "label"}, {"changes", {{"text", longLabel}}}}}}};
    result = propose(*canvas, patch, summary, error, {}, native);
    check(!result.empty() && canvas->snapshot().document == beforePatch.document, "real native patch did not isolate its preview");
    check(canvas->grab().save(directory + QStringLiteral("/native-preview.png")), "could not retain native preview");
    canvas->discardPreview(); check(canvas->snapshot().document == beforePatch.document, "real native discard changed source");
    result = propose(*canvas, patch, summary, error, {}, native);
    check(!result.empty() && canvas->acceptPreview(error), "real native patch was not accepted");
    const auto accepted = canvas->snapshot();
    const auto acceptedPng = payload(*accepted.document.findImage(nativeId));
    const QImage acceptedImage = QImage::fromData(acceptedPng);
    magenta = false;
    for (int y = 0; y < acceptedImage.height(); ++y) for (int x = 0; x < acceptedImage.width(); ++x)
        magenta = magenta || acceptedImage.pixelColor(x, y) == QColor(Qt::magenta);
    check(acceptedImage.pixelColor(0, 0) == QColor(QStringLiteral("#DDEEDD")) && magenta,
          "native patch PNG disagrees with its preserved source background/assets");
    check(acceptedImage.copy(freehandRegion) == beforeImage.copy(freehandRegion), "native patch altered unrelated freehand pixels");
    const auto acceptedSource = app::excalidrawSceneFromPng(acceptedPng);
    check(acceptedSource.has_value(), "real native patch lost editable source");
    const auto finalScene = QJsonDocument::fromJson(*acceptedSource).object();
    check(finalScene.value("files") == original.value("files") && finalScene.value("customRoot") == original.value("customRoot") &&
              finalScene.value("elements").toArray().at(2) == elements.at(2) &&
              finalScene.value("elements").toArray().at(1).toObject().value("originalText").toString() == QString::fromStdString(longLabel) &&
              finalScene.value("elements").toArray().at(1).toObject().value("height").toDouble() > 20 &&
              finalScene.value("elements").toArray().at(0).toObject().value("height").toDouble() > 60 &&
              (finalScene.value("elements").toArray().at(4).toObject().value("x") != elements.at(4).toObject().value("x") ||
               finalScene.value("elements").toArray().at(4).toObject().value("y") != elements.at(4).toObject().value("y") ||
               finalScene.value("elements").toArray().at(4).toObject().value("points") != elements.at(4).toObject().value("points")),
          "real native patch lost unrelated content or failed to remeasure the changed label");
    canvas->undo(); check(canvas->snapshot().document == beforePatch.document, "real native patch was not one Undo");
    canvas->redo(); check(canvas->snapshot().document == accepted.document, "real native patch Redo lost authoritative source");
    writeArtifact(directory + QStringLiteral("/native-before.png"), beforePng);
    writeArtifact(directory + QStringLiteral("/native-after.png"), acceptedPng);
    writeArtifact(directory + QStringLiteral("/native-before.excalidraw"), originalSource);
    writeArtifact(directory + QStringLiteral("/native-after.excalidraw"), *acceptedSource);
    std::string codecError;
    const auto checkpoint = app::RecoveryCodec::encode({accepted.document, {}}, codecError);
    const auto recovered = checkpoint ? app::RecoveryCodec::decode(*checkpoint, codecError) : std::nullopt;
    check(recovered && recovered->document == accepted.document, "real rendered native source changed through recovery");
    writeArtifact(directory + QStringLiteral("/accepted-recovery.json"), QByteArray::fromStdString(*checkpoint));
    check(canvas->exportPdf(directory + QStringLiteral("/figures.pdf"), error), "real figure PDF export failed");
    auto* save = window.findChild<QAction*>(QStringLiteral("file.saveAs")); check(save, "could not find manual Save As");
    const QString docx = directory + QStringLiteral("/figures.docx");
    QFile::remove(docx);
    QTimer::singleShot(0, &window, [&] { auto* dialog = window.findChild<QFileDialog*>(); check(dialog, "manual Save As dialog missing");
        dialog->selectFile(docx); check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection), "could not choose native DOCX destination"); });
    save->trigger();
    app::MainWindow reopened; check(reopened.openPath(docx), "rendered figures DOCX did not reopen");
    const auto* restored = reopened.findChild<app::DocumentCanvas*>();
    check(restored, "reopened DOCX has no document canvas");
    const auto reopenedDocument = restored->snapshot().document;
    const auto reopenedCheckpoint = app::RecoveryCodec::encode({reopenedDocument, {}}, codecError);
    check(reopenedCheckpoint.has_value(), "could not record reopened figure diagnostics");
    writeArtifact(directory + QStringLiteral("/reopened-recovery.json"), QByteArray::fromStdString(*reopenedCheckpoint));
    // Basic Word files store numeric drawing IDs, not OwlDocs core GUIDs.
    // The native scene identities and every figure property must still survive.
    check(reopenedDocument.paragraphs().size() == accepted.document.paragraphs().size(), "DOCX reopen changed figure paragraphs");
    for (std::size_t index = 0; index < accepted.document.paragraphs().size(); ++index) {
        const auto& expected = accepted.document.paragraphs().at(index);
        const auto& actual = reopenedDocument.paragraphs().at(index);
        check(actual.text() == expected.text() && actual.images().size() == expected.images().size(), "DOCX reopen changed figure positions/count");
        for (const auto& picture : expected.images()) {
            const core::ImageAtom* found = nullptr;
            for (const auto& candidate : actual.images()) if (candidate.accessible_name == picture.accessible_name) found = &candidate;
            check(found && payload(*found) == payload(picture) && found->format == picture.format &&
                      found->utf16_offset == picture.utf16_offset && found->width_emu == picture.width_emu &&
                      found->height_emu == picture.height_emu && found->layout == picture.layout &&
                      app::excalidrawSceneFromPng(payload(*found)) == app::excalidrawSceneFromPng(payload(picture)),
                  "DOCX reopen changed a real figure's bytes, native identities, alt text, layout, dimensions or offset");
        }
    }
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(QStringLiteral("OwlDocsCodexFigurePatchTests"));
    if (application.arguments().contains(QStringLiteral("--real-renderer"))) realRenderer();
    else if (application.arguments().contains(QStringLiteral("--reconstruction"))) reconstruction();
    else nativePatches();
    std::cout << "Codex figure patch tests passed\n";
}
