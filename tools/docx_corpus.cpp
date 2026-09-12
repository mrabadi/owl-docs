#include "docx_corpus.h"

#include "docxstudio/ooxml/docx_document.h"

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace docxstudio::corpus {
namespace {

using Json = nlohmann::json;
using Package = std::map<std::string, std::vector<std::uint8_t>>;

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

class Sha256 {
public:
    void update(const std::uint8_t* bytes, std::size_t size) {
        total_bytes_ += static_cast<std::uint64_t>(size);
        while (size > 0) {
            const std::size_t available = block_.size() - block_size_;
            const std::size_t copied = std::min(available, size);
            std::copy_n(bytes, copied, block_.begin() +
                                      static_cast<std::ptrdiff_t>(block_size_));
            block_size_ += copied;
            bytes += copied;
            size -= copied;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() {
        const std::uint64_t total_bits = total_bytes_ * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                      block_.end(), 0U);
            transform(block_.data());
            block_size_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                  block_.begin() + 56, 0U);
        for (std::size_t index = 0; index < 8; ++index) {
            block_[63 - index] = static_cast<std::uint8_t>(
                total_bits >> static_cast<unsigned int>(index * 8));
        }
        transform(block_.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[word * 4 + byte] = static_cast<std::uint8_t>(
                    state_[word] >>
                    static_cast<unsigned int>((3 - byte) * 8));
            }
        }
        return digest;
    }

private:
    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index) {
            schedule[index] =
                (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < schedule.size(); ++index) {
            const std::uint32_t first =
                std::rotr(schedule[index - 15], 7) ^
                std::rotr(schedule[index - 15], 18) ^
                (schedule[index - 15] >> 3U);
            const std::uint32_t second =
                std::rotr(schedule[index - 2], 17) ^
                std::rotr(schedule[index - 2], 19) ^
                (schedule[index - 2] >> 10U);
            schedule[index] = schedule[index - 16] + first +
                              schedule[index - 7] + second;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const std::uint32_t sum_one = std::rotr(e, 6) ^
                                          std::rotr(e, 11) ^
                                          std::rotr(e, 25);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary_one = h + sum_one + choose +
                                                kSha256Constants[index] +
                                                schedule[index];
            const std::uint32_t sum_zero = std::rotr(a, 2) ^
                                           std::rotr(a, 13) ^
                                           std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary_two = sum_zero + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary_one;
            d = c;
            c = b;
            b = a;
            a = temporary_one + temporary_two;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_{0};
    std::uint64_t total_bytes_{0};
};

std::string bytesToHex(const std::array<std::uint8_t, 32>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::string requiredString(const Json& object, std::string_view key) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end() || !iterator->is_string() ||
        iterator->get_ref<const std::string&>().empty()) {
        throw std::runtime_error("manifest field '" + std::string(key) +
                                 "' must be a non-empty string");
    }
    return iterator->get<std::string>();
}

bool validDigest(std::string_view digest) {
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](const char value) {
               const unsigned char byte = static_cast<unsigned char>(value);
               return std::isdigit(byte) != 0 ||
                      (value >= 'a' && value <= 'f');
           });
}

bool validId(std::string_view id) {
    return !id.empty() &&
           std::all_of(id.begin(), id.end(), [](const char value) {
               const unsigned char byte = static_cast<unsigned char>(value);
               return std::isalnum(byte) != 0 || value == '-' || value == '_' ||
                      value == '.';
           });
}

Package readPackage(const std::filesystem::path& path) {
    int error_code = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error_code);
    if (archive == nullptr) throw std::runtime_error("cannot open saved ZIP");
    Package package;
    const zip_int64_t count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (count < 0) {
        zip_discard(archive);
        throw std::runtime_error("cannot enumerate saved ZIP");
    }
    for (zip_int64_t index = 0; index < count; ++index) {
        zip_stat_t status{};
        zip_stat_init(&status);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(index),
                           ZIP_FL_UNCHANGED, &status) != 0 ||
            status.name == nullptr) {
            zip_discard(archive);
            throw std::runtime_error("cannot inspect saved ZIP member");
        }
        if (status.size > static_cast<zip_uint64_t>(
                              std::numeric_limits<std::size_t>::max())) {
            zip_discard(archive);
            throw std::runtime_error("saved ZIP member is too large");
        }
        zip_file_t* member = zip_fopen_index(
            archive, static_cast<zip_uint64_t>(index), ZIP_FL_UNCHANGED);
        if (member == nullptr) {
            zip_discard(archive);
            throw std::runtime_error("cannot open saved ZIP member");
        }
        std::vector<std::uint8_t> contents(static_cast<std::size_t>(status.size));
        std::size_t consumed = 0;
        while (consumed < contents.size()) {
            const zip_int64_t read = zip_fread(
                member, contents.data() + consumed, contents.size() - consumed);
            if (read <= 0) {
                zip_fclose(member);
                zip_discard(archive);
                throw std::runtime_error("cannot read complete saved ZIP member");
            }
            consumed += static_cast<std::size_t>(read);
        }
        if (zip_fclose(member) != 0) {
            zip_discard(archive);
            throw std::runtime_error("cannot close saved ZIP member");
        }
        const auto [unused, inserted] =
            package.emplace(status.name, std::move(contents));
        static_cast<void>(unused);
        if (!inserted) {
            zip_discard(archive);
            throw std::runtime_error("saved ZIP contains duplicate members");
        }
    }
    zip_discard(archive);
    return package;
}

bool isTextOpeningTag(std::string_view opening) {
    const std::size_t name_start = opening.find_first_not_of("< \t\r\n");
    if (name_start == std::string_view::npos || opening[name_start] == '/') {
        return false;
    }
    const std::size_t name_end = opening.find_first_of(" \t\r\n>", name_start);
    const std::string_view name = opening.substr(name_start, name_end - name_start);
    const std::size_t colon = name.rfind(':');
    return (colon == std::string_view::npos ? name : name.substr(colon + 1)) == "t";
}

bool isTextClosingTag(std::string_view closing) {
    const std::size_t slash = closing.find("</");
    if (slash == std::string_view::npos) return false;
    const std::size_t name_start = slash + 2;
    const std::size_t name_end = closing.find_first_of(" \t\r\n>", name_start);
    const std::string_view name = closing.substr(name_start, name_end - name_start);
    const std::size_t colon = name.rfind(':');
    return (colon == std::string_view::npos ? name : name.substr(colon + 1)) == "t";
}

bool localizedTextOnlyChange(const std::vector<std::uint8_t>& original_bytes,
                             const std::vector<std::uint8_t>& edited_bytes) {
    const std::string_view original(
        reinterpret_cast<const char*>(original_bytes.data()), original_bytes.size());
    const std::string_view edited(
        reinterpret_cast<const char*>(edited_bytes.data()), edited_bytes.size());
    if (original == edited) return false;

    std::size_t prefix = 0;
    while (prefix < original.size() && prefix < edited.size() &&
           original[prefix] == edited[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < original.size() - prefix &&
           suffix < edited.size() - prefix &&
           original[original.size() - 1 - suffix] ==
               edited[edited.size() - 1 - suffix]) {
        ++suffix;
    }

    const std::size_t opening_start = original.rfind('<', prefix);
    if (opening_start == std::string_view::npos) return false;
    const std::size_t opening_end = original.find('>', opening_start);
    if (opening_end == std::string_view::npos || opening_end >= prefix ||
        !isTextOpeningTag(original.substr(
            opening_start, opening_end - opening_start + 1))) {
        return false;
    }
    const std::size_t original_change_end = original.size() - suffix;
    const std::size_t edited_change_end = edited.size() - suffix;
    const std::size_t original_closing = original.find('<', original_change_end);
    const std::size_t edited_closing = edited.find('<', edited_change_end);
    if (original_closing == std::string_view::npos ||
        edited_closing == std::string_view::npos ||
        !isTextClosingTag(original.substr(original_closing)) ||
        !isTextClosingTag(edited.substr(edited_closing))) {
        return false;
    }
    const std::string_view original_content = original.substr(
        opening_end + 1, original_closing - opening_end - 1);
    const std::string_view edited_content = edited.substr(
        opening_end + 1, edited_closing - opening_end - 1);
    if (original_content.find('<') != std::string_view::npos ||
        original_content.find('>') != std::string_view::npos ||
        edited_content.find('<') != std::string_view::npos ||
        edited_content.find('>') != std::string_view::npos) {
        return false;
    }
    return original.substr(0, opening_end + 1) ==
               edited.substr(0, opening_end + 1) &&
           original.substr(original_closing) == edited.substr(edited_closing);
}

std::optional<docxstudio::ooxml::TextSpanId> editableSpan(
    const docxstudio::ooxml::DocxDocument& document, std::size_t ordinal) {
    std::size_t current = 0;
    for (const auto& paragraph : document.paragraphs()) {
        for (const auto& run : paragraph.runs) {
            for (const auto& fragment : run.fragments) {
                if (fragment.kind != docxstudio::ooxml::FragmentKind::text ||
                    !fragment.editable || !fragment.text_span_id) {
                    continue;
                }
                if (current == ordinal) return fragment.text_span_id;
                ++current;
            }
        }
    }
    return std::nullopt;
}

FixtureResult runFixture(const Json& fixture,
                         const std::filesystem::path& manifest_directory,
                         const std::filesystem::path& output_directory) {
    FixtureResult result;
    try {
        result.id = requiredString(fixture, "id");
        if (!validId(result.id)) {
            throw std::runtime_error(
                "fixture id may contain only letters, digits, '.', '_', and '-'");
        }
        const std::string relative_path = requiredString(fixture, "path");
        const std::filesystem::path configured(relative_path);
        result.path = configured.is_absolute()
                          ? configured
                          : manifest_directory / configured;
        const std::string license = requiredString(fixture, "license");
        const std::string provenance = requiredString(fixture, "provenanceUrl");
        const std::string revision = requiredString(fixture, "sourceRevision");
        static_cast<void>(license);
        static_cast<void>(revision);
        if (!provenance.starts_with("https://") &&
            !provenance.starts_with("generated:") &&
            !provenance.starts_with("local-authorized:")) {
            throw std::runtime_error(
                "provenanceUrl must be HTTPS, generated:, or local-authorized:");
        }
        const std::string expected_digest = requiredString(fixture, "sha256");
        if (!validDigest(expected_digest)) {
            throw std::runtime_error(
                "sha256 must be 64 lowercase hexadecimal characters");
        }
        if (!std::filesystem::is_regular_file(result.path)) {
            throw std::runtime_error("fixture path is not a regular file");
        }
        std::set<std::string> checks;
        const auto checks_iterator = fixture.find("checks");
        if (checks_iterator == fixture.end() || !checks_iterator->is_array()) {
            throw std::runtime_error("checks must be a non-empty array");
        }
        for (const auto& check : *checks_iterator) {
            if (!check.is_string() || !checks.emplace(check.get<std::string>()).second) {
                throw std::runtime_error("checks must contain unique strings");
            }
        }
        if (!checks.contains("exact-copy")) {
            throw std::runtime_error("every corpus fixture must request exact-copy");
        }
        for (const auto& check : checks) {
            if (check != "exact-copy" && check != "localized-edit") {
                throw std::runtime_error("unknown fixture check '" + check + "'");
            }
        }

        docxstudio::ooxml::Error open_error;
        auto document = docxstudio::ooxml::DocxDocument::open(
            result.path, &open_error);
        if (!document) {
            throw std::runtime_error(
                "open failed: " +
                (open_error.message.empty() ? std::string("unknown error")
                                            : open_error.message));
        }
        result.opened = true;
        const std::filesystem::path exact_path =
            output_directory / (result.id + ".exact.docx");
        if (std::filesystem::exists(exact_path)) {
            throw std::runtime_error(
                "refusing to overwrite an existing exact-copy result");
        }
        const auto exact_save = document->saveAs(exact_path);
        if (!exact_save.saved || !exact_save.byte_identical_to_opened_file) {
            throw std::runtime_error(
                exact_save.error ? "exact Save As failed: " + exact_save.error->message
                                 : "exact Save As changed package bytes");
        }
        // Hash the retained bytes written by DocxDocument, rather than opening
        // the manifest path a second time. This keeps a path or symlink swap
        // between verification and open from substituting unverified input.
        if (sha256File(exact_path) != expected_digest) {
            std::error_code remove_error;
            std::filesystem::remove(exact_path, remove_error);
            throw std::runtime_error(
                "opened fixture SHA-256 does not match the manifest");
        }
        result.exact_copy_verified = true;

        if (checks.contains("localized-edit")) {
            const auto edit_configuration = fixture.find("localizedEdit");
            if (edit_configuration == fixture.end() ||
                !edit_configuration->is_object()) {
                throw std::runtime_error(
                    "localized-edit requires a localizedEdit object");
            }
            const std::size_t ordinal = edit_configuration->value(
                "textSpanOrdinal", std::numeric_limits<std::size_t>::max());
            if (ordinal == std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error(
                    "localizedEdit.textSpanOrdinal must be an unsigned integer");
            }
            const std::string replacement =
                requiredString(*edit_configuration, "replacement");
            auto span = editableSpan(*document, ordinal);
            if (!span) {
                throw std::runtime_error(
                    "localized edit span ordinal is not editable");
            }
            const auto edit = document->replaceText(*span, replacement);
            if (!edit.accepted) {
                throw std::runtime_error(
                    edit.error ? "localized edit failed: " + edit.error->message
                               : "localized edit was refused");
            }
            const std::filesystem::path edited_path =
                output_directory / (result.id + ".edited.docx");
            if (std::filesystem::exists(edited_path)) {
                throw std::runtime_error(
                    "refusing to overwrite an existing localized-edit result");
            }
            const auto edited_save = document->saveAs(edited_path);
            if (!edited_save.saved || edited_save.byte_identical_to_opened_file) {
                throw std::runtime_error(
                    edited_save.error
                        ? "localized Save As failed: " + edited_save.error->message
                        : "localized Save As incorrectly reported exact bytes");
            }

            // The exact copy is the already verified, immutable baseline for
            // locality comparison even if the original path changes later.
            const Package before = readPackage(exact_path);
            const Package after = readPackage(edited_path);
            if (before.size() != after.size()) {
                throw std::runtime_error(
                    "localized edit changed the package member set");
            }
            for (const auto& [name, contents] : before) {
                const auto saved = after.find(name);
                if (saved == after.end()) {
                    throw std::runtime_error(
                        "localized edit removed package member '" + name + "'");
                }
                if (name != "word/document.xml" && saved->second != contents) {
                    throw std::runtime_error(
                        "localized edit changed unrelated member '" + name + "'");
                }
            }
            const auto original_xml = before.find("word/document.xml");
            const auto edited_xml = after.find("word/document.xml");
            if (original_xml == before.end() || edited_xml == after.end() ||
                !localizedTextOnlyChange(original_xml->second, edited_xml->second)) {
                throw std::runtime_error(
                    "document.xml change was not confined to one w:t text node");
            }
            result.localized_edit_verified = true;
        }
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

}  // namespace

bool pathIsInsideGitWorkingTree(const std::filesystem::path& path) {
    std::error_code canonical_error;
    std::filesystem::path cursor =
        std::filesystem::weakly_canonical(path, canonical_error);
    if (canonical_error || cursor.empty()) {
        throw std::runtime_error("cannot resolve corpus output directory");
    }
    const auto isRegularFileOrMissing = [](const std::filesystem::path& candidate) {
        std::error_code status_error;
        const bool regular =
            std::filesystem::is_regular_file(candidate, status_error);
        if (status_error &&
            status_error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error(
                "cannot inspect corpus output directory ancestors");
        }
        return regular;
    };
    while (true) {
        const auto marker = cursor / ".git";
        const bool marker_exists =
            isRegularFileOrMissing(marker) ||
            isRegularFileOrMissing(marker / "HEAD");
        if (marker_exists) return true;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) break;
        cursor = parent;
    }
    return false;
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot hash fixture file");
    Sha256 digest;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            digest.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) throw std::runtime_error("failed while hashing fixture file");
    return bytesToHex(digest.finish());
}

RunReport runManifest(const std::filesystem::path& manifest,
                      const std::filesystem::path& output_directory,
                      std::optional<std::size_t> minimum_fixture_count) {
    RunReport report;
    try {
        std::ifstream input(manifest);
        if (!input) throw std::runtime_error("cannot open corpus manifest");
        Json root;
        input >> root;
        if (!root.is_object() || root.value("schema", std::string()) !=
                                     "owl-docs-corpus-v1") {
            throw std::runtime_error(
                "manifest schema must be 'owl-docs-corpus-v1'");
        }
        const auto fixtures = root.find("fixtures");
        if (fixtures == root.end() || !fixtures->is_array()) {
            throw std::runtime_error("manifest fixtures must be an array");
        }
        const std::size_t configured_minimum = root.value(
            "minimumFixtureCount", static_cast<std::size_t>(0));
        report.required_fixture_count =
            minimum_fixture_count.value_or(configured_minimum);
        report.fixture_count = fixtures->size();
        if (report.fixture_count < report.required_fixture_count) {
            throw std::runtime_error(
                "manifest contains " + std::to_string(report.fixture_count) +
                " fixtures but requires at least " +
                std::to_string(report.required_fixture_count));
        }
        if (pathIsInsideGitWorkingTree(output_directory)) {
            throw std::runtime_error(
                "corpus output directory must remain outside Git working trees");
        }
        std::error_code directory_error;
        std::filesystem::create_directories(output_directory, directory_error);
        if (directory_error || !std::filesystem::is_directory(output_directory)) {
            throw std::runtime_error("cannot create corpus output directory");
        }

        std::set<std::string> ids;
        const std::filesystem::path manifest_directory =
            std::filesystem::absolute(manifest).parent_path();
        report.fixtures.reserve(fixtures->size());
        for (const auto& fixture : *fixtures) {
            FixtureResult result = runFixture(
                fixture, manifest_directory, output_directory);
            if (!result.id.empty() && !ids.emplace(result.id).second) {
                result.error = "fixture id is duplicated";
                result.exact_copy_verified = false;
                result.localized_edit_verified = false;
            }
            report.opened_count += result.opened ? 1U : 0U;
            report.exact_copy_count += result.exact_copy_verified ? 1U : 0U;
            report.localized_edit_count +=
                result.localized_edit_verified ? 1U : 0U;
            report.fixtures.push_back(std::move(result));
        }
        report.passed =
            report.exact_copy_count == report.fixture_count &&
            std::all_of(report.fixtures.begin(), report.fixtures.end(),
                        [](const FixtureResult& result) {
                            return result.error.empty();
                        });
        if (!report.passed) {
            report.error = "one or more corpus fixtures failed";
        }
    } catch (const std::exception& exception) {
        report.error = exception.what();
        report.passed = false;
    }
    return report;
}

std::string reportJson(const RunReport& report) {
    Json output{{"ok", report.passed},
                {"fixtureCount", report.fixture_count},
                {"requiredFixtureCount", report.required_fixture_count},
                {"openedCount", report.opened_count},
                {"exactCopyCount", report.exact_copy_count},
                {"localizedEditCount", report.localized_edit_count}};
    if (!report.error.empty()) output["error"] = report.error;
    output["fixtures"] = Json::array();
    for (const auto& fixture : report.fixtures) {
        Json fixture_json{{"id", fixture.id},
                          {"path", fixture.path.string()},
                          {"opened", fixture.opened},
                          {"exactCopyVerified", fixture.exact_copy_verified},
                          {"localizedEditVerified",
                           fixture.localized_edit_verified}};
        if (!fixture.error.empty()) fixture_json["error"] = fixture.error;
        output["fixtures"].push_back(std::move(fixture_json));
    }
    return output.dump(2) + "\n";
}

}  // namespace docxstudio::corpus
