#include "docxstudio/app/FileFingerprint.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <array>

namespace docxstudio::app {
namespace {

constexpr qsizetype kHashBufferSize = 256 * 1024;

}  // namespace

std::optional<FileFingerprint> fingerprintOpenFile(QFile& file, QString& error) {
    error.clear();
    if (!file.isOpen() || !(file.openMode() & QIODevice::ReadOnly)) {
        error = QStringLiteral("The file is not open for reading.");
        return std::nullopt;
    }
    if (file.isSequential()) {
        error = QStringLiteral("The file is not seekable.");
        return std::nullopt;
    }

    const qint64 originalPosition = file.pos();
    if (originalPosition < 0 || !file.seek(0)) {
        error = file.errorString();
        return std::nullopt;
    }

    QFileInfo before(file);
    before.refresh();
    const qint64 beforeSize = before.size();
    const auto beforeModified = before.lastModified();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, static_cast<std::size_t>(kHashBufferSize)> buffer{};
    while (true) {
        const qint64 count = file.read(buffer.data(), kHashBufferSize);
        if (count < 0) {
            error = file.errorString();
            static_cast<void>(file.seek(originalPosition));
            return std::nullopt;
        }
        if (count == 0) break;
        hash.addData(buffer.data(), static_cast<qsizetype>(count));
    }

    QFileInfo after(file);
    after.refresh();
    if (!file.seek(originalPosition)) {
        error = file.errorString();
        return std::nullopt;
    }
    if (beforeSize != after.size() || beforeModified != after.lastModified()) {
        error = QStringLiteral("The file changed while it was being fingerprinted.");
        return std::nullopt;
    }

    return FileFingerprint{
        after.size(), after.lastModified().toMSecsSinceEpoch(), hash.result()};
}

std::optional<FileFingerprint> fingerprintFile(const QString& path,
                                                QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return std::nullopt;
    }
    return fingerprintOpenFile(file, error);
}

FileFingerprintComparison compareFileFingerprint(
    const QString& path, const FileFingerprint& expected, QString& error) {
    const auto current = fingerprintFile(path, error);
    if (!current) return FileFingerprintComparison::unavailable;
    return *current == expected ? FileFingerprintComparison::matches
                                : FileFingerprintComparison::differs;
}

}  // namespace docxstudio::app
