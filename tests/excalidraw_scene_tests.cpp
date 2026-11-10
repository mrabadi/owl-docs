#include "docxstudio/app/ExcalidrawScene.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdlib>
#include <iostream>

using namespace docxstudio::app;
namespace {
void check(bool condition, const char* message) { if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
QJsonObject shape(const QString& id = QStringLiteral("shape")) {
    return {{"id", id}, {"type", "rectangle"}, {"x", 20}, {"y", 30}, {"width", 100}, {"height", 60},
        {"roughness", 0}, {"fillStyle", "solid"}, {"strokeColor", "#222624"}, {"backgroundColor", "#ffffff"},
        {"strokeWidth", 2}, {"opacity", 100}, {"angle", 0}, {"groupIds", QJsonArray{}}};
}
QJsonObject sourceScene(bool complex = false) {
    auto box = shape();
    box["boundElements"] = QJsonArray{QJsonObject{{"id", "label"}, {"type", "text"}}};
    QJsonObject label{{"id", "label"}, {"type", "text"}, {"containerId", "shape"}, {"x", 30}, {"y", 40}, {"width", 80}, {"height", 20},
        {"text", "Visible label"}, {"originalText", "Source label"}, {"fontFamily", 2}, {"fontSize", 20}, {"textAlign", "center"}, {"verticalAlign", "middle"},
        {"autoResize", true}, {"lineHeight", 1.15}, {"roughness", 1}, {"fillStyle", "hachure"}, {"strokeWidth", 1}, {"strokeColor", "#222624"}, {"opacity", 100}};
    QJsonObject arrow{{"id", "arrow"}, {"type", "arrow"}, {"x", 120}, {"y", 60}, {"width", 80}, {"height", 0},
        {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{80, 0}}}, {"roughness", 0}, {"fillStyle", "solid"},
        {"strokeColor", "#222624"}, {"backgroundColor", "transparent"}, {"strokeWidth", 2}, {"opacity", 100}, {"angle", 0}, {"endArrowhead", "arrow"}};
    QJsonArray elements{box, label, arrow};
    QJsonObject result{{"type", "excalidraw"}, {"version", 2}, {"elements", elements}, {"files", QJsonObject{}}, {"appState", QJsonObject{{"viewBackgroundColor", "#ffffff"}}}};
    if (complex) {
        box["groupIds"] = QJsonArray{"retained-group"}; box["angle"] = 0.2; box["customData"] = QJsonObject{{"retained", true}};
        box["boundElements"] = QJsonArray{QJsonObject{{"id", "label"}, {"type", "text"}}, QJsonObject{{"id", "arrow"}, {"type", "arrow"}}};
        arrow["endBinding"] = QJsonObject{{"elementId", "shape"}, {"focus", 0.1}, {"gap", 4}};
        arrow["boundElements"] = QJsonArray{QJsonObject{{"id", "arrow-label"}, {"type", "text"}}};
        auto arrowLabel = label; arrowLabel["id"] = "arrow-label"; arrowLabel["containerId"] = "arrow";
        auto unrelated = shape(QStringLiteral("unrelated")); unrelated["customData"] = QJsonObject{{"exact", "preserve"}};
        elements = {box, label, arrow, arrowLabel, unrelated}; result["elements"] = elements;
        result["files"] = QJsonObject{{"asset", QJsonObject{{"dataURL", "data:image/png;base64,AA=="}, {"custom", "retained"}}}};
        result["customRoot"] = QJsonObject{{"owner", "retained"}};
    }
    return result;
}
QByteArray encode(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
void inspection() {
    const auto result = inspectExcalidrawScene(encode(sourceScene()));
    check(result.complete() && result.elements.size() == 2, "supported native labels do not roundtrip through editable skeleton reads");
    const auto label = result.elements.at(0).toObject().value("label").toObject();
    check(label.value("id") == "label" && label.value("text") == "Source label" && label.value("fontSize") == 20, "bound label identity/original source text/font size lost");
    check(!inspectExcalidrawScene(encode(sourceScene(true))).complete(), "complex scene incorrectly authorizes destructive replacement");
    check(inspectExcalidrawScene(encode({{"elements", QJsonArray{}}})).complete(), "valid empty scene is not complete");
    QJsonArray many;
    for (int index = 0; index < 129; ++index) {
        auto box = shape(QStringLiteral("shape%1").arg(index));
        const auto id = QStringLiteral("label%1").arg(index);
        box["boundElements"] = QJsonArray{QJsonObject{{"id", id}, {"type", "text"}}};
        auto labelObject = sourceScene().value("elements").toArray().at(1).toObject();
        labelObject["id"] = id; labelObject["containerId"] = box.value("id");
        many.append(box); many.append(labelObject);
    }
    const auto truncated = inspectExcalidrawScene(encode({{"elements", many}}));
    check(truncated.truncated && truncated.elements.size() == 128 && !truncated.complete(), "bound labels were counted as extra skeleton elements or truncation was hidden");
}
void patching() {
    const auto original = sourceScene(true);
    const auto source = encode(original);
    QString error;
    const auto patch = prepareExcalidrawScenePatch(source,
        {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "Long edited label"}, {"fontSize", 24}}}}}, error);
    check(patch.has_value(), "native scene edits are unavailable");
    check(patch->renderElementIds == QStringList{QStringLiteral("label")} && patch->dependentElementIds.contains("shape") && patch->dependentElementIds.contains("arrow") && patch->dependentElementIds.contains("arrow-label") && !patch->dependentElementIds.contains("unrelated"), "native dependency closure omitted parent/connector/label or included unrelated shape");
    auto rendered = QJsonDocument::fromJson(patch->scene).object();
    auto elements = rendered.value("elements").toArray();
    auto box = elements.at(0).toObject(); box["height"] = 120; box["groupIds"] = QJsonArray{"must-not-copy"}; elements[0] = box;
    auto label = elements.at(1).toObject(); label["x"] = 220; label["width"] = 160; label["text"] = "Long edited\nlabel"; elements[1] = label;
    auto unrelated = elements.at(4).toObject(); unrelated["x"] = 999; elements[4] = unrelated;
    rendered["elements"] = elements; rendered["files"] = QJsonObject{{"wrong", true}};
    const auto finished = finishExcalidrawScenePatch(*patch, encode(rendered), error);
    check(!finished.isEmpty(), "native renderer result did not merge");
    const auto merged = QJsonDocument::fromJson(finished).object();
    const auto output = merged.value("elements").toArray();
    check(output.at(0).toObject().value("height") == 120 && output.at(1).toObject().value("x") == 220 && output.at(1).toObject().value("originalText") == "Long edited label", "derived geometry/source text not merged correctly");
    check(output.at(0).toObject().value("groupIds") == original.value("elements").toArray().at(0).toObject().value("groupIds") && output.at(4) == original.value("elements").toArray().at(4) && merged.value("files") == original.value("files") && merged.value("customRoot") == original.value("customRoot"), "native merge destroyed unrelated source objects/assets/custom data");
    check(!prepareExcalidrawScenePatch(source, {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"id", "replacement"}}}}}, error) && !error.isEmpty(), "identity-changing patch accepted");
    check(!prepareExcalidrawScenePatch(source, {QJsonObject{{"id", "missing"}, {"changes", QJsonObject{{"x", 1}}}}}, error), "missing patch target accepted");
}
void assetBoundaries() {
    QString error;
    for (const auto* url : {"file:///etc/passwd", "https://example.invalid/picture.png", "../picture.png", "data:text/html;base64,AA=="}) {
        auto source = sourceScene(true);
        source["files"] = QJsonObject{{"asset", QJsonObject{{"dataURL", QLatin1String(url)}}}};
        const auto patch = prepareExcalidrawScenePatch(encode(source),
            {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "Edited"}}}}}, error);
        check(!patch && error.contains("asset", Qt::CaseInsensitive), "external/non-image asset was allowed into native renderer");
    }
    check(prepareExcalidrawScenePatch(encode(sourceScene(true)),
        {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "Edited"}}}}}, error).has_value(), "self-contained image asset was rejected");
}
void patchBounds() {
    QString error;
    const QString unicode(2001, QChar(0x03B1));
    check(!prepareExcalidrawScenePatch(encode(sourceScene()),
        {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", unicode}}}}}, error), "native patch text exceeded its UTF-8 byte bound");
    auto scene = sourceScene();
    auto elements = scene.value("elements").toArray();
    auto element = elements[0].toObject(); element["id"] = "unsafe id"; elements[0] = element; scene["elements"] = elements;
    check(!prepareExcalidrawScenePatch(encode(scene),
        {QJsonObject{{"id", "unsafe id"}, {"changes", QJsonObject{{"x", 1}}}}}, error), "non-schema native patch identity was accepted");
    const auto source = encode(sourceScene(true));
    const QJsonArray duplicate{QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "one"}}}}, QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "two"}}}}};
    check(!prepareExcalidrawScenePatch(source, duplicate, error), "duplicate native patch IDs were accepted");
    for (const auto& changes : {QJsonObject{{"width", 5}}, QJsonObject{{"points", QJsonArray{QJsonArray{0, 0}}}}, QJsonObject{{"opacity", 1.5}}, QJsonObject{{"x", 10001}}, QJsonObject{{"angle", 7}}})
        check(!prepareExcalidrawScenePatch(source, {QJsonObject{{"id", "arrow"}, {"changes", changes}}}, error), "invalid native geometry or opacity accepted");
    const auto patch = prepareExcalidrawScenePatch(source, {QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "Edited"}}}}}, error);
    check(patch.has_value(), "valid patch rejected while testing renderer guards");
    auto rendered = QJsonDocument::fromJson(patch->scene).object();
    elements = rendered.value("elements").toArray(); elements.removeAt(1); rendered["elements"] = elements;
    check(finishExcalidrawScenePatch(*patch, encode(rendered), error).isEmpty(), "renderer omitted affected element without rejecting merge");
    rendered = QJsonDocument::fromJson(patch->scene).object(); elements = rendered.value("elements").toArray();
    element = elements[2].toObject(); element["endBinding"] = QJsonObject{{"elementId", "unrelated"}, {"focus", 0}, {"gap", 1}}; elements[2] = element; rendered["elements"] = elements;
    check(finishExcalidrawScenePatch(*patch, encode(rendered), error).isEmpty(), "renderer retargeted an original binding");
    check(encode(sourceScene(true)) == source, "invalid patch or renderer failure changed source bytes");
}
void labelStyles() {
    for (const auto& change : {QJsonObject{{"strokeColor", "#ff0000"}}, QJsonObject{{"opacity", 50}}}) {
        auto scene = sourceScene(); auto elements = scene.value("elements").toArray();
        auto label = elements[1].toObject();
        for (auto field = change.begin(); field != change.end(); ++field) label[field.key()] = field.value();
        elements[1] = label; scene["elements"] = elements;
        check(!inspectExcalidrawScene(encode(scene)).complete(), "custom bound-label color/opacity was declared losslessly replaceable");
    }
}
void lockedTargets() {
    auto scene = sourceScene(true); auto elements = scene.value("elements").toArray();
    auto element = elements[1].toObject(); element["locked"] = true; elements[1] = element; scene["elements"] = elements;
    const QJsonArray edits{QJsonObject{{"id", "label"}, {"changes", QJsonObject{{"text", "Edited"}}}}};
    QString error;
    check(!prepareExcalidrawScenePatch(encode(scene), edits, error) && error.contains("locked", Qt::CaseInsensitive), "native patch edited a locked target");
    scene = sourceScene(true); elements = scene.value("elements").toArray();
    element = elements[4].toObject(); element["locked"] = true; elements[4] = element; scene["elements"] = elements;
    const auto patch = prepareExcalidrawScenePatch(encode(scene), edits, error);
    check(patch.has_value(), "unrelated locked object blocked a valid native patch");
    const auto output = QJsonDocument::fromJson(patch->scene).object().value("elements").toArray();
    check(output[4] == elements[4], "unrelated locked object was changed");
}
void dependencyChain() {
    QJsonArray elements;
    for (int index = 0; index < 1000; ++index) {
        auto element = shape(QStringLiteral("node%1").arg(index));
        if (index < 999) element["containerId"] = QStringLiteral("node%1").arg(index + 1);
        elements.append(element);
    }
    elements.append(shape(QStringLiteral("unrelated")));
    const auto source = encode({{"elements", elements}, {"files", QJsonObject{}}});
    QString error;
    const auto patch = prepareExcalidrawScenePatch(source,
        {QJsonObject{{"id", "node500"}, {"changes", QJsonObject{{"x", 1}}}}}, error);
    check(patch && patch->renderElementIds == QStringList{QStringLiteral("node500")} &&
        patch->dependentElementIds.size() == 999 && patch->dependentElementIds.first() == "node0" &&
        patch->dependentElementIds.last() == "node999" && !patch->dependentElementIds.contains("unrelated"),
        "long dependency chain changed the native affected set or source order");
}
void applicationState() {
    for (const auto& state : {QJsonObject{{"exportWithDarkMode", true}}, QJsonObject{{"nativeMetadata", QJsonObject{{"owner", "retained"}}}}}) {
        auto scene = sourceScene(); scene["appState"] = state;
        check(!inspectExcalidrawScene(encode(scene)).complete(), "nonrepresentable source app state was declared replaceable");
    }
    auto scene = sourceScene(); scene["appState"] = "invalid";
    check(!inspectExcalidrawScene(encode(scene)).complete(), "invalid application state was declared replaceable");
    scene["appState"] = QJsonObject{{"viewBackgroundColor", "#ffffff"}, {"exportWithDarkMode", false},
        {"gridSize", 20}, {"gridStep", 5}, {"gridModeEnabled", true}, {"zoom", QJsonObject{{"value", 2}}},
        {"selectedElementIds", QJsonObject{{"shape", true}}}, {"theme", "dark"}};
    check(inspectExcalidrawScene(encode(scene)).complete(), "neutral viewport/selection/grid state prevented supported scene replacement");
}
void transparentBackground() {
    auto scene = sourceScene(); auto elements = scene.value("elements").toArray();
    auto element = elements[0].toObject(); element["backgroundColor"] = "transparent";
    elements[0] = element; scene["elements"] = elements;
    const auto inspected = inspectExcalidrawScene(encode(scene));
    check(inspected.complete() && !inspected.elements[0].toObject().contains("backgroundColor"),
        "transparent shape default was emitted outside the skeleton schema");
    const auto colored = inspectExcalidrawScene(encode(sourceScene()));
    check(colored.elements[0].toObject().value("backgroundColor") == "#ffffff", "explicit colored shape background was omitted");
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "patch") patching();
    else if (argc > 1 && std::string(argv[1]) == "assets") assetBoundaries();
    else if (argc > 1 && std::string(argv[1]) == "bounds") patchBounds();
    else if (argc > 1 && std::string(argv[1]) == "label-styles") labelStyles();
    else if (argc > 1 && std::string(argv[1]) == "locked") lockedTargets();
    else if (argc > 1 && std::string(argv[1]) == "chain") dependencyChain();
    else if (argc > 1 && std::string(argv[1]) == "app-state") applicationState();
    else if (argc > 1 && std::string(argv[1]) == "transparent") transparentBackground();
    else { inspection(); patching(); assetBoundaries(); patchBounds(); labelStyles(); lockedTargets(); dependencyChain(); applicationState(); transparentBackground(); }
    std::cout << "Excalidraw scene tests passed\n";
}
