#include "docxstudio/app/FileFingerprint.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "could not open fingerprint fixture for writing");
    check(file.write(contents) == contents.size(),
          "could not write fingerprint fixture");
    check(file.flush(), "could not flush fingerprint fixture");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory failed");
    const QString path = temporary.filePath(QStringLiteral("document.docx"));
    writeFile(path, QByteArrayLiteral("alpha"));

    QFile held(path);
    check(held.open(QIODevice::ReadOnly), "could not open held fixture");
    check(held.seek(2), "could not position held fixture");
    QString error;
    const auto original = docxstudio::app::fingerprintOpenFile(held, error);
    check(original.has_value(), "could not fingerprint an open file");
    check(error.isEmpty(), "successful fingerprint returned an error");
    check(held.pos() == 2, "fingerprinting did not restore the file position");
    check(original->size == 5, "fingerprint recorded the wrong size");
    check(original->sha256.size() == 32, "fingerprint is not SHA-256");
    held.close();

    check(docxstudio::app::compareFileFingerprint(path, *original, error) ==
              docxstudio::app::FileFingerprintComparison::matches,
          "unchanged file did not match its fingerprint");

    // Prove that comparison does not trust size and modification time alone:
    // use the replacement's exact metadata with the original content hash.
    writeFile(path, QByteArrayLiteral("bravo"));
    const auto replacement = docxstudio::app::fingerprintFile(path, error);
    check(replacement.has_value(), "could not fingerprint replacement file");
    docxstudio::app::FileFingerprint forgedMetadata = *replacement;
    forgedMetadata.sha256 = original->sha256;
    check(docxstudio::app::compareFileFingerprint(path, forgedMetadata, error) ==
              docxstudio::app::FileFingerprintComparison::differs,
          "same-size, same-mtime metadata bypassed the content hash");
    check(docxstudio::app::compareFileFingerprint(path, *replacement, error) ==
              docxstudio::app::FileFingerprintComparison::matches,
          "replacement did not match its own fingerprint");

    check(QFile::remove(path), "could not remove fingerprint fixture");
    check(docxstudio::app::compareFileFingerprint(path, *replacement, error) ==
              docxstudio::app::FileFingerprintComparison::unavailable,
          "missing file was not reported as unavailable");
    check(!error.isEmpty(), "unavailable fingerprint returned no diagnostic");

    std::cout << "file fingerprint tests passed\n";
    return 0;
}
