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
        const auto bridgeParagraph =
            bridgeBefore.document.paragraphs().front().id().toString();
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
              {{{"kind", "insert_excalidraw_figure"},
                {"target", {{"blockId", bridgeParagraph}, {"start", 0}}},
                {"figure",
                 {{"accessibleName", "Codex process figure"},
                  {"widthPoints", 300},
                  {"elements",
                   {{{"id", "tool-box"},
                     {"type", "rectangle"},
                     {"x", 10}, {"y", 10},
                     {"width", 180}, {"height", 80},
                     {"label", {{"text", "Professional"}}}}}}}}}}}},
            previewSummary, error);
        check(error.isEmpty() && toolResult.value("committed", true) == false &&
                  bridgeCanvas.hasPreview(),
              "Codex Excalidraw operation did not create a review preview");
        check(bridgeCanvas.acceptPreview(error),
              "Codex Excalidraw preview could not be accepted");
        const auto& bridgeImages =
            bridgeCanvas.snapshot().document.paragraphs().front().images();
        check(bridgeImages.size() == 1 &&
                  docxstudio::app::excalidrawSceneFromPng(
                      atomBytes(bridgeImages.front()))
                      .has_value(),
              "accepted Codex preview was not a native editable figure");
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
