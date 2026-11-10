#include "docxstudio/app/ExcalidrawScene.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <numbers>

namespace docxstudio::app {
namespace {
constexpr qsizetype sceneLimit = 8 * 1024 * 1024;
constexpr qsizetype editsLimit = 256 * 1024;
bool number(const QJsonValue& value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
        value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool color(const QJsonValue& value, bool transparent = false) {
    static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return value.isString() && ((transparent && value.toString() == QLatin1String("transparent")) ||
        expression.match(value.toString()).hasMatch());
}
bool points(const QJsonValue& value, qsizetype maximum) {
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    if (array.size() < 2 || array.size() > maximum) return false;
    for (const auto point : array) {
        if (!point.isArray() || point.toArray().size() != 2 ||
            !number(point.toArray().at(0), -10000, 10000) ||
            !number(point.toArray().at(1), -10000, 10000)) return false;
    }
    return true;
}
bool parse(const QByteArray& source, QJsonObject& result, QString& error) {
    if (source.isEmpty() || source.size() > sceneLimit) {
        error = QStringLiteral("Excalidraw scene must be nonempty and at most 8 MiB."); return false;
    }
    QJsonParseError failure;
    const auto document = QJsonDocument::fromJson(source, &failure);
    if (failure.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("elements")).isArray()) {
        error = QStringLiteral("Excalidraw scene must contain a valid elements array."); return false;
    }
    result = document.object(); return true;
}
bool activeElements(const QJsonArray& elements, QMap<QString, QJsonObject>& result, QString& error) {
    QSet<QString> seen;
    for (const auto value : elements) {
        if (!value.isObject()) { error = QStringLiteral("Excalidraw scene contains an invalid element."); return false; }
        const auto element = value.toObject();
        const auto id = element.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id.size() > 256 || seen.contains(id)) {
            error = QStringLiteral("Excalidraw element identities are missing or duplicated."); return false;
        }
        seen.insert(id);
        if (!element.value(QStringLiteral("isDeleted")).toBool()) result.insert(id, element);
    }
    return true;
}
QString sourceText(const QJsonObject& element) {
    const auto original = element.value(QStringLiteral("originalText"));
    return original.isString() ? original.toString() : element.value(QStringLiteral("text")).toString();
}
bool present(const QJsonValue& value) { return !value.isUndefined() && !value.isNull(); }
bool nonempty(const QJsonValue& value) {
    if (!present(value)) return false;
    if (value.isArray()) return !value.toArray().isEmpty();
    if (value.isObject()) return !value.toObject().isEmpty();
    if (value.isString()) return !value.toString().isEmpty();
    return value.toBool(true);
}
bool supportedText(const QJsonObject& element, bool bound) {
    const auto text = sourceText(element);
    if (text.isEmpty() || text.size() > (bound ? 2000 : 4000) ||
        !number(element.value(QStringLiteral("fontSize")), 8, 96) ||
        element.value(QStringLiteral("fontFamily")).toInt() != 2) return false;
    const auto differs = [&element](const char* key, const QString& expected) {
        const auto value = element.value(QLatin1String(key)); return present(value) && value.toString() != expected;
    };
    return !differs("fontStyle", QStringLiteral("normal")) &&
        (!present(element.value(QStringLiteral("fontWeight"))) || element.value(QStringLiteral("fontWeight")) == QJsonValue(QStringLiteral("normal")) || element.value(QStringLiteral("fontWeight")) == QJsonValue(400)) &&
        !differs("textAlign", bound ? QStringLiteral("center") : QStringLiteral("left")) &&
        !differs("verticalAlign", bound ? QStringLiteral("middle") : QStringLiteral("top")) &&
        (!present(element.value(QStringLiteral("autoResize"))) || element.value(QStringLiteral("autoResize")).toBool()) &&
        (!present(element.value(QStringLiteral("lineHeight"))) || (element.value(QStringLiteral("lineHeight")).isDouble() && std::abs(element.value(QStringLiteral("lineHeight")).toDouble() - 1.15) < 0.000001));
}
bool supportedStyle(const QJsonObject& element) {
    const auto type = element.value(QStringLiteral("type")).toString();
    if ((present(element.value(QStringLiteral("angle"))) && element.value(QStringLiteral("angle")).toDouble() != 0) ||
        nonempty(element.value(QStringLiteral("groupIds"))) || nonempty(element.value(QStringLiteral("customData"))) ||
        nonempty(element.value(QStringLiteral("frameId"))) || nonempty(element.value(QStringLiteral("roundness"))) ||
        nonempty(element.value(QStringLiteral("link"))) || element.value(QStringLiteral("locked")).toBool() ||
        (present(element.value(QStringLiteral("strokeStyle"))) && element.value(QStringLiteral("strokeStyle")) != QJsonValue(QStringLiteral("solid")))) return false;
    if (!color(element.value(QStringLiteral("strokeColor"))) || !number(element.value(QStringLiteral("opacity")), 0, 100) ||
        std::floor(element.value(QStringLiteral("opacity")).toDouble()) != element.value(QStringLiteral("opacity")).toDouble()) return false;
    if (type == QLatin1String("text")) return true;
    if (element.value(QStringLiteral("roughness")).toDouble(-1) != 0 || element.value(QStringLiteral("fillStyle")) != QJsonValue(QStringLiteral("solid")) ||
        !number(element.value(QStringLiteral("strokeWidth")), 1, 4)) return false;
    if (type == QLatin1String("line") || type == QLatin1String("arrow")) {
        if (present(element.value(QStringLiteral("startBinding"))) || present(element.value(QStringLiteral("endBinding"))) ||
            present(element.value(QStringLiteral("startArrowhead"))) || element.value(QStringLiteral("elbowed")).toBool() ||
            nonempty(element.value(QStringLiteral("fixedSegments"))) || element.value(QStringLiteral("startIsSpecial")).toBool() || element.value(QStringLiteral("endIsSpecial")).toBool()) return false;
        const auto end = element.value(QStringLiteral("endArrowhead"));
        if (type == QLatin1String("arrow") ? end != QJsonValue(QStringLiteral("arrow")) : present(end)) return false;
        const auto background = element.value(QStringLiteral("backgroundColor"));
        return !present(background) || background == QJsonValue(QStringLiteral("transparent"));
    }
    return color(element.value(QStringLiteral("backgroundColor")), true);
}
bool supportedKeys(const QJsonObject& element) {
    static const QSet<QString> keys{
        QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height"),
        QStringLiteral("angle"), QStringLiteral("strokeColor"), QStringLiteral("backgroundColor"), QStringLiteral("fillStyle"),
        QStringLiteral("strokeWidth"), QStringLiteral("strokeStyle"), QStringLiteral("roughness"), QStringLiteral("opacity"),
        QStringLiteral("groupIds"), QStringLiteral("frameId"), QStringLiteral("index"), QStringLiteral("roundness"), QStringLiteral("seed"),
        QStringLiteral("version"), QStringLiteral("versionNonce"), QStringLiteral("isDeleted"), QStringLiteral("boundElements"),
        QStringLiteral("updated"), QStringLiteral("link"), QStringLiteral("locked"), QStringLiteral("customData"),
        QStringLiteral("text"), QStringLiteral("originalText"), QStringLiteral("fontSize"), QStringLiteral("fontFamily"),
        QStringLiteral("fontWeight"), QStringLiteral("fontStyle"), QStringLiteral("textAlign"), QStringLiteral("verticalAlign"),
        QStringLiteral("containerId"), QStringLiteral("autoResize"), QStringLiteral("lineHeight"), QStringLiteral("baseline"),
        QStringLiteral("points"), QStringLiteral("lastCommittedPoint"), QStringLiteral("startBinding"), QStringLiteral("endBinding"),
        QStringLiteral("startArrowhead"), QStringLiteral("endArrowhead"), QStringLiteral("elbowed"), QStringLiteral("fixedSegments"),
        QStringLiteral("startIsSpecial"), QStringLiteral("endIsSpecial")};
    for (auto item = element.begin(); item != element.end(); ++item) if (!keys.contains(item.key())) return false;
    return true;
}
QByteArray encode(const QJsonObject& scene) { return QJsonDocument(scene).toJson(QJsonDocument::Compact); }
void unsupported(EditableExcalidrawScene& result, const QString& message) {
    if (!result.unsupported.contains(message) && result.unsupported.size() < 128) result.unsupported.append(message);
}
bool changeAllowed(const QString& key, const QJsonValue& value, const QString& type) {
    const bool linear = type == QLatin1String("line") || type == QLatin1String("arrow");
    if (key == QLatin1String("x") || key == QLatin1String("y")) return number(value, -10000, 10000);
    if (key == QLatin1String("width") || key == QLatin1String("height")) return !linear && number(value, 0, 10000) && value.toDouble() > 0;
    if (key == QLatin1String("angle")) return number(value, -2 * std::numbers::pi, 2 * std::numbers::pi);
    if (key == QLatin1String("text")) return type == QLatin1String("text") && value.isString() && !value.toString().isEmpty() && value.toString().toUtf8().size() <= 4000;
    if (key == QLatin1String("fontSize")) return type == QLatin1String("text") && number(value, 8, 96);
    if (key == QLatin1String("strokeColor")) return color(value);
    if (key == QLatin1String("backgroundColor")) return color(value, true);
    if (key == QLatin1String("strokeWidth")) return number(value, 1, 4);
    if (key == QLatin1String("opacity")) return number(value, 0, 100) && std::floor(value.toDouble()) == value.toDouble();
    if (key == QLatin1String("points")) return linear && points(value, 64);
    return false;
}
bool derivedAllowed(const QString& key, const QJsonValue& value) {
    if (key == QLatin1String("x") || key == QLatin1String("y") || key == QLatin1String("width") || key == QLatin1String("height")) return number(value, -1000000, 1000000);
    if (key == QLatin1String("angle")) return number(value, -2 * std::numbers::pi, 2 * std::numbers::pi);
    if (key == QLatin1String("points")) return points(value, 4096);
    if (key == QLatin1String("text")) return value.isString() && value.toString().size() <= 65536;
    if (key == QLatin1String("version") || key == QLatin1String("versionNonce") || key == QLatin1String("updated")) return number(value, 0, 9007199254740991.0);
    if (key == QLatin1String("startIsSpecial") || key == QLatin1String("endIsSpecial")) return value.isBool();
    if (key == QLatin1String("fixedSegments")) return value.isNull() || value.isArray();
    return false;
}
bool mergeBinding(QJsonObject& target, const QString& key, const QJsonValue& rendered, QString& error) {
    const auto original = target.value(key);
    if (!present(original)) {
        if (present(rendered)) { error = QStringLiteral("Native renderer attempted to add an element binding."); return false; }
        return true;
    }
    if (!original.isObject() || !rendered.isObject() || original.toObject().value(QStringLiteral("elementId")) != rendered.toObject().value(QStringLiteral("elementId"))) {
        error = QStringLiteral("Native renderer changed an element binding identity."); return false;
    }
    auto binding = original.toObject(); const auto result = rendered.toObject();
    for (const auto& field : {QStringLiteral("focus"), QStringLiteral("gap"), QStringLiteral("fixedPoint")}) {
        if (!result.contains(field)) continue;
        const auto value = result.value(field);
        bool valid = field == QLatin1String("focus") ? number(value, -1000000, 1000000) : number(value, 0, 1000000);
        if (field == QLatin1String("fixedPoint")) valid = value.isNull() || (value.isArray() && value.toArray().size() == 2 &&
            number(value.toArray().at(0), -1000000, 1000000) && number(value.toArray().at(1), -1000000, 1000000));
        if (!valid) { error = QStringLiteral("Native renderer returned invalid binding geometry."); return false; }
        binding[field] = value;
    }
    target[key] = binding; return true;
}
}  // namespace

EditableExcalidrawScene inspectExcalidrawScene(const QByteArray& source) {
    EditableExcalidrawScene result; QJsonObject scene; QString error;
    if (!parse(source, scene, error)) { unsupported(result, error); return result; }
    QMap<QString, QJsonObject> elements;
    if (!activeElements(scene.value(QStringLiteral("elements")).toArray(), elements, error)) { unsupported(result, error); return result; }
    if (nonempty(scene.value(QStringLiteral("files")))) unsupported(result, QStringLiteral("Embedded assets require native scene editing."));
    for (auto field = scene.begin(); field != scene.end(); ++field) {
        if (field.key() != QLatin1String("elements") && field.key() != QLatin1String("files") && field.key() != QLatin1String("appState") &&
            field.key() != QLatin1String("type") && field.key() != QLatin1String("version") && field.key() != QLatin1String("source"))
            unsupported(result, QStringLiteral("Scene contains additional root metadata."));
    }
    const auto background = scene.value(QStringLiteral("appState")).toObject().value(QStringLiteral("viewBackgroundColor"));
    if (present(background) && background != QJsonValue(QStringLiteral("#ffffff"))) unsupported(result, QStringLiteral("Scene uses a custom background."));
    const auto stateValue = scene.value(QStringLiteral("appState"));
    if (present(stateValue) && !stateValue.isObject()) unsupported(result, QStringLiteral("Scene contains invalid application state."));
    const auto state = stateValue.toObject();
    // Editing aids and viewport preferences do not change figure content.
    // Persisted rendering options and unknown metadata require native patching.
    static const QSet<QString> neutralStateKeys{
        QStringLiteral("viewBackgroundColor"), QStringLiteral("exportWithDarkMode"),
        QStringLiteral("gridSize"), QStringLiteral("gridStep"), QStringLiteral("gridModeEnabled"),
        QStringLiteral("zoom"), QStringLiteral("scrollX"), QStringLiteral("scrollY"),
        QStringLiteral("selectedElementIds"), QStringLiteral("selectedGroupIds"), QStringLiteral("editingGroupId"),
        QStringLiteral("theme"), QStringLiteral("name"), QStringLiteral("viewModeEnabled"), QStringLiteral("zenModeEnabled"),
        QStringLiteral("currentItemBackgroundColor"), QStringLiteral("currentItemEndArrowhead"), QStringLiteral("currentItemFillStyle"),
        QStringLiteral("currentItemFontFamily"), QStringLiteral("currentItemFontSize"), QStringLiteral("currentItemRoundness"),
        QStringLiteral("currentItemArrowType"), QStringLiteral("currentItemOpacity"), QStringLiteral("currentItemRoughness"),
        QStringLiteral("currentItemStartArrowhead"), QStringLiteral("currentItemStrokeColor"), QStringLiteral("currentItemStrokeStyle"),
        QStringLiteral("currentItemStrokeWidth"), QStringLiteral("currentItemTextAlign")};
    for (auto field = state.begin(); field != state.end(); ++field) {
        if (!neutralStateKeys.contains(field.key()) || (field.key() == QLatin1String("exportWithDarkMode") &&
            (!field.value().isBool() || field.value().toBool()))) {
            unsupported(result, QStringLiteral("Application state %1 requires native scene editing.").arg(field.key()));
        }
    }
    QMap<QString, QStringList> labels;
    for (auto entry = elements.begin(); entry != elements.end(); ++entry) {
        if (entry->value(QStringLiteral("type")) == QJsonValue(QStringLiteral("text")) && nonempty(entry->value(QStringLiteral("containerId"))))
            labels[entry->value(QStringLiteral("containerId")).toString()].append(entry.key());
    }
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    for (const auto sourceElement : scene.value(QStringLiteral("elements")).toArray()) {
        const auto element = sourceElement.toObject();
        if (element.value(QStringLiteral("isDeleted")).toBool()) continue;
        const auto id = element.value(QStringLiteral("id")).toString(); const auto type = element.value(QStringLiteral("type")).toString();
        const bool boundText = type == QLatin1String("text") && nonempty(element.value(QStringLiteral("containerId")));
        bool supported = idPattern.match(id).hasMatch() && supportedKeys(element) && supportedStyle(element) &&
            number(element.value(QStringLiteral("x")), -10000, 10000) && number(element.value(QStringLiteral("y")), -10000, 10000);
        const bool shape = type == QLatin1String("rectangle") || type == QLatin1String("ellipse") || type == QLatin1String("diamond");
        const bool linear = type == QLatin1String("line") || type == QLatin1String("arrow");
        if (type == QLatin1String("text")) supported = supported && supportedText(element, boundText);
        else if (shape) supported = supported && number(element.value(QStringLiteral("width")), 0, 10000) && element.value(QStringLiteral("width")).toDouble() > 0 &&
            number(element.value(QStringLiteral("height")), 0, 10000) && element.value(QStringLiteral("height")).toDouble() > 0;
        else if (linear) supported = supported && points(element.value(QStringLiteral("points")), 16);
        else supported = false;
        const auto ownedLabels = labels.value(id);
        if (ownedLabels.size() > 1 || (type == QLatin1String("line") && !ownedLabels.isEmpty())) supported = false;
        for (const auto binding : element.value(QStringLiteral("boundElements")).toArray()) {
            const auto child = binding.toObject();
            if (child.value(QStringLiteral("type")) != QJsonValue(QStringLiteral("text")) || !ownedLabels.contains(child.value(QStringLiteral("id")).toString())) supported = false;
        }
        if (boundText) {
            const auto parent = elements.value(element.value(QStringLiteral("containerId")).toString()); const auto parentType = parent.value(QStringLiteral("type")).toString();
            if (parent.isEmpty() || (parentType != QLatin1String("rectangle") && parentType != QLatin1String("ellipse") && parentType != QLatin1String("diamond") && parentType != QLatin1String("arrow"))) supported = false;
            if (element.value(QStringLiteral("strokeColor")) != parent.value(QStringLiteral("strokeColor")) ||
                element.value(QStringLiteral("opacity")) != QJsonValue(100)) supported = false;
        }
        if (!supported) unsupported(result, QStringLiteral("Element %1 has properties requiring native scene editing.").arg(id));
        if (boundText) continue;
        if (result.elements.size() >= 128) { result.truncated = true; continue; }
        if (!shape && !linear && type != QLatin1String("text")) continue;
        QJsonObject output{{QStringLiteral("id"), id}, {QStringLiteral("type"), type}, {QStringLiteral("x"), element.value(QStringLiteral("x"))}, {QStringLiteral("y"), element.value(QStringLiteral("y"))}};
        const auto copy = [&output, &element](const char* key) { const auto name = QLatin1String(key); if (element.contains(name)) output[name] = element.value(name); };
        copy("strokeColor"); copy("opacity");
        if (type == QLatin1String("text")) { output[QStringLiteral("text")] = sourceText(element); copy("fontSize"); }
        else {
            copy("strokeWidth");
            if (shape) {
                copy("width"); copy("height");
                // Omission reproduces the Professional helper's transparent
                // default and remains valid in the existing color schema.
                if (element.value(QStringLiteral("backgroundColor")) != QJsonValue(QStringLiteral("transparent"))) copy("backgroundColor");
            } else copy("points");
        }
        if (ownedLabels.size() == 1 && type != QLatin1String("line") && type != QLatin1String("text")) {
            const auto label = elements.value(ownedLabels.first());
            output[QStringLiteral("label")] = QJsonObject{{QStringLiteral("id"), ownedLabels.first()}, {QStringLiteral("text"), sourceText(label)}, {QStringLiteral("fontSize"), label.value(QStringLiteral("fontSize"))}};
        }
        result.elements.append(output);
    }
    return result;
}

std::optional<ExcalidrawScenePatch> prepareExcalidrawScenePatch(const QByteArray& source, const QJsonArray& edits, QString& error) {
    error.clear(); QJsonObject scene;
    if (!parse(source, scene, error)) return std::nullopt;
    const auto assets = scene.value(QStringLiteral("files"));
    if (present(assets) && !assets.isObject()) {
        error = QStringLiteral("Excalidraw assets must be self-contained image data URLs."); return std::nullopt;
    }
    static const QRegularExpression imageDataUrl(QStringLiteral("^data:image/[A-Za-z0-9.+-]+;base64,[A-Za-z0-9+/]+={0,2}$"));
    const auto files = assets.toObject();
    for (auto asset = files.begin(); asset != files.end(); ++asset) {
        const auto url = asset.value().toObject().value(QStringLiteral("dataURL"));
        if (!asset.value().isObject() || !url.isString() || !imageDataUrl.match(url.toString()).hasMatch()) {
            error = QStringLiteral("Excalidraw asset %1 must use a self-contained image data URL.").arg(asset.key()); return std::nullopt;
        }
    }
    if (edits.isEmpty() || edits.size() > 128 || QJsonDocument(edits).toJson(QJsonDocument::Compact).size() > editsLimit) {
        error = QStringLiteral("Native scene edits require 1–128 edits totaling at most 256 KiB."); return std::nullopt;
    }
    auto sourceElements = scene.value(QStringLiteral("elements")).toArray(); QMap<QString, QJsonObject> elements;
    if (!activeElements(sourceElements, elements, error)) return std::nullopt;
    ExcalidrawScenePatch patch; QSet<QString> edited;
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    for (const auto editValue : edits) {
        const auto edit = editValue.toObject(); const auto id = edit.value(QStringLiteral("id")).toString(); const auto changes = edit.value(QStringLiteral("changes")).toObject();
        if (!editValue.isObject() || edit.size() != 2 || !edit.value(QStringLiteral("id")).isString() || !edit.value(QStringLiteral("changes")).isObject() ||
            changes.isEmpty() || !idPattern.match(id).hasMatch() || edited.contains(id) || !elements.contains(id)) {
            error = QStringLiteral("Native scene edit has an invalid, duplicate, or unavailable target."); return std::nullopt;
        }
        auto element = elements.value(id); const auto type = element.value(QStringLiteral("type")).toString();
        if (element.value(QStringLiteral("locked")).toBool()) {
            error = QStringLiteral("Native scene edit target %1 is locked.").arg(id); return std::nullopt;
        }
        for (auto change = changes.begin(); change != changes.end(); ++change) {
            if (!changeAllowed(change.key(), change.value(), type)) { error = QStringLiteral("Native scene edit has an unsupported or invalid %1 property.").arg(change.key()); return std::nullopt; }
            element[change.key()] = change.value();
            if (change.key() == QLatin1String("text")) element[QStringLiteral("originalText")] = change.value();
        }
        edited.insert(id); patch.renderElementIds.append(id); elements[id] = element;
    }
    QMap<QString, QStringList> dependencies;
    const auto connect = [&elements, &dependencies](const QString& from, const QString& to) {
        if (elements.contains(from) && elements.contains(to)) dependencies[from].append(to);
    };
    for (auto element = elements.begin(); element != elements.end(); ++element) {
        const auto parent = element->value(QStringLiteral("containerId")).toString();
        connect(element.key(), parent);
        connect(parent, element.key());
        for (const auto binding : element->value(QStringLiteral("boundElements")).toArray())
            connect(element.key(), binding.toObject().value(QStringLiteral("id")).toString());
        // Binding targets affect their connectors. The reverse edge would
        // incorrectly include an unrelated shape at the connector's far end.
        for (const auto* key : {"startBinding", "endBinding"})
            connect(element->value(QLatin1String(key)).toObject().value(QStringLiteral("elementId")).toString(), element.key());
    }
    QSet<QString> affected = edited;
    QStringList queue = patch.renderElementIds;
    for (qsizetype next = 0; next < queue.size(); ++next) {
        for (const auto& dependent : dependencies.value(queue.at(next))) {
            if (affected.contains(dependent)) continue;
            affected.insert(dependent); queue.append(dependent);
        }
    }
    for (qsizetype index = 0; index < sourceElements.size(); ++index) {
        const auto element = sourceElements.at(index).toObject(); const auto id = element.value(QStringLiteral("id")).toString();
        if (edited.contains(id)) sourceElements[index] = elements.value(id);
        else if (affected.contains(id)) patch.dependentElementIds.append(id);
    }
    scene[QStringLiteral("elements")] = sourceElements; patch.scene = encode(scene);
    if (patch.scene.size() > sceneLimit) { error = QStringLiteral("Patched Excalidraw scene exceeds 8 MiB."); return std::nullopt; }
    return patch;
}

QByteArray finishExcalidrawScenePatch(const ExcalidrawScenePatch& patch, const QByteArray& renderedScene, QString& error) {
    error.clear(); QJsonObject source; QJsonObject rendered;
    if (!parse(patch.scene, source, error) || !parse(renderedScene, rendered, error)) return {};
    QMap<QString, QJsonObject> renderedElements;
    if (!activeElements(rendered.value(QStringLiteral("elements")).toArray(), renderedElements, error)) return {};
    QSet<QString> affected(patch.renderElementIds.begin(), patch.renderElementIds.end()); affected.unite(QSet<QString>(patch.dependentElementIds.begin(), patch.dependentElementIds.end()));
    auto elements = source.value(QStringLiteral("elements")).toArray(); QSet<QString> merged;
    for (qsizetype index = 0; index < elements.size(); ++index) {
        auto original = elements.at(index).toObject(); const auto id = original.value(QStringLiteral("id")).toString(); if (!affected.contains(id)) continue;
        if (original.value(QStringLiteral("isDeleted")).toBool() || !renderedElements.contains(id) || renderedElements.value(id).value(QStringLiteral("type")) != original.value(QStringLiteral("type"))) {
            error = QStringLiteral("Native renderer omitted or changed an affected element."); return {};
        }
        const auto result = renderedElements.value(id);
        for (const auto* key : {"x", "y", "width", "height", "angle", "points", "text", "fixedSegments", "startIsSpecial", "endIsSpecial", "version", "versionNonce", "updated"}) {
            const auto name = QLatin1String(key); if (!result.contains(name)) continue; const auto value = result.value(name);
            if (!derivedAllowed(name, value)) { error = QStringLiteral("Native renderer returned invalid %1 geometry.").arg(name); return {}; }
            original[name] = value;
        }
        for (const auto& key : {QStringLiteral("startBinding"), QStringLiteral("endBinding")}) if (!mergeBinding(original, key, result.value(key), error)) return {};
        elements[index] = original; merged.insert(id);
    }
    if (merged != affected) { error = QStringLiteral("Native renderer has invalid affected element identities."); return {}; }
    source[QStringLiteral("elements")] = elements; const auto finished = encode(source);
    if (finished.size() > sceneLimit) { error = QStringLiteral("Rendered Excalidraw scene exceeds 8 MiB."); return {}; }
    return finished;
}
}  // namespace docxstudio::app
