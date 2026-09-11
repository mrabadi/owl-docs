#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

class QFile;

namespace docxstudio::app {

struct FileFingerprint {
    qint64 size{};
    qint64 modified_msecs_since_epoch{};
    QByteArray sha256;

    bool operator==(const FileFingerprint&) const = default;
};

enum class FileFingerprintComparison {
    matches,
    differs,
    unavailable,
};

// Fingerprints the bytes represented by an already-open file descriptor and
// restores the original file position before returning.
std::optional<FileFingerprint> fingerprintOpenFile(QFile& file, QString& error);

std::optional<FileFingerprint> fingerprintFile(const QString& path,
                                                QString& error);

// Always hashes the current file. Size and timestamp are not used as a
// shortcut because another writer can preserve both values.
FileFingerprintComparison compareFileFingerprint(
    const QString& path, const FileFingerprint& expected, QString& error);

}  // namespace docxstudio::app
