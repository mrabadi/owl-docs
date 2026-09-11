#pragma once

#include "docxstudio/worker/sandbox.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace docxstudio::worker {

inline constexpr std::uint16_t kParserProtocolVersion = 1;
inline constexpr std::size_t kParserResponseHeaderSize = 20;

enum class ParserStatus : std::uint16_t {
    ok = 0,
    invalid_request = 1,
    sandbox_failed = 2,
    invalid_package = 3,
    package_limit_exceeded = 4,
    malformed_document_xml = 5,
    protocol_error = 6,
    internal_error = 7,
};

enum class ParserCompatibility : std::uint16_t {
    invalid = 0,
    basic_body = 1,
    complex_body_preserved = 2,
};

struct ParsedParagraph {
    std::vector<std::string> runs;

    [[nodiscard]] std::string plainText() const;
    auto operator<=>(const ParsedParagraph&) const = default;
};

struct ParserResponse {
    ParserStatus status{ParserStatus::internal_error};
    ParserCompatibility compatibility{ParserCompatibility::invalid};
    std::vector<ParsedParagraph> paragraphs;
    std::vector<std::string> warnings;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return status == ParserStatus::ok; }
};

struct ParserLimits {
    std::uint64_t max_package_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t max_member_count{10000};
    std::uint64_t max_member_uncompressed_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t max_total_uncompressed_bytes{1024ULL * 1024ULL * 1024ULL};
    // Media is never decoded in the parser worker, but it is still part of
    // the untrusted ZIP expansion budget.  Keep tighter per-file and
    // aggregate limits so many otherwise-valid image members cannot consume
    // the much larger generic package allowance.
    std::uint64_t max_media_member_uncompressed_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t max_total_media_uncompressed_bytes{256ULL * 1024ULL * 1024ULL};
    std::uint64_t max_document_xml_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t max_xml_depth{256};
    std::uint64_t max_xml_nodes{1'000'000};
    std::uint64_t max_paragraphs{1'000'000};
    std::uint64_t max_runs{4'000'000};
    std::uint64_t max_response_bytes{128ULL * 1024ULL * 1024ULL};
};

struct ParserWorkerOptions {
    SandboxLimits sandbox;
    ParserLimits parser;
    // A dedicated worker should close everything except its document and
    // response descriptors before parsing. Disable only in tightly controlled
    // tests or an embedding that has already closed inherited descriptors.
    bool close_inherited_descriptors{true};
    bool clear_environment{true};
};

// Binary protocol helpers. Integers are little-endian and the frame begins
// with "DXW1", followed by a versioned fixed header and a bounded payload.
// readParserResponse rejects unknown versions, invalid enums, oversized
// counts/strings and trailing bytes.
[[nodiscard]] bool writeParserResponse(
    int output_fd,
    const ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error = nullptr);

[[nodiscard]] bool readParserResponse(
    int input_fd,
    ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error = nullptr);

// Decodes one complete response frame already collected by a deadline-aware
// transport. The span must contain exactly one frame; truncated data and bytes
// following the declared payload are rejected.
[[nodiscard]] bool decodeParserResponseFrame(
    std::span<const std::uint8_t> frame,
    ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error = nullptr);

// Consumes owned_read_only_fd. It performs no path-based access and parses
// word/document.xml directly with libzip and pugixml. Normally use
// runParserWorker(), which installs the sandbox first.
[[nodiscard]] ParserResponse parseOwnedDocxDescriptor(
    int owned_read_only_fd,
    const ParserLimits& limits = {});

// Entry point for a freshly forked/execed helper. The input must be an already
// open O_RDONLY regular-file descriptor, and output_fd must be a writable pipe
// or AF_UNIX socket opened by the parent. The function duplicates the input,
// optionally closes other inherited descriptors/clears the environment,
// installs the irreversible sandbox, parses, and writes one response frame.
// Returns 0 when a response was written; the response status carries parse
// failure details.
[[nodiscard]] int runParserWorker(
    int input_fd,
    int output_fd,
    const ParserWorkerOptions& options = {});

}  // namespace docxstudio::worker
