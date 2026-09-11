#include "docxstudio/worker/protocol.h"
#include "docxstudio/xml/complexity.h"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace docxstudio::worker {
namespace {

constexpr std::array<std::uint8_t, 4> kProtocolMagic{'D', 'X', 'W', '1'};
constexpr std::string_view kDocumentPart = "word/document.xml";
constexpr std::string_view kContentTypesPart = "[Content_Types].xml";
constexpr std::string_view kRelationshipsPart = "_rels/.rels";
constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kStrictWordNamespace =
    "http://purl.oclc.org/ooxml/wordprocessingml/main";

bool isWordMediaMember(std::string_view name) {
    return name.starts_with("word/media/") &&
           name.size() > std::string_view("word/media/").size() &&
           !name.ends_with('/');
}

void setStringError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

bool appendString(
    std::vector<std::uint8_t>& output,
    std::string_view value,
    std::uint64_t maximum_size) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
        output.size() > maximum_size || maximum_size - output.size() < 4 ||
        value.size() > maximum_size - output.size() - 4) {
        return false;
    }
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
    return output.size() <= maximum_size;
}

bool appendCount(
    std::vector<std::uint8_t>& output,
    std::uint32_t value,
    std::uint64_t maximum_size) {
    if (output.size() > maximum_size || maximum_size - output.size() < 4) {
        return false;
    }
    appendU32(output, value);
    return true;
}

bool writeAll(int descriptor, const std::uint8_t* data, std::size_t size, std::string* error) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(
            descriptor,
            data + written,
            std::min<std::size_t>(size - written, static_cast<std::size_t>(SSIZE_MAX)));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int native_error = errno;
            setStringError(
                error,
                std::string("Cannot write parser response: ") + std::strerror(native_error));
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool readAll(int descriptor, std::uint8_t* data, std::size_t size, std::string* error) {
    std::size_t consumed = 0;
    while (consumed < size) {
        const ssize_t count = ::read(
            descriptor,
            data + consumed,
            std::min<std::size_t>(size - consumed, static_cast<std::size_t>(SSIZE_MAX)));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const int native_error = errno;
            setStringError(
                error,
                count == 0 ? "Parser response ended before the complete frame"
                           : std::string("Cannot read parser response: ") +
                                 std::strerror(native_error));
            return false;
        }
        consumed += static_cast<std::size_t>(count);
    }
    return true;
}

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::uint8_t> payload) : payload_(payload) {}

    bool readU32(std::uint32_t& value) {
        if (remaining() < 4) {
            return false;
        }
        value = static_cast<std::uint32_t>(payload_[position_]) |
                (static_cast<std::uint32_t>(payload_[position_ + 1]) << 8U) |
                (static_cast<std::uint32_t>(payload_[position_ + 2]) << 16U) |
                (static_cast<std::uint32_t>(payload_[position_ + 3]) << 24U);
        position_ += 4;
        return true;
    }

    bool readString(std::string& value) {
        std::uint32_t length = 0;
        if (!readU32(length) || length > remaining()) {
            return false;
        }
        value.assign(
            reinterpret_cast<const char*>(payload_.data() + position_),
            static_cast<std::size_t>(length));
        position_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return payload_.size() - position_; }

private:
    std::span<const std::uint8_t> payload_;
    std::size_t position_{0};
};

std::uint16_t readU16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint64_t readU64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(bytes[shift / 8]) << shift;
    }
    return value;
}

std::string zipArchiveError(zip_t* archive) {
    return archive == nullptr ? "unknown libzip error"
                              : std::string(zip_error_strerror(zip_get_error(archive)));
}

bool readZipEntry(
    zip_t* archive,
    zip_uint64_t index,
    std::uint64_t maximum_size,
    std::string& contents,
    ParserResponse& response) {
    zip_stat_t status;
    zip_stat_init(&status);
    if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &status) != 0 ||
        (status.valid & ZIP_STAT_SIZE) == 0) {
        response.status = ParserStatus::invalid_package;
        response.error = "Cannot inspect document.xml: " + zipArchiveError(archive);
        return false;
    }
    if (status.size > maximum_size || status.size > std::numeric_limits<std::size_t>::max()) {
        response.status = ParserStatus::package_limit_exceeded;
        response.error = "word/document.xml exceeds the configured worker limit";
        return false;
    }
    zip_file_t* file = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
    if (file == nullptr) {
        response.status = ParserStatus::invalid_package;
        response.error = "Cannot open document.xml: " + zipArchiveError(archive);
        return false;
    }
    contents.resize(static_cast<std::size_t>(status.size));
    std::size_t consumed = 0;
    while (consumed < contents.size()) {
        const zip_int64_t count = zip_fread(
            file,
            contents.data() + consumed,
            static_cast<zip_uint64_t>(contents.size() - consumed));
        if (count <= 0) {
            response.status = ParserStatus::invalid_package;
            response.error = count < 0 ? zip_file_strerror(file)
                                       : "document.xml ended before its advertised size";
            zip_fclose(file);
            return false;
        }
        consumed += static_cast<std::size_t>(count);
    }
    if (zip_fclose(file) != 0) {
        response.status = ParserStatus::invalid_package;
        response.error = "CRC validation failed while closing document.xml";
        return false;
    }
    return true;
}

std::string_view localName(std::string_view qualified_name) {
    const std::size_t colon = qualified_name.find(':');
    return colon == std::string_view::npos ? qualified_name : qualified_name.substr(colon + 1);
}

std::string_view prefixName(std::string_view qualified_name) {
    const std::size_t colon = qualified_name.find(':');
    return colon == std::string_view::npos ? std::string_view{} : qualified_name.substr(0, colon);
}

std::string namespaceUri(const pugi::xml_node& node) {
    const std::string_view prefix = prefixName(node.name());
    const std::string attribute_name = prefix.empty() ? "xmlns" : "xmlns:" + std::string(prefix);
    for (pugi::xml_node current = node; current; current = current.parent()) {
        if (const pugi::xml_attribute attribute = current.attribute(attribute_name.c_str())) {
            return attribute.value();
        }
    }
    return {};
}

bool isWordElement(const pugi::xml_node& node, std::string_view expected_local_name) {
    if (node.type() != pugi::node_element || localName(node.name()) != expected_local_name) {
        return false;
    }
    const std::string uri = namespaceUri(node);
    return uri == kWordNamespace || uri == kStrictWordNamespace;
}

bool whitespaceOnly(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    });
}

bool ignorableNode(const pugi::xml_node& node) {
    return node.type() == pugi::node_comment || node.type() == pugi::node_pi ||
           (node.type() == pugi::node_pcdata && whitespaceOnly(node.value()));
}

bool basicRun(const pugi::xml_node& run) {
    for (pugi::xml_node child : run.children()) {
        if (ignorableNode(child)) {
            continue;
        }
        if (child.type() != pugi::node_element ||
            !(isWordElement(child, "rPr") || isWordElement(child, "t") ||
              isWordElement(child, "tab") || isWordElement(child, "br") ||
              isWordElement(child, "cr"))) {
            return false;
        }
    }
    return true;
}

bool basicParagraph(const pugi::xml_node& paragraph) {
    for (pugi::xml_node child : paragraph.children()) {
        if (ignorableNode(child)) {
            continue;
        }
        if (child.type() != pugi::node_element ||
            !(isWordElement(child, "pPr") || isWordElement(child, "r"))) {
            return false;
        }
        if (isWordElement(child, "r") && !basicRun(child)) {
            return false;
        }
    }
    return true;
}

void collectParagraphs(const pugi::xml_node& node, std::vector<pugi::xml_node>& paragraphs) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (isWordElement(child, "p")) {
            paragraphs.push_back(child);
        } else {
            collectParagraphs(child, paragraphs);
        }
    }
}

void collectRuns(
    const pugi::xml_node& node,
    const pugi::xml_node& paragraph,
    std::vector<pugi::xml_node>& runs) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (isWordElement(child, "p") && child != paragraph) {
            continue;
        }
        if (isWordElement(child, "r")) {
            runs.push_back(child);
        } else {
            collectRuns(child, paragraph, runs);
        }
    }
}

void appendRunText(const pugi::xml_node& node, std::string& text) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (isWordElement(child, "t")) {
            text += child.text().get();
        } else if (isWordElement(child, "tab")) {
            text += '\t';
        } else if (isWordElement(child, "br") || isWordElement(child, "cr")) {
            text += '\n';
        } else if (!isWordElement(child, "p") && !isWordElement(child, "r")) {
            appendRunText(child, text);
        }
    }
}

ParserResponse parseDocumentXml(std::string_view xml, const ParserLimits& limits) {
    ParserResponse response;
    pugi::xml_document document;
    const pugi::xml_parse_result parse_result = document.load_buffer(
        xml.data(),
        xml.size(),
        pugi::parse_default | pugi::parse_ws_pcdata,
        pugi::encoding_auto);
    if (!parse_result) {
        response.status = ParserStatus::malformed_document_xml;
        std::ostringstream message;
        message << "Malformed word/document.xml at byte " << parse_result.offset << ": "
                << parse_result.description();
        response.error = message.str();
        return response;
    }
    const auto complexity = ::docxstudio::xml::inspectComplexity(
        document, limits.max_xml_depth, limits.max_xml_nodes);
    if (!complexity.accepted()) {
        response.status = ParserStatus::package_limit_exceeded;
        std::ostringstream message;
        message << "word/document.xml exceeds the configured XML ";
        if (complexity.status ==
            ::docxstudio::xml::ComplexityStatus::depth_exceeded) {
            message << "depth limit of " << limits.max_xml_depth;
        } else {
            message << "node-count limit of " << limits.max_xml_nodes;
        }
        response.error = message.str();
        return response;
    }
    const pugi::xml_node root = document.document_element();
    if (!isWordElement(root, "document")) {
        response.status = ParserStatus::malformed_document_xml;
        response.error = "word/document.xml has no WordprocessingML document root";
        return response;
    }
    pugi::xml_node body;
    for (pugi::xml_node child : root.children()) {
        if (isWordElement(child, "body")) {
            body = child;
            break;
        }
    }
    if (!body) {
        response.status = ParserStatus::malformed_document_xml;
        response.error = "word/document.xml has no WordprocessingML body";
        return response;
    }

    bool basic_body = true;
    for (pugi::xml_node child : body.children()) {
        if (ignorableNode(child) || isWordElement(child, "sectPr")) {
            continue;
        }
        if (!isWordElement(child, "p") || !basicParagraph(child)) {
            basic_body = false;
        }
    }

    std::vector<pugi::xml_node> paragraph_nodes;
    collectParagraphs(body, paragraph_nodes);
    if (paragraph_nodes.size() > limits.max_paragraphs) {
        response.status = ParserStatus::package_limit_exceeded;
        response.error = "Document has more paragraphs than the configured worker limit";
        return response;
    }
    response.paragraphs.reserve(paragraph_nodes.size());
    std::uint64_t total_runs = 0;
    std::uint64_t output_text_bytes = 0;
    for (pugi::xml_node paragraph_node : paragraph_nodes) {
        std::vector<pugi::xml_node> run_nodes;
        collectRuns(paragraph_node, paragraph_node, run_nodes);
        if (run_nodes.size() > limits.max_runs - std::min(total_runs, limits.max_runs)) {
            response.status = ParserStatus::package_limit_exceeded;
            response.error = "Document has more runs than the configured worker limit";
            response.paragraphs.clear();
            return response;
        }
        total_runs += run_nodes.size();
        ParsedParagraph paragraph;
        paragraph.runs.reserve(run_nodes.size());
        for (pugi::xml_node run_node : run_nodes) {
            std::string run;
            appendRunText(run_node, run);
            if (run.size() > limits.max_response_bytes -
                                 std::min(output_text_bytes, limits.max_response_bytes)) {
                response.status = ParserStatus::package_limit_exceeded;
                response.error = "Decoded text exceeds the configured response limit";
                response.paragraphs.clear();
                return response;
            }
            output_text_bytes += run.size();
            paragraph.runs.push_back(std::move(run));
        }
        response.paragraphs.push_back(std::move(paragraph));
    }
    response.status = ParserStatus::ok;
    response.compatibility = basic_body ? ParserCompatibility::basic_body
                                        : ParserCompatibility::complex_body_preserved;
    if (!basic_body) {
        response.warnings.emplace_back(
            "The body contains non-basic markup; the main process must preserve it as opaque OOXML");
    }
    return response;
}

int descriptorStat(int descriptor, struct stat* status) {
#if defined(SYS_fstat)
    return static_cast<int>(::syscall(SYS_fstat, descriptor, status));
#else
    return ::fstat(descriptor, status);
#endif
}

bool descriptorIsReadOnlyRegularFile(int descriptor, std::string& error) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        error = std::string("Cannot inspect parser input descriptor: ") + std::strerror(errno);
        return false;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        error = "Parser input descriptor must have O_RDONLY access";
        return false;
    }
    struct stat status {};
    if (descriptorStat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        error = "Parser input descriptor must refer to a regular file or memfd";
        return false;
    }
    return true;
}

bool descriptorIsSafeResponseChannel(int descriptor, std::string& error) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY) {
        error = "Parser response descriptor must be writable";
        return false;
    }
    struct stat status {};
    if (descriptorStat(descriptor, &status) != 0) {
        error = "Cannot inspect parser response descriptor";
        return false;
    }
    if (S_ISFIFO(status.st_mode)) {
        return true;
    }
    if (!S_ISSOCK(status.st_mode)) {
        error = "Parser response descriptor must be a pipe or AF_UNIX socket";
        return false;
    }
    struct sockaddr_storage address {};
    socklen_t address_length = sizeof(address);
    if (::getsockname(
            descriptor,
            reinterpret_cast<struct sockaddr*>(&address),
            &address_length) != 0 ||
        address.ss_family != AF_UNIX) {
        error = "Parser response sockets must use AF_UNIX";
        return false;
    }
    return true;
}

bool closeRange(unsigned first, unsigned last) {
    if (first > last) {
        return true;
    }
#if defined(SYS_close_range)
    if (::syscall(SYS_close_range, first, last, 0U) == 0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return false;
    }
#endif
    struct rlimit descriptor_limit {};
    if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
        return false;
    }
    const std::uint64_t fallback_end = descriptor_limit.rlim_cur == RLIM_INFINITY
                                           ? 1'048'576ULL
                                           : descriptor_limit.rlim_cur;
    const std::uint64_t bounded_last = std::min<std::uint64_t>(last, fallback_end);
    for (std::uint64_t descriptor = first; descriptor <= bounded_last; ++descriptor) {
        (void)::close(static_cast<int>(descriptor));
    }
    return true;
}

bool closeDescriptorsExcept(int first_keep, int second_keep) {
    std::array<unsigned, 2> keep{
        static_cast<unsigned>(first_keep), static_cast<unsigned>(second_keep)};
    std::sort(keep.begin(), keep.end());
    unsigned next = 0;
    for (const unsigned descriptor : keep) {
        if (next < descriptor && !closeRange(next, descriptor - 1)) {
            return false;
        }
        if (descriptor != UINT_MAX) {
            next = descriptor + 1;
        }
    }
    return next == 0 || closeRange(next, UINT_MAX);
}

}  // namespace

std::string ParsedParagraph::plainText() const {
    std::string text;
    for (const std::string& run : runs) {
        text += run;
    }
    return text;
}

bool writeParserResponse(
    int output_fd,
    const ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error) {
    if (output_fd < 0 || maximum_frame_bytes < kParserResponseHeaderSize ||
        maximum_frame_bytes > std::numeric_limits<std::size_t>::max()) {
        setStringError(error, "Invalid parser response descriptor or frame limit");
        return false;
    }
    const std::uint64_t maximum_payload = maximum_frame_bytes - kParserResponseHeaderSize;
    std::vector<std::uint8_t> payload;
    if (!appendString(payload, response.error, maximum_payload) ||
        response.warnings.size() > std::numeric_limits<std::uint32_t>::max()) {
        setStringError(error, "Parser response exceeds the frame limit");
        return false;
    }
    if (!appendCount(
            payload, static_cast<std::uint32_t>(response.warnings.size()), maximum_payload)) {
        setStringError(error, "Parser response exceeds the frame limit");
        return false;
    }
    for (const std::string& warning : response.warnings) {
        if (!appendString(payload, warning, maximum_payload)) {
            setStringError(error, "Parser response exceeds the frame limit");
            return false;
        }
    }
    if (response.paragraphs.size() > std::numeric_limits<std::uint32_t>::max()) {
        setStringError(error, "Parser response has too many paragraphs");
        return false;
    }
    if (!appendCount(
            payload, static_cast<std::uint32_t>(response.paragraphs.size()), maximum_payload)) {
        setStringError(error, "Parser response exceeds the frame limit");
        return false;
    }
    for (const ParsedParagraph& paragraph : response.paragraphs) {
        if (paragraph.runs.size() > std::numeric_limits<std::uint32_t>::max()) {
            setStringError(error, "Parser response has too many runs");
            return false;
        }
        if (!appendCount(
                payload, static_cast<std::uint32_t>(paragraph.runs.size()), maximum_payload)) {
            setStringError(error, "Parser response exceeds the frame limit");
            return false;
        }
        for (const std::string& run : paragraph.runs) {
            if (!appendString(payload, run, maximum_payload)) {
                setStringError(error, "Parser response exceeds the frame limit");
                return false;
            }
        }
    }
    if (payload.size() > maximum_payload) {
        setStringError(error, "Parser response exceeds the frame limit");
        return false;
    }

    std::vector<std::uint8_t> header;
    header.reserve(kParserResponseHeaderSize);
    header.insert(header.end(), kProtocolMagic.begin(), kProtocolMagic.end());
    appendU16(header, kParserProtocolVersion);
    appendU16(header, static_cast<std::uint16_t>(response.status));
    appendU16(header, static_cast<std::uint16_t>(response.compatibility));
    appendU16(header, 0);
    appendU64(header, payload.size());
    return writeAll(output_fd, header.data(), header.size(), error) &&
           writeAll(output_fd, payload.data(), payload.size(), error);
}

bool readParserResponse(
    int input_fd,
    ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error) {
    response = {};
    if (input_fd < 0 || maximum_frame_bytes < kParserResponseHeaderSize ||
        maximum_frame_bytes > std::numeric_limits<std::size_t>::max()) {
        setStringError(error, "Invalid parser response descriptor or frame limit");
        return false;
    }
    std::array<std::uint8_t, kParserResponseHeaderSize> header{};
    if (!readAll(input_fd, header.data(), header.size(), error)) {
        return false;
    }
    if (!std::equal(kProtocolMagic.begin(), kProtocolMagic.end(), header.begin()) ||
        readU16(header.data() + 4) != kParserProtocolVersion || readU16(header.data() + 10) != 0) {
        setStringError(error, "Parser response has an invalid magic, version, or reserved field");
        return false;
    }
    const std::uint16_t status_value = readU16(header.data() + 6);
    const std::uint16_t compatibility_value = readU16(header.data() + 8);
    if (status_value > static_cast<std::uint16_t>(ParserStatus::internal_error) ||
        compatibility_value >
            static_cast<std::uint16_t>(ParserCompatibility::complex_body_preserved)) {
        setStringError(error, "Parser response contains an unknown status value");
        return false;
    }
    const std::uint64_t payload_size = readU64(header.data() + 12);
    if (payload_size > maximum_frame_bytes - kParserResponseHeaderSize ||
        payload_size > std::numeric_limits<std::size_t>::max()) {
        setStringError(error, "Parser response payload exceeds the frame limit");
        return false;
    }
    std::vector<std::uint8_t> frame(kParserResponseHeaderSize + static_cast<std::size_t>(payload_size));
    std::copy(header.begin(), header.end(), frame.begin());
    if (!readAll(
            input_fd,
            frame.data() + kParserResponseHeaderSize,
            static_cast<std::size_t>(payload_size),
            error)) {
        return false;
    }

    return decodeParserResponseFrame(frame, response, maximum_frame_bytes, error);
}

bool decodeParserResponseFrame(
    std::span<const std::uint8_t> frame,
    ParserResponse& response,
    std::uint64_t maximum_frame_bytes,
    std::string* error) {
    response = {};
    if (maximum_frame_bytes < kParserResponseHeaderSize ||
        maximum_frame_bytes > std::numeric_limits<std::size_t>::max() ||
        frame.size() < kParserResponseHeaderSize || frame.size() > maximum_frame_bytes) {
        setStringError(error, "Parser response is truncated or exceeds the frame limit");
        return false;
    }
    const auto* header = frame.data();
    if (!std::equal(kProtocolMagic.begin(), kProtocolMagic.end(), header) ||
        readU16(header + 4) != kParserProtocolVersion || readU16(header + 10) != 0) {
        setStringError(error, "Parser response has an invalid magic, version, or reserved field");
        return false;
    }
    const std::uint16_t status_value = readU16(header + 6);
    const std::uint16_t compatibility_value = readU16(header + 8);
    if (status_value > static_cast<std::uint16_t>(ParserStatus::internal_error) ||
        compatibility_value >
            static_cast<std::uint16_t>(ParserCompatibility::complex_body_preserved)) {
        setStringError(error, "Parser response contains an unknown status value");
        return false;
    }
    const std::uint64_t payload_size = readU64(header + 12);
    if (payload_size > maximum_frame_bytes - kParserResponseHeaderSize ||
        payload_size != frame.size() - kParserResponseHeaderSize) {
        setStringError(error, "Parser response payload length does not match the frame");
        return false;
    }

    response.status = static_cast<ParserStatus>(status_value);
    response.compatibility = static_cast<ParserCompatibility>(compatibility_value);
    PayloadReader reader(frame.subspan(kParserResponseHeaderSize));
    if (!reader.readString(response.error)) {
        setStringError(error, "Parser response has a truncated error field");
        return false;
    }
    std::uint32_t warning_count = 0;
    if (!reader.readU32(warning_count) || warning_count > reader.remaining() / 4) {
        setStringError(error, "Parser response has an invalid warning count");
        return false;
    }
    response.warnings.reserve(warning_count);
    for (std::uint32_t index = 0; index < warning_count; ++index) {
        std::string warning;
        if (!reader.readString(warning)) {
            setStringError(error, "Parser response has a truncated warning");
            return false;
        }
        response.warnings.push_back(std::move(warning));
    }
    std::uint32_t paragraph_count = 0;
    if (!reader.readU32(paragraph_count) || paragraph_count > reader.remaining() / 4) {
        setStringError(error, "Parser response has an invalid paragraph count");
        return false;
    }
    response.paragraphs.reserve(paragraph_count);
    for (std::uint32_t paragraph_index = 0; paragraph_index < paragraph_count; ++paragraph_index) {
        std::uint32_t run_count = 0;
        if (!reader.readU32(run_count) || run_count > reader.remaining() / 4) {
            setStringError(error, "Parser response has an invalid run count");
            return false;
        }
        ParsedParagraph paragraph;
        paragraph.runs.reserve(run_count);
        for (std::uint32_t run_index = 0; run_index < run_count; ++run_index) {
            std::string run;
            if (!reader.readString(run)) {
                setStringError(error, "Parser response has a truncated run");
                return false;
            }
            paragraph.runs.push_back(std::move(run));
        }
        response.paragraphs.push_back(std::move(paragraph));
    }
    if (reader.remaining() != 0) {
        setStringError(error, "Parser response contains trailing payload bytes");
        return false;
    }
    return true;
}

ParserResponse parseOwnedDocxDescriptor(int owned_read_only_fd, const ParserLimits& limits) {
    ParserResponse response;
    if (limits.max_package_bytes == 0 || limits.max_member_count == 0 ||
        limits.max_member_uncompressed_bytes == 0 || limits.max_total_uncompressed_bytes == 0 ||
        limits.max_media_member_uncompressed_bytes == 0 ||
        limits.max_total_media_uncompressed_bytes == 0 ||
        limits.max_document_xml_bytes == 0 || limits.max_xml_depth == 0 ||
        limits.max_xml_nodes == 0 || limits.max_paragraphs == 0 || limits.max_runs == 0 ||
        limits.max_response_bytes < kParserResponseHeaderSize) {
        if (owned_read_only_fd >= 0) {
            ::close(owned_read_only_fd);
        }
        response.status = ParserStatus::invalid_request;
        response.error = "Parser limits must be non-zero and internally consistent";
        return response;
    }
    if (owned_read_only_fd < 0) {
        response.status = ParserStatus::invalid_request;
        response.error = "Parser input descriptor is invalid";
        return response;
    }
    std::string descriptor_error;
    if (!descriptorIsReadOnlyRegularFile(owned_read_only_fd, descriptor_error)) {
        ::close(owned_read_only_fd);
        response.status = ParserStatus::invalid_request;
        response.error = std::move(descriptor_error);
        return response;
    }
    struct stat descriptor_status {};
    if (descriptorStat(owned_read_only_fd, &descriptor_status) != 0 || descriptor_status.st_size < 0 ||
        static_cast<std::uint64_t>(descriptor_status.st_size) > limits.max_package_bytes) {
        ::close(owned_read_only_fd);
        response.status = ParserStatus::package_limit_exceeded;
        response.error = "DOCX package exceeds the configured worker byte limit";
        return response;
    }

    const auto package_size = static_cast<std::size_t>(descriptor_status.st_size);
    std::vector<std::uint8_t> package_bytes(package_size);
    std::size_t consumed = 0;
    while (consumed < package_bytes.size()) {
        const ssize_t count = ::pread(
            owned_read_only_fd,
            package_bytes.data() + consumed,
            std::min<std::size_t>(
                package_bytes.size() - consumed, static_cast<std::size_t>(SSIZE_MAX)),
            static_cast<off_t>(consumed));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(owned_read_only_fd);
            response.status = ParserStatus::invalid_package;
            response.error = count == 0 ? "DOCX descriptor ended before its advertised size"
                                        : std::string("Cannot read DOCX descriptor: ") +
                                              std::strerror(errno);
            return response;
        }
        consumed += static_cast<std::size_t>(count);
    }
    ::close(owned_read_only_fd);

    zip_error_t source_error;
    zip_error_init(&source_error);
    zip_source_t* source = zip_source_buffer_create(
        package_bytes.empty() ? nullptr : package_bytes.data(),
        static_cast<zip_uint64_t>(package_bytes.size()),
        0,
        &source_error);
    if (source == nullptr) {
        response.status = ParserStatus::internal_error;
        response.error = "Cannot create bounded in-memory ZIP source: " +
                         std::string(zip_error_strerror(&source_error));
        zip_error_fini(&source_error);
        return response;
    }
    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &source_error);
    if (archive == nullptr) {
        const std::string message = zip_error_strerror(&source_error);
        zip_source_free(source);
        zip_error_fini(&source_error);
        response.status = ParserStatus::invalid_package;
        response.error = "Cannot open DOCX ZIP from inherited descriptor bytes: " + message;
        return response;
    }
    zip_error_fini(&source_error);
    const zip_int64_t signed_entry_count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (signed_entry_count < 0 ||
        static_cast<std::uint64_t>(signed_entry_count) > limits.max_member_count) {
        response.status = signed_entry_count < 0 ? ParserStatus::invalid_package
                                                : ParserStatus::package_limit_exceeded;
        response.error = signed_entry_count < 0 ? "Cannot enumerate DOCX package"
                                                : "DOCX package has too many members";
        zip_discard(archive);
        return response;
    }

    std::unordered_set<std::string> names;
    std::optional<zip_uint64_t> document_index;
    bool has_content_types = false;
    bool has_relationships = false;
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_media_uncompressed = 0;
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(signed_entry_count); ++index) {
        zip_stat_t status;
        zip_stat_init(&status);
        if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &status) != 0 ||
            (status.valid & ZIP_STAT_NAME) == 0 || status.name == nullptr ||
            (status.valid & ZIP_STAT_SIZE) == 0) {
            response.status = ParserStatus::invalid_package;
            response.error = "Cannot inspect a DOCX package member";
            zip_discard(archive);
            return response;
        }
        const char* utf8_name = zip_get_name(archive, index, ZIP_FL_UNCHANGED | ZIP_FL_ENC_UTF_8);
        if (utf8_name == nullptr || !names.emplace(utf8_name).second) {
            response.status = ParserStatus::invalid_package;
            response.error = "DOCX package has an invalid or duplicate member name";
            zip_discard(archive);
            return response;
        }
        const std::string_view name(utf8_name);
        if (status.size > limits.max_member_uncompressed_bytes ||
            status.size > limits.max_total_uncompressed_bytes -
                              std::min(total_uncompressed, limits.max_total_uncompressed_bytes)) {
            response.status = ParserStatus::package_limit_exceeded;
            response.error = "DOCX package exceeds configured expansion limits";
            zip_discard(archive);
            return response;
        }
        total_uncompressed += status.size;
        if (isWordMediaMember(name)) {
            if (status.size > limits.max_media_member_uncompressed_bytes ||
                status.size > limits.max_total_media_uncompressed_bytes -
                                  std::min(total_media_uncompressed,
                                           limits.max_total_media_uncompressed_bytes)) {
                response.status = ParserStatus::package_limit_exceeded;
                response.error =
                    "DOCX package exceeds configured media expansion limits";
                zip_discard(archive);
                return response;
            }
            total_media_uncompressed += status.size;
        }
        if (name == kDocumentPart) {
            document_index = index;
        } else if (name == kContentTypesPart) {
            has_content_types = true;
        } else if (name == kRelationshipsPart) {
            has_relationships = true;
        }
    }
    if (!document_index.has_value() || !has_content_types || !has_relationships) {
        response.status = ParserStatus::invalid_package;
        response.error = "ZIP is missing required DOCX OPC members";
        zip_discard(archive);
        return response;
    }

    std::string document_xml;
    if (!readZipEntry(
            archive, *document_index, limits.max_document_xml_bytes, document_xml, response)) {
        zip_discard(archive);
        return response;
    }
    zip_discard(archive);
    return parseDocumentXml(document_xml, limits);
}

int runParserWorker(int input_fd, int output_fd, const ParserWorkerOptions& options) {
    ParserResponse response;
    if (input_fd < 0 || output_fd < 0 || input_fd == output_fd) {
        response.status = ParserStatus::invalid_request;
        response.error = "Parser worker requires distinct, valid input and output descriptors";
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 64;
    }
    std::string descriptor_error;
    if (!descriptorIsSafeResponseChannel(output_fd, descriptor_error)) {
        return 64;
    }
    if (!descriptorIsReadOnlyRegularFile(input_fd, descriptor_error)) {
        response.status = ParserStatus::invalid_request;
        response.error = std::move(descriptor_error);
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 64;
    }
    const int owned_input = ::fcntl(input_fd, F_DUPFD_CLOEXEC, 3);
    if (owned_input < 0) {
        response.status = ParserStatus::invalid_request;
        response.error = std::string("Cannot duplicate parser input: ") + std::strerror(errno);
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 71;
    }
    if (options.clear_environment && ::clearenv() != 0) {
        ::close(owned_input);
        response.status = ParserStatus::invalid_request;
        response.error = "Cannot clear parser worker environment";
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 71;
    }
    if (options.close_inherited_descriptors &&
        !closeDescriptorsExcept(owned_input, output_fd)) {
        ::close(owned_input);
        response.status = ParserStatus::invalid_request;
        response.error = "Cannot close inherited parser worker descriptors";
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 71;
    }

    SandboxError sandbox_error;
    if (!installParserSandbox(options.sandbox, &sandbox_error)) {
        ::close(owned_input);
        response.status = ParserStatus::sandbox_failed;
        response.error = sandbox_error.message;
        std::string ignored;
        (void)writeParserResponse(output_fd, response, options.parser.max_response_bytes, &ignored);
        return 70;
    }

    try {
        response = parseOwnedDocxDescriptor(owned_input, options.parser);
    } catch (const std::bad_alloc&) {
        response.status = ParserStatus::package_limit_exceeded;
        response.error = "Parser worker exhausted its bounded address space";
    } catch (const std::exception& exception) {
        response.status = ParserStatus::internal_error;
        response.error = std::string("Parser worker failed safely: ") + exception.what();
    } catch (...) {
        response.status = ParserStatus::internal_error;
        response.error = "Parser worker failed with an unknown internal error";
    }
    std::string protocol_error;
    if (!writeParserResponse(
            output_fd, response, options.parser.max_response_bytes, &protocol_error)) {
        if (protocol_error.find("exceeds") == std::string::npos &&
            protocol_error.find("too many") == std::string::npos) {
            return 74;
        }
        ParserResponse fallback;
        fallback.status = ParserStatus::protocol_error;
        fallback.error = std::move(protocol_error);
        std::string ignored;
        if (!writeParserResponse(
                output_fd, fallback, options.parser.max_response_bytes, &ignored)) {
            return 74;
        }
    }
    return 0;
}

}  // namespace docxstudio::worker
