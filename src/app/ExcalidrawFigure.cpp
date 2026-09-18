#include "docxstudio/app/ExcalidrawFigure.h"
#include "docxstudio/core/document.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace docxstudio::app {
namespace {

constexpr std::array<unsigned char, 8> kPngSignature{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
constexpr auto kKeyword = "owl-docs.excalidraw.v1";

std::uint32_t readBigEndian(const char* value) noexcept {
    return (static_cast<std::uint32_t>(
                static_cast<unsigned char>(value[0])) << 24U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(value[1])) << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(value[2])) << 8U) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(value[3]));
}

void appendBigEndian(QByteArray& output, std::uint32_t value) {
    output.append(static_cast<char>((value >> 24U) & 0xffU));
    output.append(static_cast<char>((value >> 16U) & 0xffU));
    output.append(static_cast<char>((value >> 8U) & 0xffU));
    output.append(static_cast<char>(value & 0xffU));
}

std::uint32_t crc32(const QByteArray& bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^
                (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

bool validScene(const QByteArray& scene) {
    if (scene.isEmpty() || scene.size() > kMaximumExcalidrawSceneBytes) {
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(scene, &parseError);
    return parseError.error == QJsonParseError::NoError &&
        document.isObject() &&
        document.object().value(QStringLiteral("elements")).isArray();
}

QByteArray metadataChunk(const QByteArray& scene) {
    QByteArray data(kKeyword);
    data.append('\0');  // keyword terminator
    data.append('\0');  // uncompressed
    data.append('\0');  // compression method
    data.append('\0');  // empty language tag
    data.append('\0');  // empty translated keyword
    data.append(scene);
    QByteArray typed("iTXt", 4);
    typed.append(data);
    QByteArray chunk;
    appendBigEndian(chunk, static_cast<std::uint32_t>(data.size()));
    chunk.append(typed);
    appendBigEndian(chunk, crc32(typed));
    return chunk;
}

QString figureEditorExecutable() {
#ifdef DOCXSTUDIO_INSTALL_FIGURE_EDITOR_PATH
    const QString installed =
        QString::fromUtf8(DOCXSTUDIO_INSTALL_FIGURE_EDITOR_PATH);
    if (QFileInfo::exists(installed)) return installed;
#endif
#ifdef DOCXSTUDIO_DEV_FIGURE_EDITOR_PATH
    const QString development =
        QString::fromUtf8(DOCXSTUDIO_DEV_FIGURE_EDITOR_PATH);
    if (QFileInfo::exists(development)) return development;
#endif
    return {};
}

}  // namespace

std::optional<QByteArray> excalidrawSceneFromPng(const QByteArray& png) {
    if (png.size() < 20 ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(),
                    reinterpret_cast<const unsigned char*>(png.constData()))) {
        return std::nullopt;
    }
    qsizetype cursor = 8;
    while (cursor <= png.size() - 12) {
        const auto length = readBigEndian(png.constData() + cursor);
        if (length > static_cast<std::uint32_t>(png.size() - cursor - 12)) {
            return std::nullopt;
        }
        const QByteArray type = png.sliced(cursor + 4, 4);
        const QByteArray data =
            png.sliced(cursor + 8, static_cast<qsizetype>(length));
        const auto expectedCrc =
            readBigEndian(png.constData() + cursor + 8 +
                          static_cast<qsizetype>(length));
        QByteArray typed = type;
        typed.append(data);
        if (crc32(typed) != expectedCrc) return std::nullopt;
        if (type == QByteArrayLiteral("iTXt")) {
            const QByteArray prefix =
                QByteArray(kKeyword) + QByteArray("\0\0\0\0\0", 5);
            if (data.startsWith(prefix)) {
                const QByteArray scene = data.sliced(prefix.size());
                return validScene(scene)
                    ? std::optional<QByteArray>(scene)
                    : std::nullopt;
            }
        }
        cursor += 12 + static_cast<qsizetype>(length);
        if (type == QByteArrayLiteral("IEND")) break;
    }
    return std::nullopt;
}

QByteArray pngWithExcalidrawScene(
    const QByteArray& png, const QByteArray& scene, QString& error) {
    if (!validScene(scene)) {
        error = QObject::tr("The figure editor returned invalid or oversized scene data.");
        return {};
    }
    if (png.size() < 20 ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(),
                    reinterpret_cast<const unsigned char*>(png.constData()))) {
        error = QObject::tr("The figure editor did not return a valid PNG preview.");
        return {};
    }
    QByteArray output;
    output.reserve(png.size() + scene.size() + 64);
    output.append(png.first(8));
    qsizetype cursor = 8;
    bool inserted = false;
    while (cursor <= png.size() - 12) {
        const auto length = readBigEndian(png.constData() + cursor);
        if (length > static_cast<std::uint32_t>(png.size() - cursor - 12)) {
            error = QObject::tr("The figure preview contains a malformed PNG chunk.");
            return {};
        }
        const qsizetype chunkSize =
            12 + static_cast<qsizetype>(length);
        const QByteArray type = png.sliced(cursor + 4, 4);
        const QByteArray data =
            png.sliced(cursor + 8, static_cast<qsizetype>(length));
        const QByteArray prefix =
            QByteArray(kKeyword) + QByteArray("\0\0\0\0\0", 5);
        const bool oldMetadata =
            type == QByteArrayLiteral("iTXt") && data.startsWith(prefix);
        if (type == QByteArrayLiteral("IEND") && !inserted) {
            output.append(metadataChunk(scene));
            inserted = true;
        }
        if (!oldMetadata) output.append(png.sliced(cursor, chunkSize));
        cursor += chunkSize;
        if (type == QByteArrayLiteral("IEND")) break;
    }
    if (!inserted ||
        output.size() > static_cast<qsizetype>(
            core::kMaximumEncodedImageBytes)) {
        error = QObject::tr("The editable figure exceeds the 16 MiB document-image limit.");
        return {};
    }
    return output;
}

std::optional<QSizeF> fitExcalidrawFigureDisplaySize(
    const QSize rasterSize, const std::optional<double> widthPoints,
    const std::optional<double> heightPoints) {
    if (rasterSize.width() <= 0 || rasterSize.height() <= 0) {
        return std::nullopt;
    }
    const auto validRequestedDimension = [](const std::optional<double> value) {
        return !value || (std::isfinite(*value) && *value > 0.0);
    };
    if (!validRequestedDimension(widthPoints) ||
        !validRequestedDimension(heightPoints)) {
        return std::nullopt;
    }
    const double aspect = static_cast<double>(rasterSize.height()) /
                          static_cast<double>(rasterSize.width());
    double width = widthPoints.value_or(432.0);
    double height = width * aspect;
    if (!widthPoints && heightPoints) {
        height = *heightPoints;
        width = height / aspect;
    } else if (widthPoints && heightPoints) {
        const double scale = std::min(
            *widthPoints / static_cast<double>(rasterSize.width()),
            *heightPoints / static_cast<double>(rasterSize.height()));
        width = static_cast<double>(rasterSize.width()) * scale;
        height = static_cast<double>(rasterSize.height()) * scale;
    }

    const double maximumScale = std::min(936.0 / width, 936.0 / height);
    if (maximumScale < 1.0) {
        width *= maximumScale;
        height *= maximumScale;
    }
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
        height <= 0.0) {
        return std::nullopt;
    }
    return QSizeF(width, height);
}

struct ExcalidrawFigureEditor::Session {
    std::unique_ptr<QTemporaryDir> directory;
    std::unique_ptr<QProcess> process;
    QByteArray token;
    Completion completion;
    bool persistMode{true};
};

ExcalidrawFigureEditor::ExcalidrawFigureEditor(QObject* parent)
    : QObject(parent) {}

ExcalidrawFigureEditor::~ExcalidrawFigureEditor() {
    if (session_ && session_->process->state() != QProcess::NotRunning) {
        session_->process->kill();
        session_->process->waitForFinished(1000);
    }
}

bool ExcalidrawFigureEditor::busy() const noexcept {
    return static_cast<bool>(session_);
}

void ExcalidrawFigureEditor::open(
    std::optional<QByteArray> scene, Completion completion) {
    start(std::move(scene), std::nullopt, false, std::move(completion));
}

void ExcalidrawFigureEditor::renderProfessional(
    const QJsonArray& skeleton, Completion completion) {
    start(std::nullopt, skeleton, true, std::move(completion));
}

void ExcalidrawFigureEditor::start(
    std::optional<QByteArray> scene, std::optional<QJsonArray> skeleton,
    const bool renderOnly, Completion completion) {
    if (busy()) {
        completion(std::nullopt,
                   tr("A figure editor window is already open."));
        return;
    }
    const QString executable = figureEditorExecutable();
    if (executable.isEmpty()) {
        completion(
            std::nullopt,
            tr("The bundled Excalidraw figure editor is unavailable. Reinstall Owl Docs using the complete package."));
        return;
    }
    if (scene && !validScene(*scene)) {
        completion(std::nullopt,
                   tr("This picture does not contain a valid editable Excalidraw scene."));
        return;
    }

    auto session = std::make_unique<Session>();
    session->directory = std::make_unique<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/owl-docs-figure-XXXXXX"));
    if (!session->directory->isValid()) {
        completion(std::nullopt,
                   tr("Could not create a private figure-editing session."));
        return;
    }
    session->token = QCryptographicHash::hash(
        QUuid::createUuid().toRfc4122() +
            QByteArray::number(QCoreApplication::applicationPid()),
        QCryptographicHash::Sha256).toHex();
    QSettings settings;
    const QString storedMode =
        settings.value(QStringLiteral("excalidraw/mode"),
                       QStringLiteral("professional"))
            .toString();
    const QString mode = renderOnly ? QStringLiteral("professional")
        : storedMode == QStringLiteral("sketch")
        ? QStringLiteral("sketch")
        : QStringLiteral("professional");
    QJsonObject request{
        {QStringLiteral("version"), 1},
        {QStringLiteral("token"), QString::fromLatin1(session->token)},
        {QStringLiteral("title"), tr("Excalidraw Figure — Owl Docs")},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("renderOnly"), renderOnly}};
    request.insert(
        QStringLiteral("scene"),
        scene ? QJsonDocument::fromJson(*scene).object()
              : QJsonValue(QJsonValue::Null));
    if (skeleton) request.insert(QStringLiteral("skeleton"), *skeleton);
    QSaveFile requestFile(
        session->directory->filePath(QStringLiteral("request.json")));
    if (!requestFile.open(QIODevice::WriteOnly) ||
        requestFile.write(QJsonDocument(request).toJson(
            QJsonDocument::Compact)) < 0 ||
        !requestFile.commit()) {
        completion(std::nullopt,
                   tr("Could not initialize the figure-editing session."));
        return;
    }
    session->completion = std::move(completion);
    session->persistMode = !renderOnly;
    session->process = std::make_unique<QProcess>(this);
    auto* process = session->process.get();
    connect(process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
                finishSession(exitCode);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (!session_ || error == QProcess::Crashed) return;
                finishSession(-1);
            });
#ifdef Q_OS_LINUX
    const QString bubblewrap = QStringLiteral("/usr/bin/bwrap");
    if (!QFileInfo::exists(bubblewrap)) {
        const auto callback = std::move(session->completion);
        callback(
            std::nullopt,
            tr("The bundled figure editor requires bubblewrap for its offline sandbox."));
        return;
    }
    const QString root = QFileInfo(executable).absolutePath();
    const QString uid = QString::number(static_cast<qulonglong>(getuid()));
    const QString sandboxRuntime =
        QStringLiteral("/run/user/") + uid;
    QDir(session->directory->path()).mkpath(QStringLiteral("config"));
    QDir(session->directory->path()).mkpath(QStringLiteral("cache"));
    QDir(session->directory->path()).mkpath(QStringLiteral("home"));
    QDir(session->directory->path()).mkpath(QStringLiteral("tmp"));
    QStringList arguments{
        QStringLiteral("--die-with-parent"),
        QStringLiteral("--unshare-user"),
        QStringLiteral("--unshare-pid"),
        QStringLiteral("--unshare-ipc"),
        QStringLiteral("--unshare-uts"),
        QStringLiteral("--unshare-cgroup"),
        QStringLiteral("--unshare-net"),
        QStringLiteral("--new-session"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--ro-bind"), QStringLiteral("/usr"),
        QStringLiteral("/usr"),
        QStringLiteral("--ro-bind"), QStringLiteral("/lib"),
        QStringLiteral("/lib"),
        QStringLiteral("--ro-bind"), QStringLiteral("/bin"),
        QStringLiteral("/bin"),
        QStringLiteral("--ro-bind"), QStringLiteral("/etc"),
        QStringLiteral("/etc"),
        QStringLiteral("--ro-bind"), root, QStringLiteral("/app"),
        QStringLiteral("--dir"), QStringLiteral("/home"),
        QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
        QStringLiteral("--dir"), QStringLiteral("/run"),
        QStringLiteral("--dir"), QStringLiteral("/run/user"),
        QStringLiteral("--dir"), sandboxRuntime,
        QStringLiteral("--bind"), session->directory->path(),
        QStringLiteral("/session"),
        QStringLiteral("--setenv"), QStringLiteral("HOME"),
        QStringLiteral("/session/home"),
        QStringLiteral("--setenv"), QStringLiteral("XDG_CONFIG_HOME"),
        QStringLiteral("/session/config"),
        QStringLiteral("--setenv"), QStringLiteral("XDG_CACHE_HOME"),
        QStringLiteral("/session/cache"),
        QStringLiteral("--setenv"), QStringLiteral("TMPDIR"),
        QStringLiteral("/session/tmp"),
        QStringLiteral("--setenv"),
        QStringLiteral("ELECTRON_DISABLE_SECURITY_WARNINGS"),
        QStringLiteral("true")};
    if (QFileInfo::exists(QStringLiteral("/lib64"))) {
        arguments << QStringLiteral("--ro-bind")
                  << QStringLiteral("/lib64")
                  << QStringLiteral("/lib64");
    }
    const auto environment = QProcessEnvironment::systemEnvironment();
    const QString wayland = environment.value(
        QStringLiteral("WAYLAND_DISPLAY"));
    const QString runtime = environment.value(
        QStringLiteral("XDG_RUNTIME_DIR"));
    const QString waylandSocket =
        runtime.isEmpty() || wayland.isEmpty()
        ? QString()
        : QDir(runtime).filePath(wayland);
    QString platformArgument;
    if (!waylandSocket.isEmpty() && QFileInfo::exists(waylandSocket)) {
        arguments << QStringLiteral("--bind") << waylandSocket
                  << sandboxRuntime + QLatin1Char('/') + wayland
                  << QStringLiteral("--setenv")
                  << QStringLiteral("WAYLAND_DISPLAY") << wayland
                  << QStringLiteral("--setenv")
                  << QStringLiteral("XDG_RUNTIME_DIR") << sandboxRuntime;
        platformArgument = QStringLiteral("--ozone-platform=wayland");
    } else {
        const QString display =
            environment.value(QStringLiteral("DISPLAY"));
        if (display.isEmpty() ||
            !QFileInfo::exists(QStringLiteral("/tmp/.X11-unix"))) {
            const auto callback = std::move(session->completion);
            callback(std::nullopt,
                     tr("No usable Wayland or X11 display is available for the figure editor."));
            return;
        }
        arguments << QStringLiteral("--ro-bind")
                  << QStringLiteral("/tmp/.X11-unix")
                  << QStringLiteral("/tmp/.X11-unix")
                  << QStringLiteral("--setenv")
                  << QStringLiteral("DISPLAY") << display;
        platformArgument = QStringLiteral("--ozone-platform=x11");
    }
    arguments << QStringLiteral("/app/") + QFileInfo(executable).fileName()
              << QStringLiteral("--no-sandbox")
              << platformArgument
              << QStringLiteral("--owl-docs-figure-session=/session");
    process->setProgram(bubblewrap);
    process->setArguments(arguments);
#else
    process->setProgram(executable);
    process->setArguments({
        QStringLiteral("--owl-docs-figure-session"),
        session->directory->path()});
#endif
    session_ = std::move(session);
    process->start();
}

void ExcalidrawFigureEditor::finishSession(int exitCode) {
    if (!session_) return;
    auto session = std::move(session_);
    const QString stderrText =
        QString::fromUtf8(session->process->readAllStandardError()).trimmed();
    QFile resultFile(
        session->directory->filePath(QStringLiteral("result.json")));
    if (!resultFile.open(QIODevice::ReadOnly)) {
        session->completion(
            std::nullopt,
            stderrText.isEmpty()
                ? tr("The figure editor closed without returning a result (exit code %1).")
                      .arg(exitCode)
                : tr("The figure editor failed: %1").arg(stderrText));
        return;
    }
    QJsonParseError parseError;
    const auto result =
        QJsonDocument::fromJson(resultFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !result.isObject() ||
        result.object().value(QStringLiteral("token")).toString().toLatin1() !=
            session->token) {
        session->completion(
            std::nullopt,
            tr("The figure editor returned an invalid session result."));
        return;
    }
    const auto object = result.object();
    const QString mode = object.value(QStringLiteral("mode")).toString();
    if (session->persistMode &&
        (mode == QStringLiteral("sketch") ||
         mode == QStringLiteral("professional"))) {
        QSettings settings;
        settings.setValue(QStringLiteral("excalidraw/mode"), mode);
        settings.sync();
    }
    if (!object.value(QStringLiteral("saved")).toBool()) {
        const QString error =
            object.value(QStringLiteral("error")).toString();
        session->completion(
            std::nullopt,
            error.isEmpty() ? QString() : error);
        return;
    }
    QFile pngFile(
        session->directory->filePath(QStringLiteral("preview.png")));
    QFile sceneFile(
        session->directory->filePath(QStringLiteral("scene.excalidraw")));
    if (!pngFile.open(QIODevice::ReadOnly) ||
        !sceneFile.open(QIODevice::ReadOnly)) {
        session->completion(
            std::nullopt,
            tr("The figure editor did not return both source and preview data."));
        return;
    }
    const QByteArray png = pngFile.read(
        static_cast<qint64>(core::kMaximumEncodedImageBytes) + 1);
    const QByteArray scene = sceneFile.read(
        static_cast<qint64>(kMaximumExcalidrawSceneBytes) + 1);
    QString embedError;
    const QByteArray embedded =
        pngWithExcalidrawScene(png, scene, embedError);
    if (embedded.isEmpty()) {
        session->completion(std::nullopt, embedError);
        return;
    }
    session->completion(Result{embedded, scene}, {});
}

std::optional<ExcalidrawFigureEditor::Result>
renderProfessionalExcalidrawSkeleton(const QJsonArray& skeleton,
                                     QString& error) {
    error.clear();
    ExcalidrawFigureEditor editor;
    std::optional<ExcalidrawFigureEditor::Result> result;
    bool completed = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        error = QObject::tr("The local Excalidraw renderer timed out.");
        loop.quit();
    });
    editor.renderProfessional(
        skeleton,
        [&](std::optional<ExcalidrawFigureEditor::Result> rendered,
            const QString& renderError) {
            result = std::move(rendered);
            error = renderError;
            completed = true;
            loop.quit();
        });
    if (!completed) {
        timeout.start(30000);
        loop.exec();
    }
    return result;
}

}  // namespace docxstudio::app
