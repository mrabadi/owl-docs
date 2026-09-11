#include "docxstudio/worker/client.h"
#include "docxstudio/worker/protocol.h"
#include "docxstudio/worker/sandbox.h"

#include <zip.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using docxstudio::worker::ParsedParagraph;
using docxstudio::worker::ParserClientOptions;
using docxstudio::worker::ParserClientStatus;
using docxstudio::worker::ParserCompatibility;
using docxstudio::worker::ParserResponse;
using docxstudio::worker::ParserStatus;
using docxstudio::worker::ParserLimits;
using docxstudio::worker::ParserWorkerOptions;
using docxstudio::worker::SandboxError;
using docxstudio::worker::SandboxLimits;

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/docxstudio-worker-tests-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* path = ::mkdtemp(writable.data());
        if (path == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = path;
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

void addMember(zip_t* archive, const std::string& name, std::string_view contents) {
    void* owned = nullptr;
    if (!contents.empty()) {
        owned = std::malloc(contents.size());
        check(owned != nullptr, "malloc failed for worker ZIP fixture");
        std::memcpy(owned, contents.data(), contents.size());
    }
    zip_source_t* source =
        zip_source_buffer(archive, owned, static_cast<zip_uint64_t>(contents.size()), 1);
    if (source == nullptr) {
        std::free(owned);
        throw std::runtime_error("zip_source_buffer failed");
    }
    if (zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
        zip_source_free(source);
        throw std::runtime_error("zip_file_add failed");
    }
}

void createDocxWithDocument(
    const std::filesystem::path& path,
    std::string_view document,
    const std::vector<std::pair<std::string, std::string>>& extra_members = {}) {
    constexpr std::string_view content_types =
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "</Types>";
    constexpr std::string_view relationships =
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>";
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "zip_open failed for worker fixture");
    addMember(archive, "[Content_Types].xml", content_types);
    addMember(archive, "_rels/.rels", relationships);
    addMember(archive, "word/document.xml", document);
    addMember(archive, "customXml/opaque.bin", std::string_view("\0\1\2", 3));
    for (const auto& [name, contents] : extra_members) {
        addMember(archive, name, contents);
    }
    check(zip_close(archive) == 0, "zip_close failed for worker fixture");
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    check(stream.good(), "could not create malformed parser fixture");
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    check(stream.good(), "could not write malformed parser fixture");
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "could not read parser fixture");
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

ParserResponse parseFile(const std::filesystem::path& path,
                         const ParserLimits& limits = {}) {
    const int input = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    check(input >= 0, "could not open parser fixture");
    return docxstudio::worker::parseOwnedDocxDescriptor(input, limits);
}

void createDocx(const std::filesystem::path& path) {
    constexpr std::string_view document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:r><w:t>Hello &amp; worker</w:t></w:r>"
        "<w:r><w:tab/><w:t>tail</w:t></w:r></w:p>"
        "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Table</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:sectPr/>"
        "</w:body></w:document>";
    createDocxWithDocument(path, document);
}

std::filesystem::path parserWorkerPath() {
#if defined(DOCXSTUDIO_PARSER_WORKER_PATH)
    return std::filesystem::path(DOCXSTUDIO_PARSER_WORKER_PATH);
#else
    if (const char* configured = std::getenv("DOCXSTUDIO_PARSER_WORKER")) {
        return std::filesystem::path(configured);
    }
    return std::filesystem::canonical("/proc/self/exe").parent_path() /
           "owl-docs-parser-worker";
#endif
}

std::filesystem::path installFakeHelper(
    const TemporaryDirectory& temporary,
    std::string_view name) {
    const std::filesystem::path helper = temporary.file(name);
    std::filesystem::create_symlink(std::filesystem::canonical("/proc/self/exe"), helper);
    return helper;
}

int outputDescriptorFromArguments(int argc, char** argv) {
    constexpr std::string_view prefix = "--output-fd=";
    for (int index = 1; index < argc; ++index) {
        std::string_view argument(argv[index]);
        if (!argument.starts_with(prefix)) {
            continue;
        }
        argument.remove_prefix(prefix.size());
        unsigned value = 0;
        const auto parsed =
            std::from_chars(argument.data(), argument.data() + argument.size(), value, 10);
        if (parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size() &&
            value <= static_cast<unsigned>(INT_MAX)) {
            return static_cast<int>(value);
        }
    }
    return -1;
}

int runFakeHelperIfRequested(int argc, char** argv) {
    const std::string name = std::filesystem::path(argv[0]).filename();
    if (name == "fake-timeout-worker") {
        (void)::poll(nullptr, 0, 5000);
        return 0;
    }
    if (name == "fake-protocol-worker") {
        const int output = outputDescriptorFromArguments(argc, argv);
        if (output < 0) {
            return 91;
        }
        constexpr std::array<std::uint8_t, 3> malformed{'B', 'A', 'D'};
        const ssize_t ignored = ::write(output, malformed.data(), malformed.size());
        (void)ignored;
        return 0;
    }
    if (name == "fake-exit-worker") {
        return 23;
    }
    if (name == "fake-signal-worker") {
        (void)::raise(SIGTERM);
        return 92;
    }
    if (name == "fake-delayed-signal-worker") {
        const int output = outputDescriptorFromArguments(argc, argv);
        if (output < 0 || ::close(output) != 0) {
            return 93;
        }
        (void)::poll(nullptr, 0, 50);
        (void)::raise(SIGTERM);
        return 94;
    }
    return -1;
}

void testProtocolRoundTrip() {
    int descriptors[2]{};
    check(::pipe2(descriptors, O_CLOEXEC) == 0, "pipe2 failed");
    ParserResponse original;
    original.status = ParserStatus::ok;
    original.compatibility = ParserCompatibility::complex_body_preserved;
    original.paragraphs = {ParsedParagraph{{"one", " two"}}, ParsedParagraph{{"three"}}};
    original.warnings = {"opaque table retained"};

    std::string error;
    check(
        docxstudio::worker::writeParserResponse(descriptors[1], original, 1024 * 1024, &error),
        error);
    ::close(descriptors[1]);
    ParserResponse decoded;
    check(
        docxstudio::worker::readParserResponse(descriptors[0], decoded, 1024 * 1024, &error),
        error);
    ::close(descriptors[0]);
    check(decoded.status == original.status, "protocol status changed");
    check(decoded.compatibility == original.compatibility, "protocol compatibility changed");
    check(decoded.paragraphs == original.paragraphs, "protocol paragraphs changed");
    check(decoded.warnings == original.warnings, "protocol warnings changed");
}

struct SandboxProbe {
    int installed{0};
    int no_new_privs{0};
    int ipv4_denied{0};
    int ipv6_denied{0};
    int unix_socket_denied{0};
    int path_open_denied{0};
    int process_creation_denied{0};
    int install_error{0};
};

void testSandboxDenialsInForkedChild() {
    int result_pipe[2]{};
    check(::pipe2(result_pipe, O_CLOEXEC) == 0, "probe pipe2 failed");
    const pid_t child = ::fork();
    check(child >= 0, "fork failed for sandbox probe");
    if (child == 0) {
        ::close(result_pipe[0]);
        SandboxProbe probe;
        SandboxError error;
        SandboxLimits limits;
        limits.address_space_bytes = 512ULL * 1024ULL * 1024ULL;
        limits.cpu_seconds = 5;
        limits.open_file_descriptors = 16;
        probe.installed = docxstudio::worker::installParserSandbox(limits, &error) ? 1 : 0;
        probe.install_error = error.native_error;
        if (probe.installed != 0) {
            probe.no_new_privs = ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1 ? 1 : 0;
            errno = 0;
            const int ipv4 = ::socket(AF_INET, SOCK_STREAM, 0);
            probe.ipv4_denied = ipv4 == -1 && errno == EPERM ? 1 : 0;
            if (ipv4 >= 0) {
                ::close(ipv4);
            }
            errno = 0;
            const int ipv6 = ::socket(AF_INET6, SOCK_STREAM, 0);
            probe.ipv6_denied = ipv6 == -1 && errno == EPERM ? 1 : 0;
            if (ipv6 >= 0) {
                ::close(ipv6);
            }
            errno = 0;
            const int local = ::socket(AF_UNIX, SOCK_STREAM, 0);
            probe.unix_socket_denied = local == -1 && errno == EPERM ? 1 : 0;
            if (local >= 0) {
                ::close(local);
            }
            errno = 0;
            const int file = ::open("/etc/passwd", O_RDONLY | O_CLOEXEC);
            probe.path_open_denied = file == -1 && errno == EPERM ? 1 : 0;
            if (file >= 0) {
                ::close(file);
            }
            errno = 0;
            const pid_t nested = ::fork();
            probe.process_creation_denied = nested == -1 && errno == EPERM ? 1 : 0;
            if (nested == 0) {
                _exit(99);
            }
        }
        const ssize_t ignored = ::write(result_pipe[1], &probe, sizeof(probe));
        (void)ignored;
        _exit(0);
    }

    ::close(result_pipe[1]);
    SandboxProbe probe;
    const ssize_t count = ::read(result_pipe[0], &probe, sizeof(probe));
    ::close(result_pipe[0]);
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "waitpid failed for sandbox probe");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "sandbox probe child failed");
    check(count == static_cast<ssize_t>(sizeof(probe)), "sandbox probe response was truncated");
    check(probe.installed != 0, "libseccomp sandbox installation failed");
    check(probe.no_new_privs != 0, "PR_SET_NO_NEW_PRIVS was not retained");
    check(probe.ipv4_denied != 0, "AF_INET socket was not denied with EPERM");
    check(probe.ipv6_denied != 0, "AF_INET6 socket was not denied with EPERM");
    check(probe.unix_socket_denied != 0, "AF_UNIX socket creation was not denied");
    check(probe.path_open_denied != 0, "path-based open was not denied");
    check(probe.process_creation_denied != 0, "process creation was not denied");
}

void testSandboxedParserWorker(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("worker.docx");
    createDocx(path);
    const int input = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    check(input >= 0, "could not open worker fixture");
    int response_pipe[2]{};
    check(::pipe2(response_pipe, O_CLOEXEC) == 0, "response pipe2 failed");

    const pid_t child = ::fork();
    check(child >= 0, "fork failed for parser worker");
    if (child == 0) {
        ::close(response_pipe[0]);
        ParserWorkerOptions options;
        options.sandbox.address_space_bytes = 512ULL * 1024ULL * 1024ULL;
        options.sandbox.cpu_seconds = 5;
        const int worker_status =
            docxstudio::worker::runParserWorker(input, response_pipe[1], options);
        _exit(worker_status);
    }

    ::close(input);
    ::close(response_pipe[1]);
    ParserResponse response;
    std::string protocol_error;
    check(
        docxstudio::worker::readParserResponse(
            response_pipe[0], response, 128ULL * 1024ULL * 1024ULL, &protocol_error),
        protocol_error);
    ::close(response_pipe[0]);
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "waitpid failed for parser worker");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "parser worker process failed");
    check(response.ok(), response.error);
    check(
        response.compatibility == ParserCompatibility::complex_body_preserved,
        "table document should be classified as complex body");
    check(response.paragraphs.size() == 2, "worker paragraph count differs");
    check(response.paragraphs[0].plainText() == "Hello & worker\ttail", "worker text differs");
    check(response.paragraphs[1].plainText() == "Table", "worker table text differs");
}

void testWorkerXmlComplexityLimits(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("deep-worker.docx");
    std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>";
    for (int depth = 0; depth < 300; ++depth) document += "<w:sdt>";
    document += "<w:p><w:r><w:t>Too deep</w:t></w:r></w:p>";
    for (int depth = 0; depth < 300; ++depth) document += "</w:sdt>";
    document += "<w:sectPr/></w:body></w:document>";
    createDocxWithDocument(path, document);

    const int input = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    check(input >= 0, "could not open deep worker fixture");
    const ParserResponse response =
        docxstudio::worker::parseOwnedDocxDescriptor(input);
    check(response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce its XML depth limit");
    check(response.error.find("depth limit") != std::string::npos,
          "worker depth rejection did not identify the exceeded limit");

    const auto wide_path = temporary.file("node-count-worker.docx");
    createDocx(wide_path);
    const int wide_input = ::open(wide_path.c_str(), O_RDONLY | O_CLOEXEC);
    check(wide_input >= 0, "could not open node-count worker fixture");
    ParserLimits limits;
    limits.max_xml_nodes = 7;
    const ParserResponse wide_response =
        docxstudio::worker::parseOwnedDocxDescriptor(wide_input, limits);
    check(wide_response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce its XML node-count limit");
    check(wide_response.error.find("node-count limit") != std::string::npos,
          "worker node-count rejection did not identify the exceeded limit");
}

void testWorkerPackageAndMediaLimits(const TemporaryDirectory& temporary) {
    const auto ordinary = temporary.file("ordinary-limits.docx");
    createDocx(ordinary);

    ParserLimits limits;
    limits.max_package_bytes =
        static_cast<std::uint64_t>(std::filesystem::file_size(ordinary) - 1);
    auto response = parseFile(ordinary, limits);
    check(response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce the compressed package byte limit");

    limits = {};
    limits.max_member_count = 3;
    response = parseFile(ordinary, limits);
    check(response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce the ZIP member-count limit");

    limits = {};
    limits.max_member_uncompressed_bytes = 32;
    response = parseFile(ordinary, limits);
    check(response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce the per-member expansion limit");

    limits = {};
    limits.max_document_xml_bytes = 32;
    response = parseFile(ordinary, limits);
    check(response.status == ParserStatus::package_limit_exceeded,
          "worker did not enforce the document.xml byte limit");

    constexpr std::string_view minimal_document =
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>media"
        "</w:t></w:r></w:p></w:body></w:document>";
    const auto one_media = temporary.file("one-media-limit.docx");
    createDocxWithDocument(
        one_media, minimal_document,
        {{"word/media/oversized.png", std::string(17, 'x')}});
    limits = {};
    limits.max_media_member_uncompressed_bytes = 16;
    response = parseFile(one_media, limits);
    check(response.status == ParserStatus::package_limit_exceeded &&
              response.error.find("media expansion") != std::string::npos,
          "worker did not enforce or identify the per-media limit");

    const auto aggregate_media = temporary.file("aggregate-media-limit.docx");
    createDocxWithDocument(
        aggregate_media, minimal_document,
        {{"word/media/one.png", std::string(9, 'a')},
         {"word/media/two.jpg", std::string(9, 'b')}});
    limits = {};
    limits.max_media_member_uncompressed_bytes = 10;
    limits.max_total_media_uncompressed_bytes = 16;
    response = parseFile(aggregate_media, limits);
    check(response.status == ParserStatus::package_limit_exceeded &&
              response.error.find("media expansion") != std::string::npos,
          "worker did not enforce or identify the aggregate media limit");

    limits = {};
    limits.max_total_media_uncompressed_bytes = 0;
    response = parseFile(ordinary, limits);
    check(response.status == ParserStatus::invalid_request,
          "worker accepted a zero media expansion limit");
}

void testMalformedPackageAndXml(const TemporaryDirectory& temporary) {
    const auto not_zip = temporary.file("not-a-zip.docx");
    writeBytes(not_zip, {'N', 'O', 'T', 'Z', 'I', 'P'});
    auto response = parseFile(not_zip);
    check(response.status == ParserStatus::invalid_package,
          "non-ZIP input was not rejected as an invalid package");

    const auto malformed_xml = temporary.file("malformed-xml.docx");
    createDocxWithDocument(
        malformed_xml,
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\"><w:body><w:p></w:body></w:document>");
    response = parseFile(malformed_xml);
    check(response.status == ParserStatus::malformed_document_xml &&
              response.error.find("Malformed word/document.xml") !=
                  std::string::npos,
          "malformed XML was not rejected with a bounded parse diagnostic");

    const std::array<std::string_view, 5> malformed_documents{
        "",
        "<document><body/></document>",
        "<w:document xmlns:w=\"urn:not-word\"><w:body/></w:document>",
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
        "wordprocessingml/2006/main\"/>",
        std::string_view("<w:document\0/>", 14)};
    for (std::size_t index = 0; index < malformed_documents.size(); ++index) {
        const auto path = temporary.file(
            "malformed-xml-corpus-" + std::to_string(index) + ".docx");
        createDocxWithDocument(path, malformed_documents[index]);
        response = parseFile(path);
        check(!response.ok(),
              "malformed XML corpus member was unexpectedly accepted");
        check(response.error.size() <= 1024,
              "malformed XML diagnostic was not bounded");
    }
}

void testDeterministicZipFuzzSmoke(const TemporaryDirectory& temporary) {
    const auto seed_path = temporary.file("zip-fuzz-seed.docx");
    createDocx(seed_path);
    const auto seed = readBytes(seed_path);
    check(seed.size() > 64, "ZIP fuzz seed was unexpectedly small");

    const std::array<std::size_t, 6> truncation_sizes{
        0, 1, 4, 22, seed.size() / 2, seed.size() - 1};
    for (std::size_t index = 0; index < truncation_sizes.size(); ++index) {
        const auto path = temporary.file(
            "zip-truncation-" + std::to_string(index) + ".docx");
        writeBytes(path, std::vector<std::uint8_t>(
            seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(
                                     truncation_sizes[index])));
        const auto response = parseFile(path);
        check(!response.ok(),
              "truncated ZIP fuzz corpus member was unexpectedly accepted");
        check(response.error.size() <= 1024,
              "truncated ZIP diagnostic was not bounded");
    }

    // Fixed-seed single-byte mutations exercise local headers, compressed
    // data, the central directory, and the end record without introducing a
    // flaky random dependency.  A mutation may remain a valid ZIP when it
    // lands in an opaque member; the smoke contract is bounded completion and
    // a valid status, not mandatory rejection of harmless byte changes.
    std::uint32_t state = 0x4f574c31U;
    for (std::size_t index = 0; index < 64; ++index) {
        state = state * 1664525U + 1013904223U;
        auto mutated = seed;
        const std::size_t offset =
            static_cast<std::size_t>(state) % mutated.size();
        mutated[offset] ^= static_cast<std::uint8_t>(
            1U << static_cast<unsigned>((state >> 24U) & 7U));
        const auto path = temporary.file(
            "zip-mutation-" + std::to_string(index) + ".docx");
        writeBytes(path, mutated);
        const auto response = parseFile(path);
        const auto status = static_cast<std::uint16_t>(response.status);
        check(status <= static_cast<std::uint16_t>(ParserStatus::internal_error),
              "ZIP fuzz corpus produced an invalid parser status");
        check(response.error.size() <= 1024,
              "ZIP fuzz corpus produced an unbounded diagnostic");
        check(response.paragraphs.size() <= 2,
              "ZIP fuzz corpus produced unbounded paragraph output");
    }
}

ParserClientOptions clientOptions(
    const std::filesystem::path& helper,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    ParserClientOptions options;
    options.helper_executable = helper;
    options.timeout = timeout;
    return options;
}

void testSpawnedParserClient(const TemporaryDirectory& temporary) {
    const std::filesystem::path helper = parserWorkerPath();
    check(std::filesystem::exists(helper), "built parser helper is missing");
    check(helper.is_absolute(), "parser helper test path must be absolute");

    const std::filesystem::path path = temporary.file("spawned-worker.docx");
    createDocx(path);
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper));
    check(result.transportOk(), result.error);
    check(result.response.ok(), result.response.error);
    check(result.worker_exit.available, "worker exit status was not collected");
    check(
        result.worker_exit.exited && result.worker_exit.exit_code == 0,
        "successful parser helper did not exit cleanly");
    check(result.response.paragraphs.size() == 2, "spawned parser paragraph count differs");
    check(
        result.response.paragraphs[0].plainText() == "Hello & worker\ttail",
        "spawned parser text differs");
}

void testDescriptorClientDoesNotNeedDocumentPath(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("unlinked-worker.docx");
    createDocx(path);
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    check(descriptor >= 0, "could not open descriptor-only fixture");
    check(::lseek(descriptor, 7, SEEK_SET) == 7, "could not set descriptor offset");
    check(std::filesystem::remove(path), "could not unlink descriptor-only fixture");

    const auto result = docxstudio::worker::parseDocxDescriptorWithWorker(
        descriptor, clientOptions(parserWorkerPath()));
    check(result.transportOk(), result.error);
    check(result.response.ok(), result.response.error);
    check(::fcntl(descriptor, F_GETFD) >= 0, "client consumed the caller-owned descriptor");
    check(::lseek(descriptor, 0, SEEK_CUR) == 7, "client changed the caller's file offset");
    ::close(descriptor);
}

void testClientTimeoutAndReap(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("timeout.docx");
    createDocx(path);
    const std::filesystem::path helper = installFakeHelper(temporary, "fake-timeout-worker");
    const auto started = std::chrono::steady_clock::now();
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper, std::chrono::milliseconds(100)));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(result.status == ParserClientStatus::timed_out, "slow helper was not timed out");
    check(result.worker_exit.available, "timed-out helper was not reaped");
    check(
        result.worker_exit.signaled && result.worker_exit.signal_number == SIGKILL,
        "timed-out helper was not killed as a contained process group");
    check(elapsed < std::chrono::seconds(3), "worker timeout was not enforced promptly");
}

void testClientProtocolFailure(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("protocol.docx");
    createDocx(path);
    const std::filesystem::path helper = installFakeHelper(temporary, "fake-protocol-worker");
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper));
    check(
        result.status == ParserClientStatus::protocol_error,
        "malformed helper response was not classified as a protocol error");
    check(result.worker_exit.available, "malformed-response helper was not reaped");
    check(!result.response_received, "malformed response was exposed as decoded data");
}

void testClientExitFailure(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("exit.docx");
    createDocx(path);
    const std::filesystem::path helper = installFakeHelper(temporary, "fake-exit-worker");
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper));
    check(
        result.status == ParserClientStatus::worker_exited,
        "nonzero helper exit was not classified separately");
    check(
        result.worker_exit.available && result.worker_exit.exited &&
            result.worker_exit.exit_code == 23,
        "nonzero helper exit status was not preserved");
}

void testClientSignalFailure(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("signal.docx");
    createDocx(path);
    const std::filesystem::path helper = installFakeHelper(temporary, "fake-signal-worker");
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper));
    check(
        result.status == ParserClientStatus::worker_signaled,
        "signaled helper was not classified separately");
    check(
        result.worker_exit.available && result.worker_exit.signaled &&
            result.worker_exit.signal_number == SIGTERM,
        "helper termination signal was not preserved");
}

void testClientWaitsForExitAfterResponseEof(
    const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("delayed-signal.docx");
    createDocx(path);
    const std::filesystem::path helper =
        installFakeHelper(temporary, "fake-delayed-signal-worker");
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(helper));
    check(
        result.status == ParserClientStatus::worker_signaled,
        "response EOF raced the helper's termination classification");
    check(
        result.worker_exit.available && result.worker_exit.signaled &&
            result.worker_exit.signal_number == SIGTERM,
        "response EOF replaced the helper's real termination signal");
}

void testClientSpawnFailure(const TemporaryDirectory& temporary) {
    const std::filesystem::path path = temporary.file("missing-helper.docx");
    createDocx(path);
    const auto result = docxstudio::worker::parseDocxFileWithWorker(
        path, clientOptions(temporary.file("does-not-exist")));
    check(
        result.status == ParserClientStatus::spawn_failed,
        "missing helper was not classified as a spawn failure");
    check(result.native_error == ENOENT, "spawn error did not retain ENOENT");
}

}  // namespace

int main(int argc, char** argv) {
    const int fake_helper_status = runFakeHelperIfRequested(argc, argv);
    if (fake_helper_status >= 0) {
        return fake_helper_status;
    }
    try {
        TemporaryDirectory temporary;
        testProtocolRoundTrip();
        testSandboxDenialsInForkedChild();
        testSandboxedParserWorker(temporary);
        testWorkerXmlComplexityLimits(temporary);
        testWorkerPackageAndMediaLimits(temporary);
        testMalformedPackageAndXml(temporary);
        testDeterministicZipFuzzSmoke(temporary);
        testSpawnedParserClient(temporary);
        testDescriptorClientDoesNotNeedDocumentPath(temporary);
        testClientTimeoutAndReap(temporary);
        testClientProtocolFailure(temporary);
        testClientExitFailure(temporary);
        testClientSignalFailure(temporary);
        testClientWaitsForExitAfterResponseEof(temporary);
        testClientSpawnFailure(temporary);
        std::cout << "Worker tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Worker test failure: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
