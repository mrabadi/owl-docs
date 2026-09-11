#include "docxstudio/app/QtProcess.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace docxstudio::app {

QtProcess::QtProcess(QObject* parent) : QObject(parent), process_(new QProcess(this)) {
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray bytes = process_->readAllStandardOutput();
        if (callbacks_.standardOutput) {
            callbacks_.standardOutput(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        }
    });
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray bytes = process_->readAllStandardError();
        if (callbacks_.standardError) {
            callbacks_.standardError(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        }
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                if (callbacks_.exited) {
                    callbacks_.exited(code);
                }
            });
}

QtProcess::~QtProcess() { stop(); }

QString QtProcess::resolveProgram(const std::string& requested) const {
    const QString candidate = QString::fromStdString(requested);
    const QString desktopBundled = QStringLiteral("/usr/lib/chatgpt/resources/codex");
    if (requested == "codex" && QFileInfo(desktopBundled).isExecutable()) {
        return desktopBundled;
    }
    if (candidate.contains(QLatin1Char('/'))) {
        return QFileInfo(candidate).isExecutable() ? QFileInfo(candidate).absoluteFilePath()
                                                    : QString();
    } else {
        const auto found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

bool QtProcess::start(const codex::ProcessSpec& spec,
                      codex::ProcessCallbacks callbacks,
                      std::string& error) {
    if (process_->state() != QProcess::NotRunning) {
        error = "process is already running";
        return false;
    }
    const auto program = resolveProgram(spec.program);
    if (program.isEmpty()) {
        error = "compatible Codex executable was not found";
        return false;
    }
    callbacks_ = std::move(callbacks);
    QStringList arguments;
    for (const auto& argument : spec.arguments) {
        arguments.push_back(QString::fromStdString(argument));
    }
    process_->setWorkingDirectory(QString::fromStdString(spec.workingDirectory));
    const auto inherited = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment environment;
    for (const auto* name : {"HOME", "USER", "LOGNAME", "LANG", "LANGUAGE",
                             "LC_ALL", "LC_CTYPE", "XDG_CONFIG_HOME",
                             "XDG_DATA_HOME", "XDG_CACHE_HOME", "CODEX_HOME",
                             "SSL_CERT_FILE", "SSL_CERT_DIR", "TZ"}) {
        const QString key = QString::fromLatin1(name);
        if (inherited.contains(key)) environment.insert(key, inherited.value(key));
    }
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    process_->setProcessEnvironment(environment);
    process_->setProgram(program);
    process_->setArguments(arguments);
    process_->start(QIODevice::ReadWrite);
    if (!process_->waitForStarted(5000)) {
        error = process_->errorString().toStdString();
        process_->kill();
        process_->waitForFinished(1000);
        callbacks_ = {};
        return false;
    }
    return true;
}

bool QtProcess::writeStandardInput(std::string_view bytes, std::string& error) {
    if (process_->state() == QProcess::NotRunning) {
        error = "Codex process is not running";
        return false;
    }
    qint64 offset = 0;
    const qint64 size = static_cast<qint64>(bytes.size());
    while (offset < size) {
        const qint64 written = process_->write(bytes.data() + offset, size - offset);
        if (written <= 0) {
            error = process_->errorString().toStdString();
            return false;
        }
        offset += written;
    }
    return true;
}

void QtProcess::stop() noexcept {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
    process_->closeWriteChannel();
    process_->terminate();
    if (!process_->waitForFinished(1500)) {
        process_->kill();
        process_->waitForFinished(1000);
    }
}

}  // namespace docxstudio::app
