#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace docxstudio::app {

inline constexpr qsizetype kMaximumExcalidrawSceneBytes =
    8 * 1024 * 1024;

// Stores inert Excalidraw JSON in an ancillary PNG iTXt chunk. Ordinary DOCX
// consumers see a normal PNG; OwlDocs can recover the editable source without
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

    // The optional executable is a trusted C++ process-boundary injection. It
    // is never read from document data, model arguments or app configuration.
    explicit ExcalidrawFigureEditor(QObject* parent = nullptr,
                                   QString executableOverride = {});
    ~ExcalidrawFigureEditor() override;

    [[nodiscard]] bool busy() const noexcept;
    void open(std::optional<QByteArray> scene, Completion completion);
    void renderProfessional(const QJsonArray& skeleton,
                            Completion completion);
    void renderProfessionalScene(const QByteArray& scene,
                                 const QStringList& patchElementIds,
                                 Completion completion);

private:
    struct Session;
    void start(std::optional<QByteArray> scene,
               std::optional<QJsonArray> skeleton, bool renderOnly,
               Completion completion,
               std::optional<QStringList> patchElementIds = std::nullopt);
    void finishSession(int exitCode);

    std::unique_ptr<Session> session_;
    QString executableOverride_;
};

// Injectable process boundary for deterministic editor-tool verification. An
// empty renderer uses the bundled offline Professional renderer below.
using ExcalidrawRenderer = std::function<
    std::optional<ExcalidrawFigureEditor::Result>(const QJsonArray&, QString&)>;
using ExcalidrawSceneRenderer = std::function<
    std::optional<ExcalidrawFigureEditor::Result>(
        const QByteArray&, const QStringList&, QString&)>;

// Runs the bundled offline renderer through a short nested Qt event loop. This
// is used only while answering a synchronous app-server dynamic-tool call; the
// generated figure still enters the normal user-visible preview lifecycle.
[[nodiscard]] std::optional<ExcalidrawFigureEditor::Result>
renderProfessionalExcalidrawSkeleton(const QJsonArray& skeleton,
                                     QString& error,
                                     const QString& executableOverride = {});
[[nodiscard]] std::optional<ExcalidrawFigureEditor::Result>
renderProfessionalExcalidrawScene(const QByteArray& scene,
                                const QStringList& patchElementIds,
                                QString& error,
                                const QString& executableOverride = {});

}  // namespace docxstudio::app
