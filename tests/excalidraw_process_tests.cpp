#include "docxstudio/app/ExcalidrawFigure.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <iostream>
#include <memory>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}

struct Helper {
    QTemporaryDir directory;
    QString executable;
    explicit Helper(const QByteArray& mode) {
        check(directory.isValid(), "helper fixture directory unavailable");
        executable = directory.filePath(QStringLiteral("figure helper.py"));
        QFile script(executable);
        check(script.open(QIODevice::WriteOnly), "helper fixture cannot be written");
        script.write(R"PY(#!/usr/bin/python3
import json, pathlib, shutil, sys, time
root = pathlib.Path(__file__).parent
arg = sys.argv[1]
session = pathlib.Path(arg.split('=', 1)[1] if '=' in arg else sys.argv[2])
request = json.loads((session / 'request.json').read_text())
(root / 'request.json').write_text(json.dumps(request))
(root / 'ready').touch()
mode = (root / 'mode').read_text()
if mode == 'wait':
    time.sleep(60)
else:
    scene = request.get('scene') or {'type': 'excalidraw', 'version': 2, 'elements': []}
    (session / 'scene.excalidraw').write_text(json.dumps(scene))
    shutil.copyfile(root / 'preview.png', session / 'preview.png')
    result = {'token': request['token'], 'mode': 'professional', 'saved': True}
    if mode != 'missing-version':
        result['version'] = 1 if mode == 'old-version' else request['version']
    (session / 'result.json').write_text(json.dumps(result))
)PY");
        script.close();
        check(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
              "helper fixture cannot be made executable");
        QFile modeFile(directory.filePath(QStringLiteral("mode")));
        check(modeFile.open(QIODevice::WriteOnly), "helper mode unavailable");
        modeFile.write(mode);
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::white);
        check(image.save(directory.filePath(QStringLiteral("preview.png"))),
              "helper raster fixture unavailable");
    }
    bool ready() const { return QFile::exists(directory.filePath(QStringLiteral("ready"))); }
    QJsonObject request() const {
        QFile input(directory.filePath(QStringLiteral("request.json")));
        if (!input.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(input.readAll()).object();
    }
};

template<class Predicate> bool await(Predicate predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 3000) QTest::qWait(5);
    return predicate();
}

void destructorDoesNotComplete() {
    Helper helper("wait");
    bool called = false;
    auto editor = std::make_unique<docxstudio::app::ExcalidrawFigureEditor>(nullptr, helper.executable);
    editor->renderProfessional({}, [&](auto, const QString&) { called = true; });
    check(await([&] { return helper.ready(); }), "sleeping helper did not actually start");
    check(editor->busy() && !called, "fixture did not retain an active render");
    editor.reset();
    QCoreApplication::processEvents();
    check(!called, "destroying the figure editor invoked a stale completion");
}

void nativeProtocol(const QByteArray& mode) {
    Helper helper(mode);
    const QByteArray scene(R"({"type":"excalidraw","version":2,"elements":[{"id":"native-shape","type":"rectangle","roughness":2}],"appState":{"viewBackgroundColor":"#124578"},"files":{}})");
    QString error;
    const auto result = docxstudio::app::renderProfessionalExcalidrawScene(
        scene, {QStringLiteral("native-shape")}, error, helper.executable);
    const auto request = helper.request();
    check(request.value(QStringLiteral("version")).toInt() == 2,
          "native scene request did not use protocol version 2");
    check(request.value(QStringLiteral("mode")).toString() == QStringLiteral("professional") &&
          request.value(QStringLiteral("renderOnly")).toBool(), "native render request escaped the render-only protocol");
    check(request.value(QStringLiteral("scene")).toObject() == QJsonDocument::fromJson(scene).object(),
          "native render replaced the original source scene");
    check(request.value(QStringLiteral("patchElementIds")).toArray() == QJsonArray{QStringLiteral("native-shape")} &&
          !request.contains(QStringLiteral("skeleton")), "native render lost patch identities or sent a reconstruction skeleton");
    if (mode == "ok") {
        check(result.has_value() && error.isEmpty(), "valid native v2 result was rejected");
        if (result) {
            check(QJsonDocument::fromJson(result->scene).object() == QJsonDocument::fromJson(scene).object(),
                  "native source styling/background did not survive rendering");
            check(docxstudio::app::excalidrawSceneFromPng(result->png) == result->scene,
                  "native renderer did not preserve editable PNG source");
        }
    } else {
        check(!result && !error.isEmpty(), "native renderer accepted a missing/old protocol result version");
    }
}

void callbackCanDestroyEditor() {
    Helper helper("ok");
    bool called = false;
    auto editor = std::make_unique<docxstudio::app::ExcalidrawFigureEditor>(nullptr, helper.executable);
    editor->renderProfessional({}, [&](auto result, const QString& error) {
        check(result.has_value() && error.isEmpty(), "legacy render result was rejected");
        called = true;
        editor.reset();
    });
    check(await([&] { return called; }), "legacy callback did not complete");
    check(!editor, "completion did not destroy its editor");
}

void timeoutKeepsItsError() {
    Helper helper("wait");
    QString error;
    const auto result = docxstudio::app::renderProfessionalExcalidrawSkeleton({}, error, helper.executable);
    check(!result && error == QStringLiteral("The local Excalidraw renderer timed out."),
          "renderer teardown overwrote the timeout error or returned a stale result");
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsProcessTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ExcalidrawProcessTests"));
    const QString selector = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (selector.isEmpty() || selector == QStringLiteral("destructor")) destructorDoesNotComplete();
    if (selector.isEmpty() || selector == QStringLiteral("native")) nativeProtocol("ok");
    if (selector.isEmpty() || selector == QStringLiteral("old-version")) nativeProtocol("old-version");
    if (selector.isEmpty() || selector == QStringLiteral("missing-version")) nativeProtocol("missing-version");
    if (selector.isEmpty() || selector == QStringLiteral("callback-destroy")) callbackCanDestroyEditor();
    if (selector == QStringLiteral("timeout") || selector == QStringLiteral("--timeout")) timeoutKeepsItsError();
    return failures ? 1 : 0;
}
