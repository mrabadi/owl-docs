#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <optional>

namespace docxstudio::app {
struct EditableExcalidrawScene {
    QJsonArray elements;
    QStringList unsupported;
    bool truncated{false};
    bool complete() const { return unsupported.isEmpty() && !truncated; }
};
[[nodiscard]] EditableExcalidrawScene inspectExcalidrawScene(const QByteArray& source);
struct ExcalidrawScenePatch {
    QByteArray scene;
    QStringList renderElementIds;
    QStringList dependentElementIds;
};
[[nodiscard]] std::optional<ExcalidrawScenePatch> prepareExcalidrawScenePatch(
    const QByteArray& source, const QJsonArray& edits, QString& error);
[[nodiscard]] QByteArray finishExcalidrawScenePatch(
    const ExcalidrawScenePatch& patch, const QByteArray& renderedScene, QString& error);
}  // namespace docxstudio::app
