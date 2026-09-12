#include "docx_corpus.h"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/owl-docs-corpus-tests-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / std::string(name);
    }

private:
    std::filesystem::path path_;
};

void addMember(zip_t* archive, const std::string& name,
               const std::string& contents) {
    void* owned = nullptr;
    if (!contents.empty()) {
        owned = std::malloc(contents.size());
        check(owned != nullptr, "could not allocate fixture ZIP member");
        std::memcpy(owned, contents.data(), contents.size());
    }
    zip_source_t* source = zip_source_buffer(
        archive, owned, static_cast<zip_uint64_t>(contents.size()), 1);
    if (source == nullptr) std::free(owned);
    check(source != nullptr, "could not create fixture ZIP source");
    if (zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(source);
        throw std::runtime_error("could not add fixture ZIP member");
    }
}

void createFixture(const std::filesystem::path& path, std::string_view text) {
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "could not create fixture DOCX");
    addMember(
        archive, "[Content_Types].xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "</Types>");
    addMember(
        archive, "_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>");
    addMember(
        archive, "word/document.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body><w:p><w:r><w:t>" + std::string(text) +
            "</w:t></w:r></w:p><w:sectPr/></w:body></w:document>");
    addMember(archive, "customXml/item.bin",
              std::string("opaque\0corpus", 13));
    check(zip_close(archive) == 0, "could not finalize fixture DOCX");
}

void writeJson(const std::filesystem::path& path, const Json& value) {
    std::ofstream output(path);
    check(output.good(), "could not write corpus test manifest");
    output << value.dump(2) << '\n';
    check(output.good(), "could not finish corpus test manifest");
}

Json fixtureEntry(const std::string& id, const std::filesystem::path& path,
                  bool localized) {
    Json fixture{{"id", id},
                 {"path", path.filename().string()},
                 {"license", "generated Owl Docs test data"},
                 {"provenanceUrl", "generated:docx-corpus-runner-tests"},
                 {"sourceRevision", "generated-fixture-v1"},
                 {"sha256", docxstudio::corpus::sha256File(path)},
                 {"checks", Json::array({"exact-copy"})}};
    if (localized) {
        fixture["checks"].push_back("localized-edit");
        fixture["localizedEdit"] =
            {{"textSpanOrdinal", 0}, {"replacement", "Corpus replacement"}};
    }
    return fixture;
}

void testSha256(const TemporaryDirectory& temporary) {
    const auto empty = temporary.file("empty.bin");
    std::ofstream(empty, std::ios::binary);
    check(docxstudio::corpus::sha256File(empty) ==
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855",
          "SHA-256 implementation differs for an empty input");

    const auto abc = temporary.file("abc.bin");
    {
        std::ofstream output(abc, std::ios::binary);
        output << "abc";
    }
    check(docxstudio::corpus::sha256File(abc) ==
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad",
          "SHA-256 implementation differs for the standard abc vector");
}

void testExactAndLocalizedChecks(const TemporaryDirectory& temporary) {
    const auto exact = temporary.file("exact.docx");
    const auto localized = temporary.file("localized.docx");
    createFixture(exact, "Exact fixture");
    createFixture(localized, "Original");
    const Json manifest{
        {"schema", "owl-docs-corpus-v1"},
        {"minimumFixtureCount", 2},
        {"fixtures", Json::array({fixtureEntry("exact", exact, false),
                                  fixtureEntry("localized", localized, true)})}};
    const auto manifest_path = temporary.file("manifest.json");
    writeJson(manifest_path, manifest);

    const auto report = docxstudio::corpus::runManifest(
        manifest_path, temporary.file("results"));
    check(report.passed, report.error);
    check(report.fixture_count == 2 && report.opened_count == 2 &&
              report.exact_copy_count == 2 && report.localized_edit_count == 1,
          "corpus report counts do not describe successful checks");
    check(std::filesystem::is_regular_file(
              temporary.file("results/exact.exact.docx")) &&
              std::filesystem::is_regular_file(
                  temporary.file("results/localized.edited.docx")),
          "corpus runner did not retain its verification outputs");
    const Json serialized = Json::parse(docxstudio::corpus::reportJson(report));
    check(serialized.at("ok").get<bool>() &&
              serialized.at("exactCopyCount").get<std::size_t>() == 2,
          "corpus JSON report is not machine readable");

    const auto existing_result = temporary.file("results/exact.exact.docx");
    const std::string existing_digest =
        docxstudio::corpus::sha256File(existing_result);
    const auto repeated = docxstudio::corpus::runManifest(
        manifest_path, temporary.file("results"));
    check(!repeated.passed &&
              std::any_of(repeated.fixtures.begin(), repeated.fixtures.end(),
                          [](const auto& fixture) {
                              return fixture.error.find("refusing to overwrite") !=
                                     std::string::npos;
                          }) &&
              docxstudio::corpus::sha256File(existing_result) == existing_digest,
          "corpus runner overwrote an existing result on a repeated run");
}

void testManifestValidation(const TemporaryDirectory& temporary) {
    const auto source = temporary.file("validation.docx");
    createFixture(source, "Validation");
    Json entry = fixtureEntry("validation", source, false);
    entry.erase("license");
    const auto missing_license = temporary.file("missing-license.json");
    writeJson(missing_license,
              {{"schema", "owl-docs-corpus-v1"},
               {"minimumFixtureCount", 1},
               {"fixtures", Json::array({entry})}});
    auto report = docxstudio::corpus::runManifest(
        missing_license, temporary.file("missing-license-output"));
    check(!report.passed && report.fixtures.size() == 1 &&
              report.fixtures[0].error.find("license") != std::string::npos,
          "manifest accepted a fixture with no license declaration");

    entry = fixtureEntry("validation", source, false);
    entry["sha256"] = std::string(64, '0');
    const auto wrong_digest = temporary.file("wrong-digest.json");
    writeJson(wrong_digest,
              {{"schema", "owl-docs-corpus-v1"},
               {"minimumFixtureCount", 1},
               {"fixtures", Json::array({entry})}});
    report = docxstudio::corpus::runManifest(
        wrong_digest, temporary.file("wrong-digest-output"));
    check(!report.passed && report.fixtures[0].error.find("SHA-256") !=
                                std::string::npos,
          "manifest accepted a fixture whose digest changed");
    check(!std::filesystem::exists(
              temporary.file("wrong-digest-output/validation.exact.docx")),
          "a digest mismatch retained an unverified exact-copy result");

    const auto too_small = temporary.file("too-small.json");
    writeJson(too_small,
              {{"schema", "owl-docs-corpus-v1"},
               {"minimumFixtureCount", 2},
               {"fixtures", Json::array({fixtureEntry(
                                   "validation", source, false)})}});
    report = docxstudio::corpus::runManifest(
        too_small, temporary.file("too-small-output"));
    check(!report.passed && report.error.find("requires at least 2") !=
                                std::string::npos,
          "manifest minimum did not prevent an undersized corpus run");
}

void testOutputCannotEnterGitWorkingTree(
    const TemporaryDirectory& temporary) {
    const auto source = temporary.file("git-output-source.docx");
    createFixture(source, "Private-safe boundary");
    const auto manifest_path = temporary.file("git-output-manifest.json");
    writeJson(
        manifest_path,
        {{"schema", "owl-docs-corpus-v1"},
         {"minimumFixtureCount", 1},
         {"fixtures", Json::array(
                          {fixtureEntry("git-output", source, false)})}});

    const auto repository = temporary.file("synthetic-repository");
    std::filesystem::create_directories(repository / ".git");
    {
        std::ofstream head(repository / ".git/HEAD");
        head << "ref: refs/heads/main\n";
        check(head.good(), "could not create synthetic Git marker");
    }
    auto report = docxstudio::corpus::runManifest(
        manifest_path, repository / "corpus-results");
    check(!report.passed &&
              report.error.find("outside Git working trees") !=
                  std::string::npos &&
              !std::filesystem::exists(repository / "corpus-results"),
          "corpus output was allowed inside a Git working tree");

    const auto alias = temporary.file("repository-alias");
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(
        repository, alias, symlink_error);
    check(!symlink_error, "could not create output-boundary symlink fixture");
    report = docxstudio::corpus::runManifest(
        manifest_path, alias / "aliased-results");
    check(!report.passed &&
              report.error.find("outside Git working trees") !=
                  std::string::npos &&
              !std::filesystem::exists(repository / "aliased-results"),
          "a symlink bypassed the Git working-tree output boundary");
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        testSha256(temporary);
        testExactAndLocalizedChecks(temporary);
        testManifestValidation(temporary);
        testOutputCannotEnterGitWorkingTree(temporary);
        std::cout << "DOCX corpus runner tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "DOCX corpus runner test failure: " << exception.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
