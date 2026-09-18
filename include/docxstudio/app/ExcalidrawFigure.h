#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace docxstudio::app {

inline constexpr qsizetype kMaximumExcalidrawSceneBytes =
    8 * 1024 * 1024;

// Stores inert Excalidraw JSON in an ancillary PNG iTXt chunk. Ordinary DOCX
// consumers see a normal PNG; Owl Docs can recover the editable source without
// a sidecar file or executable content in the document.
[[nodiscard]] std::optional<QByteArray> excalidrawSceneFromPng(
    const QByteArray& png);
[[nodiscard]] QByteArray pngWithExcalidrawScene(
    const QByteArray& png, const QByteArray& scene, QString& error);

// Resolves optional agent-requested dimensions without ever stretching the
// exported scene. When both dimensions are present they form a bounding box.
[[nodiscard]] std::optional<QSizeF> fitExcalidrawFigureDisplaySize(
    QSize rasterSize, std::optional<double> widthPoints,
    std::optional<double> heightPoints);

class ExcalidrawFigureEditor final : public QObject {
public:
    struct Result {
        QByteArray png;
        QByteArray scene;
    };
    using Completion =
        std::function<void(std::optional<Result>, const QString&)>;

    explicit ExcalidrawFigureEditor(QObject* parent = nullptr);
    ~ExcalidrawFigureEditor() override;

    [[nodiscard]] bool busy() const noexcept;
    void open(std::optional<QByteArray> scene, Completion completion);
    void renderProfessional(const QJsonArray& skeleton,
                            Completion completion);

private:
    struct Session;
    void start(std::optional<QByteArray> scene,
               std::optional<QJsonArray> skeleton, bool renderOnly,
               Completion completion);
    void finishSession(int exitCode);

    std::unique_ptr<Session> session_;
};

// Runs the bundled offline renderer through a short nested Qt event loop. This
// is used only while answering a synchronous app-server dynamic-tool call; the
// generated figure still enters the normal user-visible preview lifecycle.
[[nodiscard]] std::optional<ExcalidrawFigureEditor::Result>
renderProfessionalExcalidrawSkeleton(const QJsonArray& skeleton,
                                     QString& error);

}  // namespace docxstudio::app
