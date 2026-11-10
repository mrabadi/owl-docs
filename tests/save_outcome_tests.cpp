#include "docxstudio/ooxml/docx_document.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool failDirectorySync = false;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}

extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor) {
    struct stat status {};
    if (failDirectorySync && ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode)) {
        failDirectorySync = false;
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}

int main() {
    using namespace docxstudio::ooxml;
    char pattern[] = "/tmp/owl-docs-save-outcome-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    check(directory != nullptr, "could not create save outcome fixture directory");
    const auto path = std::filesystem::path(directory) / "document.docx";
    check(DocxDocument::writeNew(path, {NewParagraph{{NewRun{"Original", {}}}}}).saved,
          "could not create original save outcome fixture");
    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document && document->replaceParagraphText(0, "Saved changes").accepted,
          "could not prepare save outcome edit");
    failDirectorySync = true;
    const auto result = document->saveAs(path);
    check(result.saved && !result.error && result.durability_warning && !document->dirty(),
          "committed document was reported as failed after a directory sync error");
    auto reopened = DocxDocument::open(path, &error);
    check(reopened && reopened->paragraphs()[0].plainText() == "Saved changes",
          "committed save did not contain its reported changes");
    failDirectorySync = true;
    const auto authored = DocxDocument::writeNew(path, {NewParagraph{{NewRun{"New changes", {}}}}});
    check(authored.saved && !authored.error && authored.durability_warning,
          "new document commit was reported as failed after a directory sync error");
    check(authored.validated_package_bytes && !authored.validated_package_bytes->empty() &&
              DocxDocument::openBytes(*authored.validated_package_bytes, &error),
          "new save did not return its validated committed bytes as a stable baseline");
    std::filesystem::remove_all(directory);
    std::cout << "save outcome tests passed\n";
}
