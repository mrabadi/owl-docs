#include "docxstudio/ooxml/docx_document.h"
#include "docxstudio/math/ast.h"
#include "docxstudio/math/latex_parser.h"
#include "docxstudio/xml/complexity.h"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace docxstudio::ooxml {
namespace {

constexpr std::string_view kDocumentPart = "word/document.xml";
constexpr std::string_view kContentTypesPart = "[Content_Types].xml";
constexpr std::string_view kRootRelationshipsPart = "_rels/.rels";
constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kStrictWordNamespace =
    "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr std::string_view kOfficeMathNamespace =
    "http://schemas.openxmlformats.org/officeDocument/2006/math";
constexpr std::string_view kWordprocessingDrawingNamespace =
    "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
constexpr std::string_view kDrawingMainNamespace =
    "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kStrictDrawingMainNamespace =
    "http://purl.oclc.org/ooxml/drawingml/main";
constexpr std::string_view kDrawingPictureNamespace =
    "http://schemas.openxmlformats.org/drawingml/2006/picture";
constexpr std::string_view kOfficeRelationshipsNamespace =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kStrictOfficeRelationshipsNamespace =
    "http://purl.oclc.org/ooxml/officeDocument/relationships";
constexpr std::string_view kDocumentRelationshipsPart =
    "word/_rels/document.xml.rels";
constexpr std::string_view kNewContentTypes =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/word/document.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
    "<Override PartName=\"/word/styles.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
    "<Override PartName=\"/word/settings.xml\" "
    "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
    "</Types>";
constexpr std::string_view kNewRootRelationships =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
    "Target=\"word/document.xml\"/>"
    "</Relationships>";
constexpr std::string_view kNewDocumentRelationships =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
    "Target=\"styles.xml\"/>"
    "<Relationship Id=\"rId2\" "
    "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" "
    "Target=\"settings.xml\"/>"
    "</Relationships>";
constexpr std::int64_t kMaximumInlineExtentEmu = 3'600'000'000LL;
// ECMA-376/ISO 29500 numbering levels are zero-based ilvl 0 through 8.
constexpr std::uint8_t kMaximumNativeNumberingLevel = 8;
constexpr std::size_t kNativeNumberingLevelCount = 9;
constexpr std::size_t kMaximumParagraphStyleIdBytes = 253;
// A legitimate Word style hierarchy is shallow.  Bound adversarial acyclic
// basedOn chains explicitly so resolution cannot exhaust the parser worker's
// stack or repeat unbounded work once per paragraph/run.
constexpr std::size_t kMaximumStyleChainDepth = 64;

struct BuiltInParagraphStyleDescriptor {
    std::string_view id;
    std::string_view name;
    std::string_view next;
    std::uint16_t ui_priority;
    std::uint16_t space_before_twips;
    std::uint16_t space_after_twips;
    std::int16_t left_indent_twips;
    std::int16_t right_indent_twips;
    std::int16_t font_size_half_points;
    std::int8_t outline_level;
    bool keep_with_next;
    bool keep_lines;
    bool bold;
    bool italic;
    std::optional<std::uint32_t> foreground_rgb;
    std::optional<BasicParagraphAlignment> alignment;
};

// Stable ordering is intentional: styles.xml must not depend on the order in
// which paragraphs happen to use a style.
constexpr std::array<BuiltInParagraphStyleDescriptor, 13>
    kBuiltInParagraphStyles{{
        {"NoSpacing", "No Spacing", "NoSpacing", 1, 0, 0, 0, 0, 0, -1,
         false, false, false, false, std::nullopt,
         BasicParagraphAlignment::left},
        {"Title", "Title", "Normal", 10, 0, 240, 0, 0, 56, -1,
         true, true, true, false, 0x0077216fU,
         BasicParagraphAlignment::left},
        {"Subtitle", "Subtitle", "Normal", 11, 0, 240, 0, 0, 28, -1,
         true, true, false, true, 0x005e2750U,
         BasicParagraphAlignment::left},
        {"Quote", "Quote", "Normal", 29, 120, 120, 720, 720, 22, -1,
         false, true, false, true, 0x005e2750U,
         BasicParagraphAlignment::left},
        {"Heading1", "Heading 1", "Normal", 9, 240, 120, 0, 0, 32, 0,
         true, true, true, false, 0x00e95420U,
         BasicParagraphAlignment::left},
        {"Heading2", "Heading 2", "Normal", 9, 200, 80, 0, 0, 26, 1,
         true, true, true, false, 0x0077216fU,
         BasicParagraphAlignment::left},
        {"Heading3", "Heading 3", "Normal", 9, 160, 60, 0, 0, 24, 2,
         true, true, true, false, 0x005e2750U,
         BasicParagraphAlignment::left},
        {"Heading4", "Heading 4", "Normal", 9, 160, 40, 0, 0, 22, 3,
         true, true, true, false, 0x005e2750U,
         BasicParagraphAlignment::left},
        {"Heading5", "Heading 5", "Normal", 9, 140, 40, 0, 0, 22, 4,
         true, true, true, true, 0x0077216fU,
         BasicParagraphAlignment::left},
        {"Heading6", "Heading 6", "Normal", 9, 120, 40, 0, 0, 22, 5,
         true, true, false, true, 0x0077216fU,
         BasicParagraphAlignment::left},
        {"Heading7", "Heading 7", "Normal", 9, 120, 40, 0, 0, 20, 6,
         true, true, true, false, 0x002c001eU,
         BasicParagraphAlignment::left},
        {"Heading8", "Heading 8", "Normal", 9, 100, 40, 0, 0, 20, 7,
         true, true, false, true, 0x002c001eU,
         BasicParagraphAlignment::left},
        {"Heading9", "Heading 9", "Normal", 9, 80, 40, 0, 0, 20, 8,
         true, true, true, true, 0x002c001eU,
         BasicParagraphAlignment::left},
    }};

const BuiltInParagraphStyleDescriptor* builtInParagraphStyle(
    std::string_view style_id) noexcept {
    const auto found = std::find_if(
        kBuiltInParagraphStyles.begin(), kBuiltInParagraphStyles.end(),
        [style_id](const BuiltInParagraphStyleDescriptor& descriptor) {
            return descriptor.id == style_id;
        });
    return found == kBuiltInParagraphStyles.end() ? nullptr : &*found;
}

bool supportedBuiltInParagraphStyle(std::string_view style_id) noexcept {
    return style_id == "Normal" || builtInParagraphStyle(style_id) != nullptr;
}

bool safeParagraphStyleIdToken(std::string_view style_id) noexcept {
    if (style_id.empty() ||
        style_id.size() > kMaximumParagraphStyleIdBytes) {
        return false;
    }
    const auto first = static_cast<unsigned char>(style_id.front());
    const auto ascii_letter = [](unsigned char byte) {
        return (byte >= 'A' && byte <= 'Z') ||
               (byte >= 'a' && byte <= 'z');
    };
    if (!(ascii_letter(first) || first == '_')) return false;
    return std::all_of(
        style_id.begin() + 1, style_id.end(), [ascii_letter](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return ascii_letter(byte) || (byte >= '0' && byte <= '9') ||
                   byte == '_' || byte == '-' || byte == '.';
        });
}

using NewParagraphStyleCatalog = std::set<std::string>;

struct BasicTableStyleDescriptor {
    BasicTableStyle style;
    std::string_view word_style_id;
    std::string_view border_rgb;
    std::string_view header_fill_rgb;
    std::string_view header_text_rgb;
    std::string_view band_fill_rgb;
};

constexpr std::array<BasicTableStyleDescriptor, 13> kBasicTableStyles{{
    {BasicTableStyle::plain, "TableNormal", "B7B7B7", "FFFFFF", "000000", "FFFFFF"},
    {BasicTableStyle::grid, "TableGrid", "444444", "FFFFFF", "000000", "FFFFFF"},
    {BasicTableStyle::light_gray, "LightShading", "A6A6A6", "D9E1F2", "000000", "F2F2F2"},
    {BasicTableStyle::light_blue, "LightShading-Accent1", "9EADBA", "5B9BD5", "FFFFFF", "DDEBF7"},
    {BasicTableStyle::light_orange, "LightShading-Accent2", "C88A73", "E95420", "FFFFFF", "FBE9E1"},
    {BasicTableStyle::medium_blue, "MediumShading1-Accent1", "2F75B5", "2F75B5", "FFFFFF", "D9EAF7"},
    {BasicTableStyle::medium_green, "MediumShading1-Accent6", "548235", "548235", "FFFFFF", "E2F0D9"},
    {BasicTableStyle::medium_orange, "MediumShading1-Accent2", "C65911", "C65911", "FFFFFF", "FCE4D6"},
    {BasicTableStyle::aubergine, "MediumShading1-Accent4", "5E2750", "77216F", "FFFFFF", "EFE3EE"},
    {BasicTableStyle::orange_accent, "ColorfulShading-Accent2", "77216F", "E95420", "FFFFFF", "FFF2ED"},
    {BasicTableStyle::banded_blue, "ColorfulList-Accent1", "5B9BD5", "1F4E78", "FFFFFF", "D9EAF7"},
    {BasicTableStyle::banded_aubergine, "ColorfulList-Accent4", "77216F", "5E2750", "FFFFFF", "EADDE8"},
    {BasicTableStyle::dark_header, "ColorfulGrid-Accent4", "7F7F7F", "262626", "FFFFFF", "F2F2F2"},
}};

const BasicTableStyleDescriptor& tableStyleDescriptor(BasicTableStyle style) {
    const auto found = std::find_if(
        kBasicTableStyles.begin(), kBasicTableStyles.end(),
        [style](const BasicTableStyleDescriptor& descriptor) {
            return descriptor.style == style;
        });
    // BasicTableStyle is closed; retaining a deterministic fallback also
    // keeps malformed enum values from indexing outside the descriptor table.
    return found == kBasicTableStyles.end() ? kBasicTableStyles[1] : *found;
}

std::optional<BasicTableStyle> basicTableStyleFromWordId(
    std::string_view style_id) {
    const auto found = std::find_if(
        kBasicTableStyles.begin(), kBasicTableStyles.end(),
        [style_id](const BasicTableStyleDescriptor& descriptor) {
            return descriptor.word_style_id == style_id;
        });
    if (found == kBasicTableStyles.end()) return std::nullopt;
    return found->style;
}

struct EntryRecord {
    PackageMember member;
    zip_uint64_t index{0};
    zip_uint64_t valid_fields{0};
    std::int64_t modified_time{0};
};

struct SpanLocation {
    TextSpanId id{0};
    std::size_t paragraph_index{0};
    std::size_t run_index{0};
    std::size_t fragment_index{0};
    std::size_t content_begin{0};
    std::size_t content_end{0};
    std::string original_text;
    bool editable{false};
    bool preserves_space{false};
    std::string refusal_reason;
};

struct ThemeData {
    std::optional<std::string> major_latin_font;
    std::optional<std::string> minor_latin_font;
    std::unordered_map<std::string, std::uint32_t> colors;
};

struct StyleDefinition {
    std::string id;
    std::optional<std::string> based_on;
    bool paragraph_style{false};
    bool default_style{false};
    Paragraph paragraph;
    BasicRunFormat run_format;
    bool run_format_is_basic{true};
};

struct NumberLevelDefinition {
    ImportedNumbering value;
    bool supported{false};
};

struct NumberInstanceDefinition {
    std::int32_t abstract_id{0};
    std::unordered_map<std::uint8_t, std::int32_t> start_overrides;
    std::unordered_map<std::uint8_t, NumberLevelDefinition> level_overrides;
};

struct ImportContext {
    ThemeData theme;
    BasicRunFormat default_run_format;
    Paragraph default_paragraph_format;
    std::optional<std::string> default_paragraph_style;
    std::unordered_map<std::string, StyleDefinition> styles;
    std::unordered_map<std::int32_t,
                       std::unordered_map<std::uint8_t, NumberLevelDefinition>>
        abstract_numbering;
    std::unordered_map<std::int32_t, NumberInstanceDefinition> numbering;
};

struct ParsedOmmlEquation {
    EquationPayload payload;
    BasicRunFormat direct_format;
    std::optional<std::string> style_id;
};

bool hasOnlyWordAttributes(
    const pugi::xml_node& node,
    std::initializer_list<std::string_view> allowed_names);
void parseBasicRunProperties(
    const pugi::xml_node& properties,
    BasicRunFormat& format,
    bool& format_is_basic,
    const ThemeData* theme,
    std::optional<std::string>* style_id);

struct ParsedPackage {
    std::vector<std::uint8_t> bytes;
    std::uint32_t source_mode{0600};
    std::vector<EntryRecord> entries;
    std::size_t document_entry_index{0};
    std::string document_xml;
    std::vector<Paragraph> paragraphs;
    std::vector<ImportedBodyBlock> body_blocks;
    std::optional<PageSettings> page_settings;
    std::vector<ImportedSection> sections;
    std::optional<std::string> header_text;
    std::optional<std::string> footer_text;
    std::string header_xml;
    std::string footer_xml;
    std::vector<SpanLocation> spans;
    CompatibilityReport compatibility;
    std::optional<DocumentDefaults> canonical_simple_regeneration_defaults;
};

std::string buildNewStylesXml(
    const DocumentDefaults& defaults,
    const NewParagraphStyleCatalog& paragraph_styles);
std::string buildNewSettingsXml(const DocumentDefaults& defaults);
std::string buildStoryXml(std::string_view text, bool header,
                          const PageSettings& page);

void setError(Error* error, ErrorCode code, std::string message) {
    if (error != nullptr) {
        *error = Error{code, std::move(message)};
    }
}

bool enforceXmlComplexity(
    const pugi::xml_node& tree,
    const OpenOptions& options,
    std::string_view part_name,
    Error* error) {
    const auto complexity = ::docxstudio::xml::inspectComplexity(
        tree, options.max_xml_depth, options.max_xml_nodes);
    if (complexity.accepted()) {
        return true;
    }
    std::ostringstream message;
    message << part_name << " exceeds the configured XML ";
    if (complexity.status == ::docxstudio::xml::ComplexityStatus::depth_exceeded) {
        message << "depth limit of " << options.max_xml_depth;
    } else {
        message << "node-count limit of " << options.max_xml_nodes;
    }
    setError(error, ErrorCode::xml_complexity_exceeded, message.str());
    return false;
}

std::string errnoMessage(std::string_view operation, const std::filesystem::path& path) {
    std::ostringstream stream;
    stream << operation << " '" << path.string() << "': " << std::strerror(errno);
    return stream.str();
}

std::string zipArchiveError(zip_t* archive) {
    if (archive == nullptr) {
        return "unknown libzip error";
    }
    return zip_error_strerror(zip_get_error(archive));
}

std::string zipCodeError(int code) {
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    const std::string message = zip_error_strerror(&error);
    zip_error_fini(&error);
    return message;
}

bool readFile(
    const std::filesystem::path& path,
    std::uint64_t maximum_size,
    std::vector<std::uint8_t>& bytes,
    std::uint32_t& mode,
    Error* error) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        setError(error, ErrorCode::io_error, errnoMessage("Cannot open", path));
        return false;
    }

    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        const std::string message = errnoMessage("Cannot inspect", path);
        ::close(descriptor);
        setError(error, ErrorCode::io_error, message);
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        ::close(descriptor);
        setError(error, ErrorCode::io_error, "DOCX input is not a regular file");
        return false;
    }

    const auto size = static_cast<std::uint64_t>(status.st_size);
    if (size > maximum_size || size > std::numeric_limits<std::size_t>::max()) {
        ::close(descriptor);
        setError(error, ErrorCode::package_too_large, "DOCX package exceeds the configured byte limit");
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size));
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        const ssize_t count = ::read(
            descriptor,
            bytes.data() + consumed,
            std::min<std::size_t>(bytes.size() - consumed, static_cast<std::size_t>(SSIZE_MAX)));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const std::string message = count == 0 ? "Unexpected end of DOCX input"
                                                   : errnoMessage("Cannot read", path);
            ::close(descriptor);
            setError(error, ErrorCode::io_error, message);
            return false;
        }
        consumed += static_cast<std::size_t>(count);
    }
    mode = static_cast<std::uint32_t>(status.st_mode & 0777U);
    ::close(descriptor);
    return true;
}

zip_t* openZipFromBytes(const std::vector<std::uint8_t>& bytes, Error* error) {
    zip_error_t zip_error;
    zip_error_init(&zip_error);
    zip_source_t* source = zip_source_buffer_create(
        bytes.empty() ? nullptr : bytes.data(), static_cast<zip_uint64_t>(bytes.size()), 0, &zip_error);
    if (source == nullptr) {
        const std::string message = zip_error_strerror(&zip_error);
        zip_error_fini(&zip_error);
        setError(error, ErrorCode::invalid_zip, "Cannot create ZIP source: " + message);
        return nullptr;
    }

    zip_t* archive = zip_open_from_source(source, ZIP_RDONLY, &zip_error);
    if (archive == nullptr) {
        const std::string message = zip_error_strerror(&zip_error);
        zip_source_free(source);
        zip_error_fini(&zip_error);
        setError(error, ErrorCode::invalid_zip, "Cannot open DOCX package: " + message);
        return nullptr;
    }
    zip_error_fini(&zip_error);
    return archive;
}

bool readEntry(
    zip_t* archive,
    zip_uint64_t index,
    std::uint64_t maximum_size,
    std::string& contents,
    Error* error) {
    zip_stat_t status;
    zip_stat_init(&status);
    if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &status) != 0 ||
        (status.valid & ZIP_STAT_SIZE) == 0) {
        setError(error, ErrorCode::unreadable_package_member,
                 "Cannot inspect package member: " + zipArchiveError(archive));
        return false;
    }
    if (status.size > maximum_size || status.size > std::numeric_limits<std::size_t>::max()) {
        setError(error, ErrorCode::package_limit_exceeded,
                 "Package member exceeds its configured uncompressed byte limit");
        return false;
    }

    zip_file_t* file = zip_fopen_index(archive, index, ZIP_FL_UNCHANGED);
    if (file == nullptr) {
        setError(error, ErrorCode::unreadable_package_member,
                 "Cannot open package member: " + zipArchiveError(archive));
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
            const std::string message = count == 0 ? "Unexpected end of package member"
                                                   : zip_file_strerror(file);
            zip_fclose(file);
            setError(error, ErrorCode::unreadable_package_member, message);
            return false;
        }
        consumed += static_cast<std::size_t>(count);
    }
    if (zip_fclose(file) != 0) {
        setError(error, ErrorCode::unreadable_package_member,
                 "Cannot finish reading package member");
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

std::string namespaceUriForName(const pugi::xml_node& scope, std::string_view qualified_name) {
    const std::string_view prefix = prefixName(qualified_name);
    const std::string attribute_name = prefix.empty() ? "xmlns" : "xmlns:" + std::string(prefix);
    for (pugi::xml_node current = scope; current; current = current.parent()) {
        if (const pugi::xml_attribute attribute = current.attribute(attribute_name.c_str())) {
            return attribute.value();
        }
    }
    return {};
}

std::string namespaceUri(const pugi::xml_node& node) {
    return namespaceUriForName(node, node.name());
}

std::optional<std::string> wordAttribute(
    const pugi::xml_node& node,
    std::string_view expected_local_name) {
    for (pugi::xml_attribute attribute : node.attributes()) {
        if (localName(attribute.name()) != expected_local_name) {
            continue;
        }
        const std::string uri = namespaceUriForName(node, attribute.name());
        if (uri == kWordNamespace || uri == kStrictWordNamespace) {
            return std::string(attribute.value());
        }
    }
    return std::nullopt;
}

bool isWordElement(const pugi::xml_node& node, std::string_view expected_local_name) {
    if (node.type() != pugi::node_element || localName(node.name()) != expected_local_name) {
        return false;
    }
    const std::string uri = namespaceUri(node);
    return uri == kWordNamespace || uri == kStrictWordNamespace;
}

bool isMathElement(const pugi::xml_node& node, std::string_view expected_local_name) {
    return node.type() == pugi::node_element &&
           localName(node.name()) == expected_local_name &&
           namespaceUri(node) == kOfficeMathNamespace;
}

bool isNamespacedElement(const pugi::xml_node& node,
                         std::string_view expected_local_name,
                         std::string_view expected_namespace) {
    return node.type() == pugi::node_element &&
           localName(node.name()) == expected_local_name &&
           namespaceUri(node) == expected_namespace;
}

bool isDrawingElement(const pugi::xml_node& node,
                      std::string_view expected_local_name) {
    if (node.type() != pugi::node_element ||
        localName(node.name()) != expected_local_name) {
        return false;
    }
    const auto uri = namespaceUri(node);
    return uri == kDrawingMainNamespace ||
           uri == kStrictDrawingMainNamespace;
}

std::optional<std::string> namespacedAttribute(
    const pugi::xml_node& node,
    std::string_view expected_local_name,
    std::initializer_list<std::string_view> accepted_namespaces) {
    for (const pugi::xml_attribute attribute : node.attributes()) {
        if (localName(attribute.name()) != expected_local_name) continue;
        const std::string uri = namespaceUriForName(node, attribute.name());
        if (std::find(accepted_namespaces.begin(), accepted_namespaces.end(), uri) !=
            accepted_namespaces.end()) {
            return std::string(attribute.value());
        }
    }
    return std::nullopt;
}

std::optional<std::string> mathAttribute(
    const pugi::xml_node& node,
    std::string_view expected_local_name) {
    for (pugi::xml_attribute attribute : node.attributes()) {
        if (localName(attribute.name()) == expected_local_name &&
            namespaceUriForName(node, attribute.name()) == kOfficeMathNamespace) {
            return std::string(attribute.value());
        }
    }
    return std::nullopt;
}

bool isWhitespaceOnly(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    });
}

bool ignorableNode(const pugi::xml_node& node) {
    return node.type() == pugi::node_comment || node.type() == pugi::node_pi ||
           (node.type() == pugi::node_pcdata && isWhitespaceOnly(node.value()));
}

bool hasOnlyIgnorableChildren(const pugi::xml_node& node) {
    return std::all_of(node.begin(), node.end(), [](const pugi::xml_node& child) {
        return ignorableNode(child);
    });
}

std::string latexForOmmlToken(std::string_view token) {
    static constexpr std::pair<std::string_view, std::string_view> symbols[]{
        {"alpha", "α"},       {"beta", "β"},       {"gamma", "γ"},
        {"delta", "δ"},       {"epsilon", "ϵ"},    {"varepsilon", "ε"},
        {"zeta", "ζ"},        {"eta", "η"},        {"theta", "θ"},
        {"vartheta", "ϑ"},    {"iota", "ι"},       {"kappa", "κ"},
        {"lambda", "λ"},      {"mu", "μ"},         {"nu", "ν"},
        {"xi", "ξ"},          {"pi", "π"},         {"varpi", "ϖ"},
        {"rho", "ρ"},         {"sigma", "σ"},      {"tau", "τ"},
        {"upsilon", "υ"},     {"phi", "ϕ"},        {"varphi", "φ"},
        {"chi", "χ"},         {"psi", "ψ"},        {"omega", "ω"},
        {"Gamma", "Γ"},       {"Delta", "Δ"},      {"Theta", "Θ"},
        {"Lambda", "Λ"},      {"Xi", "Ξ"},         {"Pi", "Π"},
        {"Sigma", "Σ"},       {"Upsilon", "Υ"},    {"Phi", "Φ"},
        {"Psi", "Ψ"},         {"Omega", "Ω"},      {"infty", "∞"},
        {"times", "×"},       {"cdot", "⋅"},       {"pm", "±"},
        {"mp", "∓"},          {"div", "÷"},        {"leq", "≤"},
        {"geq", "≥"},         {"neq", "≠"},        {"approx", "≈"},
        {"sim", "∼"},         {"to", "→"},         {"in", "∈"},
        {"notin", "∉"},       {"subset", "⊂"},     {"subseteq", "⊆"},
        {"cup", "∪"},         {"cap", "∩"},        {"land", "∧"},
        {"lor", "∨"},         {"partial", "∂"},    {"nabla", "∇"},
        {"|", "‖"},
    };
    for (const auto& [command, glyph] : symbols) {
        if (token == glyph) return "\\" + std::string(command);
    }
    if (token == "{") return "\\{";
    if (token == "}") return "\\}";
    return std::string(token);
}

std::optional<std::string> parseOmmlNode(const pugi::xml_node& node);

std::optional<std::string> parseOmmlChildren(const pugi::xml_node& parent) {
    std::string result;
    for (pugi::xml_node child : parent.children()) {
        if (ignorableNode(child)) continue;
        if (child.type() != pugi::node_element) return std::nullopt;
        auto parsed = parseOmmlNode(child);
        if (!parsed) return std::nullopt;
        if (!parsed->empty()) {
            if (!result.empty()) result.push_back(' ');
            result += *parsed;
        }
    }
    return result;
}

std::optional<std::string> parseOmmlArgument(
    const pugi::xml_node& parent,
    std::string_view name,
    bool required = true) {
    pugi::xml_node argument;
    for (pugi::xml_node child : parent.children()) {
        if (!isMathElement(child, name)) continue;
        if (argument) return std::nullopt;
        argument = child;
    }
    if (!argument) {
        return required ? std::nullopt : std::optional<std::string>{std::string{}};
    }
    return parseOmmlChildren(argument);
}

std::optional<std::string> parseOmmlMatrix(
    const pugi::xml_node& node,
    std::string_view environment) {
    std::vector<std::vector<std::string>> rows;
    bool saw_properties = false;
    std::optional<std::size_t> columns;
    for (pugi::xml_node child : node.children()) {
        if (ignorableNode(child)) continue;
        if (isMathElement(child, "mPr")) {
            if (saw_properties) return std::nullopt;
            saw_properties = true;
            continue;
        }
        if (!isMathElement(child, "mr")) return std::nullopt;
        std::vector<std::string> row;
        for (pugi::xml_node cell : child.children()) {
            if (ignorableNode(cell)) continue;
            if (!isMathElement(cell, "e")) return std::nullopt;
            auto contents = parseOmmlChildren(cell);
            if (!contents) return std::nullopt;
            row.push_back(std::move(*contents));
        }
        if (row.empty()) return std::nullopt;
        if (!columns) {
            columns = row.size();
        } else if (*columns != row.size()) {
            return std::nullopt;
        }
        rows.push_back(std::move(row));
    }
    if (rows.empty()) return std::nullopt;

    std::string result = "\\begin{" + std::string(environment) + "}";
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (row != 0) result += " \\\\ ";
        for (std::size_t column = 0; column < rows[row].size(); ++column) {
            if (column != 0) result += " & ";
            result += rows[row][column];
        }
    }
    result += "\\end{" + std::string(environment) + "}";
    return result;
}

std::optional<std::string> parseOmmlDelimiter(std::string_view glyph) {
    if (glyph.empty()) return std::string{"."};
    if (glyph == "(" || glyph == ")" || glyph == "[" || glyph == "]" ||
        glyph == "|" || glyph == "<" || glyph == ">") {
        return std::string(glyph);
    }
    if (glyph == "{") return std::string{"\\{"};
    if (glyph == "}") return std::string{"\\}"};
    if (glyph == "⟨") return std::string{"\\langle"};
    if (glyph == "⟩") return std::string{"\\rangle"};
    if (glyph == "‖") return std::string{"\\Vert"};
    return std::nullopt;
}

std::optional<std::string> parseOmmlNode(const pugi::xml_node& node) {
    if (isMathElement(node, "r")) {
        std::optional<std::string> text;
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child)) continue;
            if (isMathElement(child, "rPr") || isWordElement(child, "rPr")) continue;
            if (!isMathElement(child, "t") || text.has_value()) return std::nullopt;
            for (pugi::xml_node nested : child.children()) {
                if (nested.type() == pugi::node_element) return std::nullopt;
            }
            text = latexForOmmlToken(child.text().get());
        }
        return text;
    }

    if (isMathElement(node, "box")) {
        auto body = parseOmmlArgument(node, "e");
        if (!body) return std::nullopt;
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child) || isMathElement(child, "boxPr") ||
                isMathElement(child, "e")) continue;
            return std::nullopt;
        }
        return "{" + *body + "}";
    }

    if (isMathElement(node, "f")) {
        auto numerator = parseOmmlArgument(node, "num");
        auto denominator = parseOmmlArgument(node, "den");
        if (!numerator || numerator->empty() || !denominator || denominator->empty()) {
            return std::nullopt;
        }
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child) || isMathElement(child, "fPr") ||
                isMathElement(child, "num") || isMathElement(child, "den")) continue;
            return std::nullopt;
        }
        return "\\frac{" + *numerator + "}{" + *denominator + "}";
    }

    if (isMathElement(node, "rad")) {
        auto degree = parseOmmlArgument(node, "deg");
        auto radicand = parseOmmlArgument(node, "e");
        if (!degree || !radicand || radicand->empty()) return std::nullopt;
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child) || isMathElement(child, "radPr") ||
                isMathElement(child, "deg") || isMathElement(child, "e")) continue;
            return std::nullopt;
        }
        return degree->empty() ? "\\sqrt{" + *radicand + "}"
                               : "\\sqrt[" + *degree + "]{" + *radicand + "}";
    }

    const auto parse_script = [&](std::string_view tag,
                                  bool has_subscript,
                                  bool has_superscript) -> std::optional<std::string> {
        if (!isMathElement(node, tag)) return std::nullopt;
        auto base = parseOmmlArgument(node, "e");
        auto subscript = parseOmmlArgument(node, "sub", has_subscript);
        auto superscript = parseOmmlArgument(node, "sup", has_superscript);
        if (!base || base->empty() || !subscript || !superscript ||
            (has_subscript && subscript->empty()) ||
            (has_superscript && superscript->empty())) {
            return std::nullopt;
        }
        std::string result = *base;
        if (has_subscript) result += "_{" + *subscript + "}";
        if (has_superscript) result += "^{" + *superscript + "}";
        return result;
    };
    if (isMathElement(node, "sSubSup")) return parse_script("sSubSup", true, true);
    if (isMathElement(node, "sSub")) return parse_script("sSub", true, false);
    if (isMathElement(node, "sSup")) return parse_script("sSup", false, true);

    if (isMathElement(node, "nary")) {
        std::optional<std::string> operation;
        for (pugi::xml_node child : node.children()) {
            if (!isMathElement(child, "naryPr")) continue;
            for (pugi::xml_node property : child.children()) {
                if (isMathElement(property, "chr")) operation = mathAttribute(property, "val");
            }
        }
        if (!operation || (*operation != "∑" && *operation != "∫")) {
            return std::nullopt;
        }
        auto subscript = parseOmmlArgument(node, "sub");
        auto superscript = parseOmmlArgument(node, "sup");
        auto operand = parseOmmlArgument(node, "e");
        if (!subscript || !superscript || !operand || !operand->empty()) {
            return std::nullopt;
        }
        std::string result = *operation == "∑" ? "\\sum" : "\\int";
        if (!subscript->empty()) result += "_{" + *subscript + "}";
        if (!superscript->empty()) result += "^{" + *superscript + "}";
        return result;
    }

    if (isMathElement(node, "m")) return parseOmmlMatrix(node, "matrix");

    if (isMathElement(node, "d")) {
        std::optional<std::string> begin_glyph;
        std::optional<std::string> end_glyph;
        pugi::xml_node expression;
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child)) continue;
            if (isMathElement(child, "dPr")) {
                for (pugi::xml_node property : child.children()) {
                    if (isMathElement(property, "begChr")) {
                        begin_glyph = mathAttribute(property, "val");
                    } else if (isMathElement(property, "endChr")) {
                        end_glyph = mathAttribute(property, "val");
                    } else if (!isMathElement(property, "sepChr")) {
                        return std::nullopt;
                    }
                }
            } else if (isMathElement(child, "e") && !expression) {
                expression = child;
            } else {
                return std::nullopt;
            }
        }
        if (!begin_glyph || !end_glyph || !expression) return std::nullopt;
        pugi::xml_node only_child;
        for (pugi::xml_node child : expression.children()) {
            if (ignorableNode(child)) continue;
            if (only_child) {
                only_child = {};
                break;
            }
            only_child = child;
        }
        if (only_child && isMathElement(only_child, "m")) {
            std::string_view environment;
            if (*begin_glyph == "(" && *end_glyph == ")") environment = "pmatrix";
            if (*begin_glyph == "[" && *end_glyph == "]") environment = "bmatrix";
            if (*begin_glyph == "{" && *end_glyph == "}") environment = "Bmatrix";
            if (*begin_glyph == "|" && *end_glyph == "|") environment = "vmatrix";
            if (*begin_glyph == "‖" && *end_glyph == "‖") environment = "Vmatrix";
            if (!environment.empty()) return parseOmmlMatrix(only_child, environment);
        }
        auto left = parseOmmlDelimiter(*begin_glyph);
        auto right = parseOmmlDelimiter(*end_glyph);
        auto body = parseOmmlChildren(expression);
        if (!left || !right || !body || body->empty()) return std::nullopt;
        return "\\left" + *left + " " + *body + " \\right" + *right;
    }

    return std::nullopt;
}

bool collectUniformOmmlRunFormat(
    const pugi::xml_node& node,
    const ThemeData* theme,
    std::optional<BasicRunFormat>& uniform_format,
    std::optional<std::optional<std::string>>& uniform_style_id,
    bool& saw_math_run) {
    if (isMathElement(node, "r")) {
        pugi::xml_node word_properties;
        for (const pugi::xml_node child : node.children()) {
            if (!isWordElement(child, "rPr")) continue;
            if (word_properties) return false;
            word_properties = child;
        }

        BasicRunFormat direct_format;
        std::optional<std::string> style_id;
        bool format_is_basic = true;
        if (word_properties) {
            format_is_basic = hasOnlyWordAttributes(word_properties, {});
            parseBasicRunProperties(
                word_properties, direct_format, format_is_basic,
                theme, &style_id);
        }
        if (!format_is_basic) return false;

        if (!saw_math_run) {
            uniform_format = direct_format;
            uniform_style_id = style_id;
            saw_math_run = true;
        } else if (*uniform_format != direct_format ||
                   *uniform_style_id != style_id) {
            return false;
        }
        return true;
    }

    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) continue;
        if (!collectUniformOmmlRunFormat(
                child, theme, uniform_format, uniform_style_id,
                saw_math_run)) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedOmmlEquation> parseOmmlEquation(
    const pugi::xml_node& node,
    const ThemeData* theme = nullptr) {
    bool display = false;
    pugi::xml_node equation = node;
    if (isMathElement(node, "oMathPara")) {
        display = true;
        equation = {};
        for (pugi::xml_node child : node.children()) {
            if (ignorableNode(child) || isMathElement(child, "oMathParaPr")) continue;
            if (!isMathElement(child, "oMath") || equation) return std::nullopt;
            equation = child;
        }
    }
    if (!equation || !isMathElement(equation, "oMath")) return std::nullopt;
    auto source = parseOmmlChildren(equation);
    if (!source || source->empty()) return std::nullopt;
    const auto parsed = math::parseLatex(*source);
    if (!parsed) return std::nullopt;

    std::optional<BasicRunFormat> direct_format;
    std::optional<std::optional<std::string>> style_id;
    bool saw_math_run = false;
    if (!collectUniformOmmlRunFormat(
            equation, theme, direct_format, style_id, saw_math_run)) {
        return std::nullopt;
    }

    return ParsedOmmlEquation{
        EquationPayload{math::toCanonicalLatex(parsed.value()), display},
        direct_format.value_or(BasicRunFormat{}),
        style_id.value_or(std::optional<std::string>{})};
}

bool hasOnlyWordAttributes(
    const pugi::xml_node& node,
    std::initializer_list<std::string_view> allowed_names) {
    for (pugi::xml_attribute attribute : node.attributes()) {
        const std::string_view qualified_name = attribute.name();
        if (qualified_name == "xmlns" || prefixName(qualified_name) == "xmlns") {
            continue;
        }
        const std::string uri = namespaceUriForName(node, qualified_name);
        if (uri != kWordNamespace && uri != kStrictWordNamespace) {
            return false;
        }
        const std::string_view name = localName(qualified_name);
        if (std::find(allowed_names.begin(), allowed_names.end(), name) ==
            allowed_names.end()) {
            return false;
        }
    }
    return true;
}

bool hasWordAncestorBetween(
    pugi::xml_node node,
    const pugi::xml_node& stop_exclusive,
    const std::set<std::string_view>& names) {
    for (pugi::xml_node current = node.parent(); current && current != stop_exclusive;
         current = current.parent()) {
        for (const std::string_view name : names) {
            if (isWordElement(current, name)) {
                return true;
            }
        }
    }
    return false;
}

bool hasDangerousFieldMarkup(const pugi::xml_node& paragraph) {
    std::vector<pugi::xml_node> pending{paragraph};
    while (!pending.empty()) {
        const pugi::xml_node current = pending.back();
        pending.pop_back();
        for (pugi::xml_node child : current.children()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            if (isWordElement(child, "fldChar") || isWordElement(child, "instrText") ||
                isWordElement(child, "delText") || isWordElement(child, "object")) {
                return true;
            }
            pending.push_back(child);
        }
    }
    return false;
}

std::optional<std::size_t> findTagEnd(std::string_view xml, std::size_t begin) {
    if (begin >= xml.size() || xml[begin] != '<') {
        return std::nullopt;
    }
    char quote = '\0';
    for (std::size_t index = begin + 1; index < xml.size(); ++index) {
        const char character = xml[index];
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            }
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '>') {
            return index;
        }
    }
    return std::nullopt;
}

bool mapTextContent(
    const pugi::xml_node& text_element,
    std::string_view xml,
    std::size_t& content_begin,
    std::size_t& content_end,
    std::string& reason) {
    const ptrdiff_t signed_offset = text_element.offset_debug();
    if (signed_offset < 0) {
        reason = "pugixml did not retain a source offset for this w:t element";
        return false;
    }
    std::size_t element_begin = static_cast<std::size_t>(signed_offset);
    // pugixml 1.12 reports an element at the first byte of its qualified
    // name (immediately after '<'), while character nodes point at their
    // first content byte. Accept either convention explicitly.
    if (element_begin < xml.size() && xml[element_begin] != '<' && element_begin > 0 &&
        xml[element_begin - 1] == '<') {
        --element_begin;
    }
    const auto opening_end = findTagEnd(xml, element_begin);
    if (!opening_end.has_value()) {
        reason = "The w:t opening tag cannot be bounded in the original UTF-8 XML";
        return false;
    }

    std::size_t last_non_space = *opening_end;
    while (last_non_space > element_begin &&
           (xml[last_non_space - 1] == ' ' || xml[last_non_space - 1] == '\t' ||
            xml[last_non_space - 1] == '\r' || xml[last_non_space - 1] == '\n')) {
        --last_non_space;
    }
    if (last_non_space > element_begin && xml[last_non_space - 1] == '/') {
        reason = "Self-closing w:t elements require a structural rewrite";
        return false;
    }

    content_begin = *opening_end + 1;
    const std::string closing_prefix = "</" + std::string(text_element.name());
    const std::size_t markup = xml.find('<', content_begin);
    if (markup == std::string_view::npos ||
        xml.substr(markup, closing_prefix.size()) != closing_prefix) {
        reason = "The w:t element contains nested markup, CDATA, or comments";
        return false;
    }
    const auto closing_end = findTagEnd(xml, markup);
    if (!closing_end.has_value()) {
        reason = "The w:t closing tag cannot be bounded";
        return false;
    }
    const std::string_view closing_inside =
        xml.substr(markup + closing_prefix.size(), *closing_end - markup - closing_prefix.size());
    if (!isWhitespaceOnly(closing_inside)) {
        reason = "The w:t closing tag is not in the supported canonical form";
        return false;
    }
    content_end = markup;
    return true;
}

bool isUtf8Xml(std::string_view xml) {
    if (xml.size() >= 2) {
        const auto first = static_cast<unsigned char>(xml[0]);
        const auto second = static_cast<unsigned char>(xml[1]);
        if ((first == 0xffU && second == 0xfeU) || (first == 0xfeU && second == 0xffU)) {
            return false;
        }
    }
    const std::size_t declaration_end = xml.find("?>");
    if (!xml.starts_with("<?xml") || declaration_end == std::string_view::npos) {
        return true;
    }
    std::string declaration(xml.substr(0, std::min<std::size_t>(declaration_end, 512)));
    std::transform(declaration.begin(), declaration.end(), declaration.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::size_t encoding = declaration.find("encoding");
    if (encoding == std::string::npos) {
        return true;
    }
    const std::size_t equals = declaration.find('=', encoding + 8);
    if (equals == std::string::npos) {
        return false;
    }
    const std::size_t quote = declaration.find_first_of("\"'", equals + 1);
    if (quote == std::string::npos) {
        return false;
    }
    const std::size_t end_quote = declaration.find(declaration[quote], quote + 1);
    if (end_quote == std::string::npos) {
        return false;
    }
    const std::string_view value(declaration.data() + quote + 1, end_quote - quote - 1);
    return value == "utf-8" || value == "utf8" || value == "us-ascii";
}

void appendIssueOnce(
    std::vector<CompatibilityIssue>& issues,
    std::set<std::pair<IssueCode, std::string>>& seen,
    CompatibilityIssue issue) {
    const auto key = std::make_pair(issue.code, issue.detail);
    if (seen.insert(key).second) {
        issues.push_back(std::move(issue));
    }
}

void collectParagraphNodes(const pugi::xml_node& node, std::vector<pugi::xml_node>& paragraphs) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (isWordElement(child, "p")) {
            paragraphs.push_back(child);
        } else {
            collectParagraphNodes(child, paragraphs);
        }
    }
}

void collectContentForParagraph(
    const pugi::xml_node& node,
    const pugi::xml_node& paragraph,
    std::vector<pugi::xml_node>& content) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (isWordElement(child, "p") && child != paragraph) {
            continue;
        }
        if (isWordElement(child, "r")) {
            content.push_back(child);
        } else if (isMathElement(child, "oMath") ||
                   isMathElement(child, "oMathPara")) {
            content.push_back(child);
        } else {
            collectContentForParagraph(child, paragraph, content);
        }
    }
}

std::optional<InlineImagePayload> parseInlineImage(
    const pugi::xml_node& drawing);

bool directRunStructureSupported(const pugi::xml_node& run) {
    for (pugi::xml_node child : run.children()) {
        if (ignorableNode(child)) {
            continue;
        }
        const bool supported_drawing =
            isWordElement(child, "drawing") &&
            parseInlineImage(child).has_value();
        if (child.type() != pugi::node_element ||
            !(isWordElement(child, "rPr") || isWordElement(child, "t") ||
              isWordElement(child, "tab") || isWordElement(child, "br") ||
              isWordElement(child, "cr") || supported_drawing)) {
            return false;
        }
    }
    return true;
}

bool directParagraphStructureSupported(
    const pugi::xml_node& paragraph,
    const ThemeData* theme = nullptr) {
    for (pugi::xml_node child : paragraph.children()) {
        if (ignorableNode(child)) {
            continue;
        }
        if (child.type() != pugi::node_element ||
            !(isWordElement(child, "pPr") || isWordElement(child, "r") ||
              isMathElement(child, "oMath") || isMathElement(child, "oMathPara"))) {
            return false;
        }
        if (isWordElement(child, "r") && !directRunStructureSupported(child)) {
            return false;
        }
        if ((isMathElement(child, "oMath") || isMathElement(child, "oMathPara")) &&
            !parseOmmlEquation(child, theme)) {
            return false;
        }
    }
    return true;
}

bool onOffValue(const pugi::xml_node& node, bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, "val");
    if (!value.has_value() || *value == "1" || *value == "true" || *value == "on") {
        return true;
    }
    if (*value == "0" || *value == "false" || *value == "off") {
        return false;
    }
    recognized = false;
    return false;
}

std::optional<std::int32_t> parseHalfPoints(const pugi::xml_node& node, bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, "val");
    if (!value.has_value()) {
        recognized = false;
        return std::nullopt;
    }
    std::int32_t half_points = 0;
    const auto result = std::from_chars(
        value->data(), value->data() + value->size(), half_points, 10);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size() ||
        half_points < 2 || half_points > 3276) {
        recognized = false;
        return std::nullopt;
    }
    return half_points;
}

std::optional<std::int32_t> parseSignedIntegerAttribute(
    const pugi::xml_node& node,
    const char* attribute,
    bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, attribute);
    if (!value.has_value() || value->empty()) {
        recognized = false;
        return std::nullopt;
    }
    std::int32_t parsed_value = 0;
    const auto result = std::from_chars(
        value->data(), value->data() + value->size(), parsed_value, 10);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
        recognized = false;
        return std::nullopt;
    }
    return parsed_value;
}

std::optional<std::uint32_t> parseUnsignedIntegerAttribute(
    const pugi::xml_node& node,
    const char* attribute,
    bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, attribute);
    if (!value.has_value() || value->empty()) {
        recognized = false;
        return std::nullopt;
    }
    std::uint32_t parsed_value = 0;
    const auto result = std::from_chars(
        value->data(), value->data() + value->size(), parsed_value, 10);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
        recognized = false;
        return std::nullopt;
    }
    return parsed_value;
}

std::optional<std::uint32_t> parseRgb(const pugi::xml_node& node, bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, "val");
    if (!value.has_value() || value->size() != 6) {
        recognized = false;
        return std::nullopt;
    }
    std::uint32_t rgb = 0;
    const auto result = std::from_chars(value->data(), value->data() + value->size(), rgb, 16);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
        recognized = false;
        return std::nullopt;
    }
    return rgb;
}

std::optional<std::uint32_t> parseRgbAttribute(const pugi::xml_node& node,
                                               const char* attribute,
                                               bool& recognized) {
    const std::optional<std::string> value = wordAttribute(node, attribute);
    if (!value.has_value() || value->size() != 6) {
        recognized = false;
        return std::nullopt;
    }
    std::uint32_t rgb = 0;
    const auto result = std::from_chars(
        value->data(), value->data() + value->size(), rgb, 16);
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
        recognized = false;
        return std::nullopt;
    }
    return rgb;
}

template <typename T>
void overlayOptional(std::optional<T>& destination,
                     const std::optional<T>& source) {
    if (source.has_value()) destination = source;
}

void overlayRunFormat(BasicRunFormat& destination,
                      const BasicRunFormat& source) {
    overlayOptional(destination.font_family, source.font_family);
    overlayOptional(destination.font_size_half_points,
                    source.font_size_half_points);
    overlayOptional(destination.bold, source.bold);
    overlayOptional(destination.italic, source.italic);
    overlayOptional(destination.underline, source.underline);
    overlayOptional(destination.strike, source.strike);
    overlayOptional(destination.foreground_rgb, source.foreground_rgb);
    overlayOptional(destination.highlight_rgb, source.highlight_rgb);
    overlayOptional(destination.baseline, source.baseline);
}

void overlayParagraphFormat(Paragraph& destination, const Paragraph& source) {
    overlayOptional(destination.alignment, source.alignment);
    overlayOptional(destination.left_indent_twips, source.left_indent_twips);
    overlayOptional(destination.right_indent_twips, source.right_indent_twips);
    overlayOptional(destination.first_line_indent_twips,
                    source.first_line_indent_twips);
    overlayOptional(destination.space_before_twips, source.space_before_twips);
    overlayOptional(destination.space_after_twips, source.space_after_twips);
    overlayOptional(destination.line_spacing, source.line_spacing);
    overlayOptional(destination.line_spacing_rule, source.line_spacing_rule);
    overlayOptional(destination.keep_with_next, source.keep_with_next);
    overlayOptional(destination.keep_lines, source.keep_lines);
    overlayOptional(destination.page_break_before, source.page_break_before);
    overlayOptional(destination.numbering_id, source.numbering_id);
    overlayOptional(destination.numbering_level, source.numbering_level);
    if (!source.left_tab_stops_twips.empty()) {
        destination.left_tab_stops_twips = source.left_tab_stops_twips;
    }
    if (source.paragraph_mark_format) {
        if (!destination.paragraph_mark_format) {
            destination.paragraph_mark_format.emplace();
        }
        overlayRunFormat(*destination.paragraph_mark_format,
                         *source.paragraph_mark_format);
    }
}

std::optional<std::string> themeLatinFont(
    const ThemeData* theme, std::string_view token) {
    if (!theme) return std::nullopt;
    if (token == "majorHAnsi" || token == "majorAscii") {
        return theme->major_latin_font;
    }
    if (token == "minorHAnsi" || token == "minorAscii") {
        return theme->minor_latin_font;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> parseHexByte(std::string_view encoded) {
    if (encoded.size() != 2) return std::nullopt;
    unsigned int value = 0;
    const auto result = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), value, 16);
    if (result.ec != std::errc{} ||
        result.ptr != encoded.data() + encoded.size() || value > 0xffU) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

std::uint32_t applyThemeShade(std::uint32_t rgb, std::uint8_t shade) {
    std::uint32_t result = 0;
    for (unsigned int shift : {16U, 8U, 0U}) {
        const auto channel = static_cast<std::uint32_t>((rgb >> shift) & 0xffU);
        const auto adjusted = (channel * shade + 127U) / 255U;
        result |= adjusted << shift;
    }
    return result;
}

std::uint32_t applyThemeTint(std::uint32_t rgb, std::uint8_t tint) {
    std::uint32_t result = 0;
    for (unsigned int shift : {16U, 8U, 0U}) {
        const auto channel = static_cast<std::uint32_t>((rgb >> shift) & 0xffU);
        const auto adjusted =
            255U - ((255U - channel) * tint + 127U) / 255U;
        result |= adjusted << shift;
    }
    return result;
}

std::optional<std::uint32_t> effectiveThemeColor(
    const pugi::xml_node& node, const ThemeData* theme, bool& recognized) {
    std::optional<std::uint32_t> fallback;
    if (const auto encoded = wordAttribute(node, "val")) {
        if (*encoded != "auto") {
            fallback = parseRgb(node, recognized);
        }
    }

    std::optional<std::uint32_t> color;
    const auto theme_name = wordAttribute(node, "themeColor");
    if (theme_name && theme) {
        std::string key = *theme_name;
        if (key == "text1") key = "dk1";
        if (key == "background1") key = "lt1";
        if (key == "text2") key = "dk2";
        if (key == "background2") key = "lt2";
        const auto found = theme->colors.find(key);
        if (found != theme->colors.end()) color = found->second;
    }
    if (!color) color = fallback;
    if (!color && theme_name) recognized = false;

    if (const auto shade = wordAttribute(node, "themeShade")) {
        const auto parsed = parseHexByte(*shade);
        if (!parsed || !color) {
            recognized = false;
        } else {
            color = applyThemeShade(*color, *parsed);
        }
    }
    if (const auto tint = wordAttribute(node, "themeTint")) {
        const auto parsed = parseHexByte(*tint);
        if (!parsed || !color) {
            recognized = false;
        } else {
            color = applyThemeTint(*color, *parsed);
        }
    }
    return color;
}

void parseBasicRunProperties(
    const pugi::xml_node& properties,
    BasicRunFormat& format,
    bool& format_is_basic,
    const ThemeData* theme = nullptr,
    std::optional<std::string>* style_id = nullptr) {
    std::optional<std::int32_t> complex_script_size;
    for (pugi::xml_node property : properties.children()) {
        if (ignorableNode(property)) {
            continue;
        }
        if (property.type() != pugi::node_element) {
            format_is_basic = false;
            continue;
        }
        bool recognized = hasOnlyIgnorableChildren(property);
        if (isWordElement(property, "rFonts")) {
            recognized = hasOnlyWordAttributes(
                             property,
                             {"ascii", "hAnsi", "cs", "asciiTheme",
                              "hAnsiTheme", "eastAsiaTheme", "cstheme",
                              "hint"}) &&
                         recognized;
            const std::optional<std::string> ascii = wordAttribute(property, "ascii");
            const std::optional<std::string> high_ansi = wordAttribute(property, "hAnsi");
            const auto ascii_theme = wordAttribute(property, "asciiTheme");
            const auto high_ansi_theme = wordAttribute(property, "hAnsiTheme");
            const auto resolved_ascii_theme = ascii_theme
                ? themeLatinFont(theme, *ascii_theme)
                : std::nullopt;
            const auto resolved_high_ansi_theme = high_ansi_theme
                ? themeLatinFont(theme, *high_ansi_theme)
                : std::nullopt;
            if (resolved_ascii_theme) {
                format.font_family = resolved_ascii_theme;
            } else if (resolved_high_ansi_theme) {
                format.font_family = resolved_high_ansi_theme;
            } else if (ascii.has_value()) {
                format.font_family = ascii;
            } else if (high_ansi.has_value()) {
                format.font_family = high_ansi;
            } else {
                if (!format.font_family) recognized = false;
            }
            if (ascii.has_value() && high_ansi.has_value() && ascii != high_ansi) {
                recognized = false;
            }
            if (ascii_theme && high_ansi_theme &&
                resolved_ascii_theme != resolved_high_ansi_theme) {
                recognized = false;
            }
        } else if (isWordElement(property, "rStyle")) {
            recognized = style_id != nullptr &&
                         hasOnlyWordAttributes(property, {"val"}) && recognized;
            const auto value = wordAttribute(property, "val");
            if (!value || value->empty()) {
                recognized = false;
            } else if (style_id) {
                *style_id = *value;
            }
        } else if (isWordElement(property, "b")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            format.bold = onOffValue(property, recognized);
        } else if (isWordElement(property, "i")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            format.italic = onOffValue(property, recognized);
        } else if (isWordElement(property, "u")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            const std::optional<std::string> value = wordAttribute(property, "val");
            if (!value.has_value() || *value == "single") {
                format.underline = true;
            } else if (*value == "none" || *value == "0" || *value == "false") {
                format.underline = false;
            } else {
                format.underline = true;
                recognized = false;
            }
        } else if (isWordElement(property, "strike")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            format.strike = onOffValue(property, recognized);
        } else if (isWordElement(property, "color")) {
            recognized = hasOnlyWordAttributes(
                             property,
                             {"val", "themeColor", "themeTint",
                              "themeShade"}) &&
                         recognized;
            format.foreground_rgb =
                effectiveThemeColor(property, theme, recognized);
        } else if (isWordElement(property, "shd")) {
            recognized = hasOnlyWordAttributes(property, {"val", "color", "fill"}) && recognized;
            const auto value = wordAttribute(property, "val");
            if (value && *value != "clear" && *value != "solid") {
                recognized = false;
            }
            const auto pattern_color = wordAttribute(property, "color");
            if (pattern_color && *pattern_color != "auto") {
                recognized = false;
            }
            format.highlight_rgb = parseRgbAttribute(property, "fill", recognized);
        } else if (isWordElement(property, "vertAlign")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            const auto value = wordAttribute(property, "val");
            if (!value || *value == "baseline") {
                format.baseline = BasicBaseline::normal;
            } else if (*value == "superscript") {
                format.baseline = BasicBaseline::superscript;
            } else if (*value == "subscript") {
                format.baseline = BasicBaseline::subscript;
            } else {
                recognized = false;
            }
        } else if (isWordElement(property, "sz")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            format.font_size_half_points = parseHalfPoints(property, recognized);
        } else if (isWordElement(property, "szCs")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            complex_script_size = parseHalfPoints(property, recognized);
        } else {
            recognized = false;
        }
        if (!recognized) {
            format_is_basic = false;
        }
    }
    if (complex_script_size.has_value()) {
        if (format.font_size_half_points.has_value() &&
            format.font_size_half_points != complex_script_size) {
            format_is_basic = false;
        } else if (!format.font_size_half_points.has_value()) {
            format.font_size_half_points = complex_script_size;
        }
    }
}

void parseRunFormat(const pugi::xml_node& run_node, Run& run,
                    const ThemeData* theme = nullptr) {
    pugi::xml_node properties;
    for (pugi::xml_node child : run_node.children()) {
        if (isWordElement(child, "rPr")) {
            if (properties) {
                run.format_is_basic = false;
            }
            properties = child;
        }
    }
    if (properties) {
        parseBasicRunProperties(properties, run.format, run.format_is_basic,
                                theme, &run.style_id);
    }
}

std::optional<PageSettings> parsePageSettingsFromSection(
    const pugi::xml_node& section);
std::optional<SectionBreakKind> parseSectionBreakKind(
    const pugi::xml_node& section);

void parseParagraphFormat(const pugi::xml_node& paragraph_node,
                          Paragraph& paragraph,
                          const ThemeData* theme = nullptr,
                          bool allow_style_outline_level = false) {
    pugi::xml_node properties;
    for (pugi::xml_node child : paragraph_node.children()) {
        if (isWordElement(child, "pPr")) {
            if (properties) {
                paragraph.format_is_basic = false;
            }
            properties = child;
        }
    }
    if (!properties) {
        return;
    }
    for (pugi::xml_node property : properties.children()) {
        if (ignorableNode(property)) {
            continue;
        }
        if (property.type() != pugi::node_element) {
            paragraph.format_is_basic = false;
            continue;
        }
        bool recognized = hasOnlyIgnorableChildren(property);
        if (isWordElement(property, "pStyle")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            const auto value = wordAttribute(property, "val");
            if (!value || value->empty()) {
                recognized = false;
            } else {
                paragraph.style_id = *value;
            }
        } else if (isWordElement(property, "numPr")) {
            recognized = hasOnlyWordAttributes(property, {});
            bool saw_id = false;
            bool saw_level = false;
            for (const auto child : property.children()) {
                if (ignorableNode(child)) continue;
                if (isWordElement(child, "numId") && !saw_id &&
                    hasOnlyIgnorableChildren(child) &&
                    hasOnlyWordAttributes(child, {"val"})) {
                    bool parsed = true;
                    const auto value = parseSignedIntegerAttribute(
                        child, "val", parsed);
                    if (!value || *value < 0) {
                        recognized = false;
                    } else {
                        paragraph.numbering_id = *value;
                    }
                    recognized = recognized && parsed;
                    saw_id = true;
                } else if (isWordElement(child, "ilvl") && !saw_level &&
                           hasOnlyIgnorableChildren(child) &&
                           hasOnlyWordAttributes(child, {"val"})) {
                    bool parsed = true;
                    const auto value = parseUnsignedIntegerAttribute(
                        child, "val", parsed);
                    if (!value ||
                        *value > kMaximumNativeNumberingLevel) {
                        recognized = false;
                    } else {
                        paragraph.numbering_level =
                            static_cast<std::uint8_t>(*value);
                    }
                    recognized = recognized && parsed;
                    saw_level = true;
                } else {
                    recognized = false;
                }
            }
            if (!saw_id && !saw_level) recognized = false;
        } else if (isWordElement(property, "jc")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            const std::optional<std::string> value = wordAttribute(property, "val");
            if (!value.has_value()) {
                recognized = false;
            } else if (*value == "left" || *value == "start") {
                paragraph.alignment = BasicParagraphAlignment::left;
            } else if (*value == "center") {
                paragraph.alignment = BasicParagraphAlignment::center;
            } else if (*value == "right" || *value == "end") {
                paragraph.alignment = BasicParagraphAlignment::right;
            } else if (*value == "both") {
                paragraph.alignment = BasicParagraphAlignment::justified;
            } else {
                recognized = false;
            }
        } else if (isWordElement(property, "ind")) {
            recognized = hasOnlyWordAttributes(
                             property,
                             {"left", "start", "right", "end", "firstLine", "hanging"}) &&
                         recognized;
            const auto parse_alias = [&](const char* transitional, const char* logical) {
                const auto first_encoded = wordAttribute(property, transitional);
                const auto second_encoded = wordAttribute(property, logical);
                std::optional<std::int32_t> first;
                std::optional<std::int32_t> second;
                if (first_encoded) {
                    first = parseSignedIntegerAttribute(property, transitional, recognized);
                }
                if (second_encoded) {
                    second = parseSignedIntegerAttribute(property, logical, recognized);
                }
                if (first && second && *first != *second) recognized = false;
                return second ? second : first;
            };
            paragraph.left_indent_twips = parse_alias("left", "start");
            paragraph.right_indent_twips = parse_alias("right", "end");
            const auto first_line = wordAttribute(property, "firstLine");
            const auto hanging = wordAttribute(property, "hanging");
            if (first_line && hanging) {
                recognized = false;
            } else if (first_line) {
                paragraph.first_line_indent_twips =
                    parseSignedIntegerAttribute(property, "firstLine", recognized);
            } else if (hanging) {
                const auto value = parseSignedIntegerAttribute(property, "hanging", recognized);
                if (value) {
                    if (*value == std::numeric_limits<std::int32_t>::min()) {
                        recognized = false;
                    } else {
                        paragraph.first_line_indent_twips = -*value;
                    }
                }
            }
            if (!wordAttribute(property, "left") && !wordAttribute(property, "start") &&
                !wordAttribute(property, "right") && !wordAttribute(property, "end") &&
                !first_line && !hanging) {
                recognized = false;
            }
        } else if (isWordElement(property, "spacing")) {
            recognized = hasOnlyWordAttributes(
                             property, {"before", "after", "line", "lineRule"}) &&
                         recognized;
            const auto before = wordAttribute(property, "before");
            const auto after = wordAttribute(property, "after");
            const auto line = wordAttribute(property, "line");
            const auto line_rule = wordAttribute(property, "lineRule");
            if (before) {
                paragraph.space_before_twips =
                    parseUnsignedIntegerAttribute(property, "before", recognized);
            }
            if (after) {
                paragraph.space_after_twips =
                    parseUnsignedIntegerAttribute(property, "after", recognized);
            }
            if (line) {
                paragraph.line_spacing =
                    parseUnsignedIntegerAttribute(property, "line", recognized);
                if (paragraph.line_spacing && *paragraph.line_spacing == 0) recognized = false;
            }
            if (line_rule) {
                if (*line_rule == "auto") {
                    paragraph.line_spacing_rule = BasicLineSpacingRule::automatic;
                } else if (*line_rule == "atLeast") {
                    paragraph.line_spacing_rule = BasicLineSpacingRule::at_least;
                } else if (*line_rule == "exact") {
                    paragraph.line_spacing_rule = BasicLineSpacingRule::exact;
                } else {
                    recognized = false;
                }
                if (!line) recognized = false;
            } else if (line) {
                paragraph.line_spacing_rule = BasicLineSpacingRule::automatic;
            }
            if (!before && !after && !line && !line_rule) recognized = false;
        } else if (isWordElement(property, "tabs")) {
            recognized = hasOnlyWordAttributes(property, {});
            std::vector<std::uint32_t> parsed_stops;
            for (pugi::xml_node tab : property.children()) {
                if (ignorableNode(tab)) continue;
                if (!isWordElement(tab, "tab") ||
                    !hasOnlyIgnorableChildren(tab) ||
                    !hasOnlyWordAttributes(tab, {"val", "pos", "leader"})) {
                    recognized = false;
                    continue;
                }
                const auto value = wordAttribute(tab, "val");
                const auto leader = wordAttribute(tab, "leader");
                if (!value || (*value != "left" && *value != "num") ||
                    (leader && *leader != "none")) {
                    recognized = false;
                    continue;
                }
                const auto position =
                    parseUnsignedIntegerAttribute(tab, "pos", recognized);
                if (!position || *position == 0 || *position > 31680) {
                    recognized = false;
                    continue;
                }
                parsed_stops.push_back(*position);
            }
            if (parsed_stops.empty()) recognized = false;
            if (recognized) {
                std::sort(parsed_stops.begin(), parsed_stops.end());
                if (std::adjacent_find(parsed_stops.begin(), parsed_stops.end()) !=
                    parsed_stops.end()) {
                    recognized = false;
                } else {
                    paragraph.left_tab_stops_twips = std::move(parsed_stops);
                }
            }
        } else if (isWordElement(property, "keepNext")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            paragraph.keep_with_next = onOffValue(property, recognized);
        } else if (isWordElement(property, "keepLines")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            paragraph.keep_lines = onOffValue(property, recognized);
        } else if (isWordElement(property, "pageBreakBefore")) {
            recognized = hasOnlyWordAttributes(property, {"val"}) && recognized;
            paragraph.page_break_before = onOffValue(property, recognized);
        } else if (allow_style_outline_level &&
                   isWordElement(property, "outlineLvl")) {
            recognized =
                hasOnlyWordAttributes(property, {"val"}) && recognized;
            const auto level = parseUnsignedIntegerAttribute(
                property, "val", recognized);
            recognized = recognized && level.has_value() && *level <= 8U;
        } else if (isWordElement(property, "rPr")) {
            if (paragraph.paragraph_mark_format.has_value()) {
                recognized = false;
            } else {
                recognized = hasOnlyWordAttributes(property, {});
                paragraph.paragraph_mark_format.emplace();
                parseBasicRunProperties(
                    property, *paragraph.paragraph_mark_format, recognized,
                    theme);
            }
        } else if (isWordElement(property, "sectPr")) {
            // Section properties terminate this paragraph but are exposed by
            // the ordered sections() view rather than as paragraph format.
            const auto section_page = parsePageSettingsFromSection(property);
            recognized = section_page.has_value() &&
                         parseSectionBreakKind(property).has_value();
            for (const pugi::xml_node child : property.children()) {
                if (ignorableNode(child)) continue;
                if (isWordElement(child, "type")) {
                    recognized = recognized &&
                        hasOnlyIgnorableChildren(child) &&
                        hasOnlyWordAttributes(child, {"val"});
                } else if (isWordElement(child, "pgSz")) {
                    const auto orientation = wordAttribute(child, "orient");
                    const bool supported_orientation =
                        !orientation ||
                        (section_page &&
                         ((*orientation == "landscape" &&
                           section_page->width_twips >
                               section_page->height_twips) ||
                          (*orientation == "portrait" &&
                           section_page->width_twips <=
                               section_page->height_twips)));
                    recognized = recognized && supported_orientation &&
                        hasOnlyIgnorableChildren(child) &&
                        hasOnlyWordAttributes(child, {"w", "h", "orient"});
                } else if (isWordElement(child, "pgMar")) {
                    recognized = recognized &&
                        hasOnlyIgnorableChildren(child) &&
                        hasOnlyWordAttributes(
                            child,
                            {"top", "right", "bottom", "left", "header",
                             "footer", "gutter"});
                } else {
                    recognized = false;
                }
            }
        } else {
            recognized = false;
        }
        if (!recognized) paragraph.format_is_basic = false;
    }
}

std::optional<std::uint32_t> drawingColorValue(const pugi::xml_node& node) {
    for (const auto child : node.children()) {
        if (isDrawingElement(child, "srgbClr")) {
            const std::string_view encoded = child.attribute("val").value();
            if (encoded.size() != 6) return std::nullopt;
            std::uint32_t rgb = 0;
            const auto parsed = std::from_chars(
                encoded.data(), encoded.data() + encoded.size(), rgb, 16);
            if (parsed.ec == std::errc{} &&
                parsed.ptr == encoded.data() + encoded.size()) {
                return rgb;
            }
            return std::nullopt;
        }
        if (isDrawingElement(child, "sysClr")) {
            const std::string_view encoded = child.attribute("lastClr").value();
            if (encoded.size() != 6) return std::nullopt;
            std::uint32_t rgb = 0;
            const auto parsed = std::from_chars(
                encoded.data(), encoded.data() + encoded.size(), rgb, 16);
            if (parsed.ec == std::errc{} &&
                parsed.ptr == encoded.data() + encoded.size()) {
                return rgb;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

ThemeData parseThemeData(std::string_view xml, const OpenOptions& options) {
    ThemeData theme;
    if (xml.empty()) return theme;
    pugi::xml_document document;
    if (!document.load_buffer(
            xml.data(), xml.size(), pugi::parse_default,
            pugi::encoding_auto) ||
        !::docxstudio::xml::inspectComplexity(
             document, options.max_xml_depth, options.max_xml_nodes)
             .accepted()) {
        return theme;
    }
    const auto root = document.document_element();
    if (!isDrawingElement(root, "theme")) {
        return theme;
    }
    for (const auto elements : root.children()) {
        if (!isDrawingElement(elements, "themeElements")) {
            continue;
        }
        for (const auto scheme : elements.children()) {
            if (isDrawingElement(scheme, "clrScheme")) {
                for (const auto color : scheme.children()) {
                    if (color.type() != pugi::node_element ||
                        (namespaceUri(color) != kDrawingMainNamespace &&
                         namespaceUri(color) !=
                             kStrictDrawingMainNamespace)) {
                        continue;
                    }
                    if (const auto value = drawingColorValue(color)) {
                        theme.colors.emplace(
                            std::string(localName(color.name())), *value);
                    }
                }
            } else if (isDrawingElement(scheme, "fontScheme")) {
                for (const auto font_set : scheme.children()) {
                    const bool major =
                        isDrawingElement(font_set, "majorFont");
                    const bool minor =
                        isDrawingElement(font_set, "minorFont");
                    if (!major && !minor) continue;
                    for (const auto font : font_set.children()) {
                        if (!isDrawingElement(font, "latin")) {
                            continue;
                        }
                        const std::string family =
                            font.attribute("typeface").value();
                        if (family.empty()) continue;
                        if (major) {
                            theme.major_latin_font = family;
                        } else {
                            theme.minor_latin_font = family;
                        }
                    }
                }
            }
        }
    }
    return theme;
}

void parseStylesData(std::string_view xml, const OpenOptions& options,
                     ImportContext& context) {
    if (xml.empty()) return;
    pugi::xml_document document;
    if (!document.load_buffer(
            xml.data(), xml.size(), pugi::parse_default,
            pugi::encoding_auto) ||
        !::docxstudio::xml::inspectComplexity(
             document, options.max_xml_depth, options.max_xml_nodes)
             .accepted()) {
        return;
    }
    const auto root = document.document_element();
    if (!isWordElement(root, "styles")) return;

    for (const auto child : root.children()) {
        if (isWordElement(child, "docDefaults")) {
            for (const auto defaults : child.children()) {
                if (isWordElement(defaults, "rPrDefault")) {
                    Run run;
                    parseRunFormat(defaults, run, &context.theme);
                    overlayRunFormat(context.default_run_format, run.format);
                } else if (isWordElement(defaults, "pPrDefault")) {
                    Paragraph paragraph;
                    parseParagraphFormat(
                        defaults, paragraph, &context.theme);
                    overlayParagraphFormat(
                        context.default_paragraph_format, paragraph);
                }
            }
            continue;
        }
        if (!isWordElement(child, "style")) continue;
        const auto id = wordAttribute(child, "styleId");
        const auto type = wordAttribute(child, "type");
        if (!id || id->empty() || !type ||
            (*type != "paragraph" && *type != "character")) {
            continue;
        }
        StyleDefinition style;
        style.id = *id;
        style.paragraph_style = *type == "paragraph";
        for (const auto property : child.children()) {
            if (isWordElement(property, "basedOn")) {
                const auto value = wordAttribute(property, "val");
                if (value && !value->empty()) style.based_on = *value;
            }
        }
        if (const auto value = wordAttribute(child, "default")) {
            style.default_style =
                value->empty() || *value == "1" || *value == "true" ||
                *value == "on";
        }
        // Outline levels are a supported part of Owl Docs' deterministic
        // heading definitions. The current semantic paragraph surface does
        // not expose them independently, so accept them only inside a style;
        // a direct w:pPr/w:outlineLvl remains conservatively non-basic.
        parseParagraphFormat(
            child, style.paragraph, &context.theme,
            /*allow_style_outline_level=*/true);
        Run run;
        parseRunFormat(child, run, &context.theme);
        style.run_format = std::move(run.format);
        style.run_format_is_basic = run.format_is_basic;
        if (style.paragraph_style && style.default_style &&
            !context.default_paragraph_style) {
            context.default_paragraph_style = style.id;
        }
        context.styles.emplace(style.id, std::move(style));
    }
}

std::optional<BasicNumberFormat> parseNumberFormat(std::string_view value) {
    if (value == "bullet") return BasicNumberFormat::bullet;
    if (value == "decimal") return BasicNumberFormat::decimal;
    if (value == "upperLetter") return BasicNumberFormat::upper_letter;
    if (value == "lowerLetter") return BasicNumberFormat::lower_letter;
    if (value == "upperRoman") return BasicNumberFormat::upper_roman;
    if (value == "lowerRoman") return BasicNumberFormat::lower_roman;
    return std::nullopt;
}

std::optional<NumberLevelDefinition> parseNumberLevel(
    const pugi::xml_node& level, const ThemeData& theme) {
    bool recognized = true;
    const auto encoded_level = wordAttribute(level, "ilvl");
    if (!encoded_level) return std::nullopt;
    std::uint32_t level_index = 0;
    const auto parsed_level = std::from_chars(
        encoded_level->data(), encoded_level->data() + encoded_level->size(),
        level_index, 10);
    if (parsed_level.ec != std::errc{} ||
        parsed_level.ptr != encoded_level->data() + encoded_level->size() ||
        level_index > kMaximumNativeNumberingLevel) {
        return std::nullopt;
    }

    NumberLevelDefinition definition;
    definition.value.level = static_cast<std::uint8_t>(level_index);
    std::optional<BasicNumberFormat> format;
    bool saw_start = false;
    bool saw_text = false;
    bool saw_level_justification = false;
    for (const auto child : level.children()) {
        if (isWordElement(child, "start")) {
            const auto start = parseSignedIntegerAttribute(
                child, "val", recognized);
            if (start && *start >= 0) {
                definition.value.start = *start;
                saw_start = true;
            } else {
                recognized = false;
            }
        } else if (isWordElement(child, "numFmt")) {
            const auto value = wordAttribute(child, "val");
            if (value) format = parseNumberFormat(*value);
            if (!format) recognized = false;
        } else if (isWordElement(child, "lvlText")) {
            const auto value = wordAttribute(child, "val");
            if (value) {
                definition.value.level_text = *value;
                saw_text = true;
            } else {
                recognized = false;
            }
        } else if (isWordElement(child, "suff")) {
            const auto value = wordAttribute(child, "val");
            if (!value || *value == "tab") {
                definition.value.suffix = BasicNumberSuffix::tab;
            } else if (*value == "space") {
                definition.value.suffix = BasicNumberSuffix::space;
            } else if (*value == "nothing") {
                definition.value.suffix = BasicNumberSuffix::nothing;
            } else {
                recognized = false;
            }
        } else if (isWordElement(child, "lvlJc")) {
            const auto value = wordAttribute(child, "val");
            const bool supported_justification =
                !saw_level_justification &&
                hasOnlyIgnorableChildren(child) &&
                hasOnlyWordAttributes(child, {"val"}) && value &&
                *value == "left";
            recognized = recognized && supported_justification;
            saw_level_justification = true;
        } else if (isWordElement(child, "pStyle") ||
                   isWordElement(child, "pPr") ||
                   isWordElement(child, "rPr")) {
            // Parsed where relevant below, or presentation-neutral for the
            // supported marker/indent subset.
        } else {
            // Restart rules, legal-number coercion, legacy layout, and other
            // level semantics must not be silently reported as editable.
            recognized = false;
        }
    }
    if (!format || !saw_text) recognized = false;
    definition.value.format = format.value_or(BasicNumberFormat::decimal);
    if (!saw_start) definition.value.start = 1;

    Paragraph paragraph;
    parseParagraphFormat(level, paragraph, &theme);
    definition.value.left_indent_twips = paragraph.left_indent_twips;
    definition.value.first_line_indent_twips =
        paragraph.first_line_indent_twips;
    definition.value.left_tab_stops_twips =
        std::move(paragraph.left_tab_stops_twips);
    Run marker;
    parseRunFormat(level, marker, &theme);
    definition.value.marker_format = std::move(marker.format);
    definition.supported = recognized && paragraph.format_is_basic &&
                           marker.format_is_basic;
    return definition;
}

void parseNumberingData(std::string_view xml, const OpenOptions& options,
                        ImportContext& context) {
    if (xml.empty()) return;
    pugi::xml_document document;
    if (!document.load_buffer(
            xml.data(), xml.size(), pugi::parse_default,
            pugi::encoding_auto) ||
        !::docxstudio::xml::inspectComplexity(
             document, options.max_xml_depth, options.max_xml_nodes)
             .accepted()) {
        return;
    }
    const auto root = document.document_element();
    if (!isWordElement(root, "numbering")) return;
    for (const auto child : root.children()) {
        if (isWordElement(child, "abstractNum")) {
            bool recognized = true;
            const auto id = parseSignedIntegerAttribute(
                child, "abstractNumId", recognized);
            if (!id || *id < 0 || !recognized) continue;
            auto& levels = context.abstract_numbering[*id];
            for (const auto level : child.children()) {
                if (!isWordElement(level, "lvl")) continue;
                auto parsed = parseNumberLevel(level, context.theme);
                if (parsed) levels[parsed->value.level] = std::move(*parsed);
            }
        } else if (isWordElement(child, "num")) {
            bool recognized = true;
            const auto id = parseSignedIntegerAttribute(child, "numId", recognized);
            if (!id || *id <= 0 || !recognized) continue;
            NumberInstanceDefinition instance;
            bool has_abstract = false;
            for (const auto property : child.children()) {
                if (isWordElement(property, "abstractNumId")) {
                    const auto abstract_id = parseSignedIntegerAttribute(
                        property, "val", recognized);
                    if (abstract_id && *abstract_id >= 0) {
                        instance.abstract_id = *abstract_id;
                        has_abstract = true;
                    }
                } else if (isWordElement(property, "lvlOverride")) {
                    const auto encoded = wordAttribute(property, "ilvl");
                    if (!encoded) continue;
                    std::uint32_t level_index = 0;
                    const auto parsed_level = std::from_chars(
                        encoded->data(), encoded->data() + encoded->size(),
                        level_index, 10);
                    if (parsed_level.ec != std::errc{} ||
                        parsed_level.ptr != encoded->data() + encoded->size() ||
                        level_index > kMaximumNativeNumberingLevel) {
                        continue;
                    }
                    for (const auto override_value : property.children()) {
                        if (isWordElement(override_value, "startOverride")) {
                            const auto start = parseSignedIntegerAttribute(
                                override_value, "val", recognized);
                            if (start && *start >= 0) {
                                instance.start_overrides[
                                    static_cast<std::uint8_t>(level_index)] =
                                    *start;
                            }
                        } else if (isWordElement(override_value, "lvl")) {
                            auto level = parseNumberLevel(
                                override_value, context.theme);
                            if (level) {
                                instance.level_overrides[
                                    static_cast<std::uint8_t>(level_index)] =
                                    std::move(*level);
                            }
                        }
                    }
                }
            }
            if (has_abstract) context.numbering[*id] = std::move(instance);
        }
    }
}

void applyParagraphStyleChain(
    const ImportContext& context, std::string_view style_id,
    Paragraph& paragraph, BasicRunFormat& run_format, bool& supported,
    std::unordered_set<std::string>& visiting,
    std::size_t depth = 0) {
    if (depth >= kMaximumStyleChainDepth) {
        supported = false;
        return;
    }
    const auto found = context.styles.find(std::string(style_id));
    if (found == context.styles.end() || !found->second.paragraph_style ||
        !visiting.insert(found->first).second) {
        supported = false;
        return;
    }
    const auto& style = found->second;
    if (style.based_on) {
        applyParagraphStyleChain(
            context, *style.based_on, paragraph, run_format, supported,
            visiting, depth + 1);
    }
    overlayParagraphFormat(paragraph, style.paragraph);
    overlayRunFormat(run_format, style.run_format);
    supported = supported && style.paragraph.format_is_basic &&
                style.run_format_is_basic;
    visiting.erase(found->first);
}

void applyRunStyleChain(
    const ImportContext& context, std::string_view style_id,
    BasicRunFormat& run_format, bool& supported,
    std::unordered_set<std::string>& visiting,
    std::size_t depth = 0) {
    if (depth >= kMaximumStyleChainDepth) {
        supported = false;
        return;
    }
    const auto found = context.styles.find(std::string(style_id));
    if (found == context.styles.end() || found->second.paragraph_style ||
        !visiting.insert(found->first).second) {
        supported = false;
        return;
    }
    const auto& style = found->second;
    if (style.based_on) {
        applyRunStyleChain(
            context, *style.based_on, run_format, supported, visiting,
            depth + 1);
    }
    overlayRunFormat(run_format, style.run_format);
    supported = supported && style.run_format_is_basic;
    visiting.erase(found->first);
}

const NumberLevelDefinition* numberLevel(
    const ImportContext& context, std::int32_t num_id, std::uint8_t level) {
    const auto instance = context.numbering.find(num_id);
    if (instance == context.numbering.end()) return nullptr;
    const auto overridden = instance->second.level_overrides.find(level);
    if (overridden != instance->second.level_overrides.end()) {
        return &overridden->second;
    }
    const auto abstract = context.abstract_numbering.find(
        instance->second.abstract_id);
    if (abstract == context.abstract_numbering.end()) return nullptr;
    const auto found = abstract->second.find(level);
    return found == abstract->second.end() ? nullptr : &found->second;
}

std::int32_t numberLevelStart(const ImportContext& context,
                              std::int32_t num_id, std::uint8_t level,
                              const NumberLevelDefinition& definition) {
    const auto instance = context.numbering.find(num_id);
    if (instance != context.numbering.end()) {
        const auto override = instance->second.start_overrides.find(level);
        if (override != instance->second.start_overrides.end()) {
            return override->second;
        }
    }
    return definition.value.start;
}

std::string alphabeticNumber(std::int32_t value, bool upper) {
    if (value <= 0) return std::to_string(value);
    std::string result;
    while (value > 0) {
        --value;
        result.push_back(static_cast<char>((upper ? 'A' : 'a') + value % 26));
        value /= 26;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::string romanNumber(std::int32_t value, bool upper) {
    if (value <= 0 || value > 3999) return std::to_string(value);
    static constexpr std::pair<int, std::string_view> numerals[]{
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
        {100, "C"},  {90, "XC"},  {50, "L"}, {40, "XL"},
        {10, "X"},   {9, "IX"},   {5, "V"},  {4, "IV"},
        {1, "I"},
    };
    std::string result;
    for (const auto& [amount, token] : numerals) {
        while (value >= amount) {
            result += token;
            value -= amount;
        }
    }
    if (!upper) {
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
    }
    return result;
}

std::string formattedNumber(std::int32_t value, BasicNumberFormat format) {
    switch (format) {
        case BasicNumberFormat::decimal: return std::to_string(value);
        case BasicNumberFormat::upper_letter:
            return alphabeticNumber(value, true);
        case BasicNumberFormat::lower_letter:
            return alphabeticNumber(value, false);
        case BasicNumberFormat::upper_roman:
            return romanNumber(value, true);
        case BasicNumberFormat::lower_roman:
            return romanNumber(value, false);
        case BasicNumberFormat::bullet: return "\xe2\x80\xa2";
    }
    return std::to_string(value);
}

void assignNumberingMarkers(std::vector<Paragraph>& paragraphs,
                            const ImportContext& context) {
    struct CounterState {
        std::array<std::int32_t, kNativeNumberingLevelCount> values{};
        std::array<bool, kNativeNumberingLevelCount> initialized{};
    };
    std::unordered_map<std::int32_t, CounterState> counters;
    for (auto& paragraph : paragraphs) {
        if (!paragraph.numbering_id || !paragraph.numbering_level ||
            *paragraph.numbering_id <= 0) {
            continue;
        }
        const auto* definition = numberLevel(
            context, *paragraph.numbering_id, *paragraph.numbering_level);
        if (!definition || !definition->supported) {
            paragraph.format_is_basic = false;
            continue;
        }
        paragraph.numbering = definition->value;
        paragraph.numbering->num_id = *paragraph.numbering_id;
        paragraph.numbering->level = *paragraph.numbering_level;
        paragraph.numbering->start = numberLevelStart(
            context, *paragraph.numbering_id, *paragraph.numbering_level,
            *definition);

        auto& state = counters[*paragraph.numbering_id];
        const auto level = static_cast<std::size_t>(*paragraph.numbering_level);
        if (!state.initialized[level]) {
            state.values[level] = paragraph.numbering->start;
            state.initialized[level] = true;
        } else {
            ++state.values[level];
        }
        for (std::size_t deeper = level + 1; deeper < state.values.size();
             ++deeper) {
            state.initialized[deeper] = false;
        }

        std::string marker = paragraph.numbering->level_text;
        if (paragraph.numbering->format == BasicNumberFormat::bullet) {
            const std::string& source_marker =
                paragraph.numbering->level_text;
            static const std::set<std::string> supported_bullets{
                "\xe2\x80\xa2", "\xe2\x97\xa6", "\xe2\x96\xaa",
                "\xe2\x80\xa3", "*", "-"};
            marker = supported_bullets.contains(source_marker)
                ? source_marker
                : "\xe2\x80\xa2";
        } else {
            for (std::size_t placeholder_count = state.values.size();
                 placeholder_count > 0; --placeholder_count) {
                const std::size_t placeholder_level = placeholder_count - 1;
                const std::string placeholder =
                    "%" + std::to_string(placeholder_level + 1);
                std::size_t position = 0;
                while ((position = marker.find(placeholder, position)) !=
                       std::string::npos) {
                    const auto* placeholder_definition = numberLevel(
                        context, *paragraph.numbering_id,
                        static_cast<std::uint8_t>(placeholder_level));
                    const auto format = placeholder_definition
                        ? placeholder_definition->value.format
                        : BasicNumberFormat::decimal;
                    const auto value = state.initialized[placeholder_level]
                        ? state.values[placeholder_level]
                        : (placeholder_definition
                               ? numberLevelStart(
                                     context, *paragraph.numbering_id,
                                     static_cast<std::uint8_t>(placeholder_level),
                                     *placeholder_definition)
                               : 1);
                    const std::string replacement =
                        formattedNumber(value, format);
                    marker.replace(position, placeholder.size(), replacement);
                    position += replacement.size();
                }
            }
        }
        paragraph.numbering->marker_text = std::move(marker);
    }
}

std::optional<std::int64_t> parsePositiveInt64Attribute(
    const pugi::xml_node& node, const char* attribute_name,
    std::int64_t maximum = kMaximumInlineExtentEmu) {
    const std::string_view encoded = node.attribute(attribute_name).value();
    if (encoded.empty()) return std::nullopt;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), value, 10);
    // 100 metres is far beyond a useful page object but still leaves ample
    // room for unusual OOXML fixtures. Bounding geometry here prevents later
    // conversions from overflowing.
    if (parsed.ec != std::errc{} ||
        parsed.ptr != encoded.data() + encoded.size() || value <= 0 ||
        value > maximum) {
        return std::nullopt;
    }
    return value;
}

bool parseBoundedDistanceAttribute(
    const pugi::xml_node& node, const char* attribute_name,
    std::int64_t& value) {
    const pugi::xml_attribute attribute = node.attribute(attribute_name);
    if (!attribute) {
        value = 0;
        return true;
    }
    const std::string_view encoded = attribute.value();
    std::uint64_t parsed_value = 0;
    const auto parsed = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), parsed_value, 10);
    if (encoded.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != encoded.data() + encoded.size() ||
        parsed_value > static_cast<std::uint64_t>(
                           kMaximumImageWrapDistanceEmu)) {
        return false;
    }
    value = static_cast<std::int64_t>(parsed_value);
    return true;
}

bool parseBooleanAttribute(const pugi::xml_node& node, const char* name,
                           bool& value) {
    const pugi::xml_attribute attribute = node.attribute(name);
    if (!attribute) return false;
    const std::string_view encoded = attribute.value();
    if (encoded == "1" || encoded == "true") {
        value = true;
        return true;
    }
    if (encoded == "0" || encoded == "false") {
        value = false;
        return true;
    }
    return false;
}

bool parseExactInteger(std::string_view encoded, std::int64_t expected) {
    if (encoded.empty()) return false;
    std::int64_t value = 0;
    const auto parsed = std::from_chars(
        encoded.data(), encoded.data() + encoded.size(), value, 10);
    return parsed.ec == std::errc{} &&
        parsed.ptr == encoded.data() + encoded.size() && value == expected;
}

bool namespaceDeclaration(const pugi::xml_attribute& attribute) {
    const std::string_view name = attribute.name();
    return name == "xmlns" || name.starts_with("xmlns:");
}

bool hasOnlyAttributes(
    const pugi::xml_node& node,
    std::initializer_list<std::string_view> accepted) {
    for (const pugi::xml_attribute attribute : node.attributes()) {
        if (namespaceDeclaration(attribute)) continue;
        if (std::find(accepted.begin(), accepted.end(), attribute.name()) ==
            accepted.end()) {
            return false;
        }
    }
    return true;
}

bool hasNoElementChildren(const pugi::xml_node& node) {
    return std::none_of(
        node.begin(), node.end(), [](const pugi::xml_node& child) {
            return child.type() == pugi::node_element;
        });
}

bool parseCanonicalPosition(
    const pugi::xml_node& position, std::string& relative_from) {
    if (!hasOnlyAttributes(position, {"relativeFrom"})) return false;
    const pugi::xml_attribute relative = position.attribute("relativeFrom");
    if (!relative || std::string_view(relative.value()).empty()) return false;
    relative_from = relative.value();

    pugi::xml_node offset;
    for (const pugi::xml_node child : position.children()) {
        if (ignorableNode(child)) continue;
        if (!isNamespacedElement(
                child, "posOffset", kWordprocessingDrawingNamespace) ||
            offset) {
            return false;
        }
        offset = child;
    }
    return offset && hasOnlyAttributes(offset, {}) &&
        hasNoElementChildren(offset) &&
        parseExactInteger(offset.text().get(), 0);
}

bool canonicalGraphicFrameProperties(
    const pugi::xml_node& properties) {
    if (!hasOnlyAttributes(properties, {})) return false;
    pugi::xml_node locks;
    for (const pugi::xml_node child : properties.children()) {
        if (ignorableNode(child)) continue;
        if (!isDrawingElement(child, "graphicFrameLocks") || locks) {
            return false;
        }
        locks = child;
    }
    if (!locks || !hasOnlyAttributes(locks, {"noChangeAspect"}) ||
        !hasNoElementChildren(locks)) {
        return false;
    }
    bool no_change_aspect = false;
    return parseBooleanAttribute(
               locks, "noChangeAspect", no_change_aspect) &&
        no_change_aspect;
}

bool canonicalAnchorPictureTree(
    const pugi::xml_node& graphic, std::int64_t width,
    std::int64_t height) {
    std::size_t graphic_data_count = 0;
    std::size_t blip_count = 0;
    const auto visit = [&](const auto& self,
                           const pugi::xml_node& node) -> bool {
        for (const pugi::xml_node child : node.children()) {
            if (ignorableNode(child)) continue;
            if (child.type() != pugi::node_element) return false;
            const std::string uri = namespaceUri(child);
            const std::string_view local = localName(child.name());
            if (uri == kDrawingMainNamespace ||
                uri == kStrictDrawingMainNamespace) {
                static constexpr std::array<std::string_view, 10>
                    accepted_drawing_elements{
                        "graphic", "graphicData", "blip", "stretch",
                        "fillRect", "xfrm", "off", "ext", "prstGeom",
                        "avLst"};
                if (std::find(
                        accepted_drawing_elements.begin(),
                        accepted_drawing_elements.end(), local) ==
                    accepted_drawing_elements.end()) {
                    return false;
                }
                if (local == "graphicData") {
                    ++graphic_data_count;
                    if (!hasOnlyAttributes(child, {"uri"}) ||
                        std::string_view(child.attribute("uri").value()) !=
                            kDrawingPictureNamespace) {
                        return false;
                    }
                } else if (local == "blip") {
                    ++blip_count;
                    const auto relationship = namespacedAttribute(
                        child, "embed",
                        {kOfficeRelationshipsNamespace,
                         kStrictOfficeRelationshipsNamespace});
                    if (!relationship || relationship->empty()) return false;
                    for (const pugi::xml_attribute attribute :
                         child.attributes()) {
                        if (namespaceDeclaration(attribute)) continue;
                        if (localName(attribute.name()) != "embed" ||
                            !namespacedAttribute(
                                child, "embed",
                                {kOfficeRelationshipsNamespace,
                                 kStrictOfficeRelationshipsNamespace})) {
                            return false;
                        }
                    }
                } else if (local == "off") {
                    if (!hasOnlyAttributes(child, {"x", "y"}) ||
                        !parseExactInteger(child.attribute("x").value(), 0) ||
                        !parseExactInteger(child.attribute("y").value(), 0)) {
                        return false;
                    }
                } else if (local == "ext") {
                    if (!hasOnlyAttributes(child, {"cx", "cy"}) ||
                        !parseExactInteger(
                            child.attribute("cx").value(), width) ||
                        !parseExactInteger(
                            child.attribute("cy").value(), height)) {
                        return false;
                    }
                } else if (local == "prstGeom") {
                    if (!hasOnlyAttributes(child, {"prst"}) ||
                        std::string_view(child.attribute("prst").value()) !=
                            "rect") {
                        return false;
                    }
                } else if (!hasOnlyAttributes(child, {})) {
                    return false;
                }
            } else if (uri == kDrawingPictureNamespace) {
                static constexpr std::array<std::string_view, 7>
                    accepted_picture_elements{
                        "pic", "nvPicPr", "cNvPr", "cNvPicPr",
                        "blipFill", "spPr", "style"};
                if (std::find(
                        accepted_picture_elements.begin(),
                        accepted_picture_elements.end(), local) ==
                    accepted_picture_elements.end()) {
                    return false;
                }
                if (local == "cNvPr") {
                    if (!hasOnlyAttributes(child, {"id", "name"})) {
                        return false;
                    }
                } else if (!hasOnlyAttributes(child, {})) {
                    return false;
                }
            } else {
                return false;
            }
            if (!self(self, child)) return false;
        }
        return true;
    };
    return visit(visit, graphic) && graphic_data_count == 1U &&
        blip_count == 1U;
}

bool parseCanonicalAnchor(
    const pugi::xml_node& anchor, ImageLayout& layout,
    pugi::xml_node& extent, pugi::xml_node& doc_properties,
    pugi::xml_node& graphic) {
    static constexpr std::array<std::string_view, 10> anchor_attributes{
        "distT", "distB", "distL", "distR", "simplePos",
        "relativeHeight", "behindDoc", "locked", "layoutInCell",
        "allowOverlap"};
    if (!hasOnlyAttributes(
            anchor,
            {anchor_attributes[0], anchor_attributes[1],
             anchor_attributes[2], anchor_attributes[3],
             anchor_attributes[4], anchor_attributes[5],
             anchor_attributes[6], anchor_attributes[7],
             anchor_attributes[8], anchor_attributes[9]}) ||
        !parseBoundedDistanceAttribute(
            anchor, "distT", layout.distance_top_emu) ||
        !parseBoundedDistanceAttribute(
            anchor, "distR", layout.distance_right_emu) ||
        !parseBoundedDistanceAttribute(
            anchor, "distB", layout.distance_bottom_emu) ||
        !parseBoundedDistanceAttribute(
            anchor, "distL", layout.distance_left_emu)) {
        return false;
    }
    bool simple_position = true;
    bool behind_document = true;
    bool locked = true;
    bool layout_in_cell = false;
    bool allow_overlap = true;
    if (!parseBooleanAttribute(anchor, "simplePos", simple_position) ||
        simple_position ||
        !parseExactInteger(
            anchor.attribute("relativeHeight").value(), 0) ||
        !parseBooleanAttribute(anchor, "behindDoc", behind_document) ||
        behind_document ||
        !parseBooleanAttribute(anchor, "locked", locked) || locked ||
        !parseBooleanAttribute(anchor, "layoutInCell", layout_in_cell) ||
        !layout_in_cell ||
        !parseBooleanAttribute(anchor, "allowOverlap", allow_overlap) ||
        allow_overlap) {
        return false;
    }

    pugi::xml_node simple_position_node;
    pugi::xml_node horizontal_position;
    pugi::xml_node vertical_position;
    pugi::xml_node wrap;
    pugi::xml_node frame_properties;
    for (const pugi::xml_node child : anchor.children()) {
        if (ignorableNode(child)) continue;
        if (child.type() != pugi::node_element) return false;
        if (isNamespacedElement(
                child, "simplePos", kWordprocessingDrawingNamespace)) {
            if (simple_position_node) return false;
            simple_position_node = child;
        } else if (isNamespacedElement(
                       child, "positionH",
                       kWordprocessingDrawingNamespace)) {
            if (horizontal_position) return false;
            horizontal_position = child;
        } else if (isNamespacedElement(
                       child, "positionV",
                       kWordprocessingDrawingNamespace)) {
            if (vertical_position) return false;
            vertical_position = child;
        } else if (isNamespacedElement(
                       child, "extent", kWordprocessingDrawingNamespace)) {
            if (extent) return false;
            extent = child;
        } else if (isNamespacedElement(
                       child, "effectExtent",
                       kWordprocessingDrawingNamespace)) {
            if (!hasOnlyAttributes(child, {"l", "t", "r", "b"}) ||
                !hasNoElementChildren(child) ||
                !parseExactInteger(child.attribute("l").value(), 0) ||
                !parseExactInteger(child.attribute("t").value(), 0) ||
                !parseExactInteger(child.attribute("r").value(), 0) ||
                !parseExactInteger(child.attribute("b").value(), 0)) {
                return false;
            }
        } else if (isNamespacedElement(
                       child, "wrapSquare",
                       kWordprocessingDrawingNamespace)) {
            if (wrap || !hasOnlyAttributes(child, {"wrapText"}) ||
                std::string_view(child.attribute("wrapText").value()) !=
                    "bothSides" ||
                !hasNoElementChildren(child)) {
                return false;
            }
            layout.placement = ImagePlacement::square;
            wrap = child;
        } else if (isNamespacedElement(
                       child, "wrapTopAndBottom",
                       kWordprocessingDrawingNamespace)) {
            if (wrap || !hasOnlyAttributes(child, {}) ||
                !hasNoElementChildren(child)) {
                return false;
            }
            layout.placement = ImagePlacement::top_and_bottom;
            wrap = child;
        } else if (isNamespacedElement(
                       child, "docPr", kWordprocessingDrawingNamespace)) {
            if (doc_properties ||
                !hasOnlyAttributes(child, {"id", "name", "descr"}) ||
                !child.attribute("id") || !child.attribute("name") ||
                !hasNoElementChildren(child)) {
                return false;
            }
            doc_properties = child;
        } else if (isNamespacedElement(
                       child, "cNvGraphicFramePr",
                       kWordprocessingDrawingNamespace)) {
            if (frame_properties ||
                !canonicalGraphicFrameProperties(child)) {
                return false;
            }
            frame_properties = child;
        } else if (isDrawingElement(child, "graphic")) {
            if (graphic) return false;
            graphic = child;
        } else {
            return false;
        }
    }
    if (!simple_position_node || !horizontal_position ||
        !vertical_position || !extent || !wrap || !doc_properties ||
        !graphic ||
        !hasOnlyAttributes(simple_position_node, {"x", "y"}) ||
        !hasNoElementChildren(simple_position_node) ||
        !parseExactInteger(
            simple_position_node.attribute("x").value(), 0) ||
        !parseExactInteger(
            simple_position_node.attribute("y").value(), 0)) {
        return false;
    }
    std::string horizontal_relative;
    std::string vertical_relative;
    if (!parseCanonicalPosition(
            horizontal_position, horizontal_relative) ||
        !parseCanonicalPosition(vertical_position, vertical_relative)) {
        return false;
    }
    if (horizontal_relative == "character" &&
        vertical_relative == "paragraph") {
        layout.move_with_text = true;
    } else if (horizontal_relative == "page" &&
               vertical_relative == "page") {
        layout.move_with_text = false;
    } else {
        return false;
    }
    return true;
}

void collectDescendantsNamed(
    const pugi::xml_node& node, std::string_view local_name,
    std::string_view namespace_uri, std::vector<pugi::xml_node>& matches) {
    for (const pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) continue;
        if (isNamespacedElement(child, local_name, namespace_uri)) {
            matches.push_back(child);
        }
        collectDescendantsNamed(child, local_name, namespace_uri, matches);
    }
}

std::optional<InlineImagePayload> parseInlineImage(
    const pugi::xml_node& drawing) {
    pugi::xml_node drawing_container;
    bool anchored = false;
    for (const pugi::xml_node child : drawing.children()) {
        if (ignorableNode(child)) continue;
        const bool inline_picture = isNamespacedElement(
            child, "inline", kWordprocessingDrawingNamespace);
        const bool anchored_picture = isNamespacedElement(
            child, "anchor", kWordprocessingDrawingNamespace);
        if ((!inline_picture && !anchored_picture) || drawing_container) {
            return std::nullopt;
        }
        drawing_container = child;
        anchored = anchored_picture;
    }
    if (!drawing_container) return std::nullopt;

    pugi::xml_node extent;
    pugi::xml_node doc_properties;
    pugi::xml_node graphic;
    ImageLayout layout;
    if (anchored) {
        if (!parseCanonicalAnchor(
                drawing_container, layout, extent, doc_properties,
                graphic)) {
            return std::nullopt;
        }
    } else {
        if (!parseBoundedDistanceAttribute(
                drawing_container, "distT", layout.distance_top_emu) ||
            !parseBoundedDistanceAttribute(
                drawing_container, "distR", layout.distance_right_emu) ||
            !parseBoundedDistanceAttribute(
                drawing_container, "distB", layout.distance_bottom_emu) ||
            !parseBoundedDistanceAttribute(
                drawing_container, "distL", layout.distance_left_emu)) {
            return std::nullopt;
        }
        for (const pugi::xml_node child : drawing_container.children()) {
            if (isNamespacedElement(
                    child, "extent", kWordprocessingDrawingNamespace)) {
                if (extent) return std::nullopt;
                extent = child;
            } else if (isNamespacedElement(
                           child, "docPr",
                           kWordprocessingDrawingNamespace)) {
                if (doc_properties) return std::nullopt;
                doc_properties = child;
            } else if (isDrawingElement(child, "graphic")) {
                if (graphic) return std::nullopt;
                graphic = child;
            }
        }
    }
    if (!extent) return std::nullopt;
    const std::int64_t maximum_extent = anchored
        ? kMaximumImageDimensionEmu
        : kMaximumInlineExtentEmu;
    const auto width =
        parsePositiveInt64Attribute(extent, "cx", maximum_extent);
    const auto height =
        parsePositiveInt64Attribute(extent, "cy", maximum_extent);
    if (!width || !height) return std::nullopt;

    if (anchored &&
        !canonicalAnchorPictureTree(graphic, *width, *height)) {
        return std::nullopt;
    }

    std::vector<pugi::xml_node> blips;
    collectDescendantsNamed(
        drawing_container, "blip", kDrawingMainNamespace, blips);
    if (blips.size() != 1) return std::nullopt;
    const auto relationship_id = namespacedAttribute(
        blips.front(), "embed",
        {kOfficeRelationshipsNamespace, kStrictOfficeRelationshipsNamespace});
    if (!relationship_id || relationship_id->empty()) return std::nullopt;

    std::string name;
    std::string accessible_name;
    if (doc_properties) {
        name = doc_properties.attribute("name").value();
        const pugi::xml_attribute description =
            doc_properties.attribute("descr");
        accessible_name = description ? description.value() : name;
    }
    if (accessible_name.size() > kMaximumImageAccessibleNameBytes) {
        return std::nullopt;
    }
    InlineImagePayload result;
    result.relationship_id = *relationship_id;
    result.name = std::move(name);
    result.width_emu = *width;
    result.height_emu = *height;
    result.accessible_name = std::move(accessible_name);
    result.layout = layout;
    return result;
}

void collectRunTokens(
    const pugi::xml_node& node,
    const pugi::xml_node& run_node,
    const pugi::xml_node& paragraph_node,
    bool utf8_source,
    bool field_sensitive,
    std::size_t paragraph_index,
    std::size_t run_index,
    std::string_view document_xml,
    Run& run,
    std::vector<SpanLocation>& spans,
    std::vector<CompatibilityIssue>& issues,
    std::set<std::pair<IssueCode, std::string>>& seen_issues,
    TextSpanId& next_span_id) {
    for (pugi::xml_node child : node.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }

        if (isWordElement(child, "t")) {
            RunFragment fragment;
            fragment.kind = FragmentKind::text;
            fragment.text = child.text().get();
            fragment.text_span_id = next_span_id;

            SpanLocation location;
            location.id = next_span_id++;
            location.paragraph_index = paragraph_index;
            location.run_index = run_index;
            location.fragment_index = run.fragments.size();
            location.original_text = fragment.text;
            location.preserves_space = std::string_view(child.attribute("xml:space").value()) == "preserve";

            const bool direct_context = child.parent() == run_node && run_node.parent() == paragraph_node;
            const bool revision_context = hasWordAncestorBetween(
                child,
                paragraph_node,
                {"ins", "del", "moveFrom", "moveTo", "fldSimple", "sdt", "hyperlink",
                 "smartTag", "customXml"});
            std::string mapping_reason;
            const bool mapped = utf8_source && mapTextContent(
                                                   child,
                                                   document_xml,
                                                   location.content_begin,
                                                   location.content_end,
                                                   mapping_reason);
            location.editable = mapped && direct_context && !revision_context && !field_sensitive;
            fragment.editable = location.editable;
            if (!location.editable) {
                if (!utf8_source) {
                    location.refusal_reason = "Text patching requires UTF-8 document.xml source bytes";
                } else if (!direct_context || revision_context) {
                    location.refusal_reason =
                        "Text is nested in a field, revision, hyperlink, content control, or other non-basic context";
                } else if (field_sensitive) {
                    location.refusal_reason =
                        "The paragraph contains field/object instructions whose displayed result is not safe to patch";
                } else {
                    location.refusal_reason = std::move(mapping_reason);
                }
                appendIssueOnce(
                    issues,
                    seen_issues,
                    CompatibilityIssue{
                        IssueSeverity::warning,
                        IssueCode::unsupported_text_context,
                        std::string(kDocumentPart),
                        location.refusal_reason,
                        paragraph_index});
            }
            spans.push_back(location);
            run.fragments.push_back(std::move(fragment));
            continue;
        }
        if (isWordElement(child, "tab")) {
            run.fragments.push_back(RunFragment{FragmentKind::tab, "", std::nullopt, false});
            continue;
        }
        if (isWordElement(child, "br")) {
            const auto type = wordAttribute(child, "type");
            const auto kind = type && *type == "page"
                ? FragmentKind::page_break
                : FragmentKind::line_break;
            run.fragments.push_back(
                RunFragment{kind, "", std::nullopt, false});
            continue;
        }
        if (isWordElement(child, "cr")) {
            run.fragments.push_back(
                RunFragment{FragmentKind::line_break, "", std::nullopt, false});
            continue;
        }
        if (isWordElement(child, "drawing")) {
            if (auto image = parseInlineImage(child)) {
                run.fragments.push_back(RunFragment{
                    FragmentKind::inline_image, "", std::nullopt, false,
                    std::nullopt, std::move(image)});
            }
            continue;
        }
        collectRunTokens(
            child,
            run_node,
            paragraph_node,
            utf8_source,
            field_sensitive,
            paragraph_index,
            run_index,
            document_xml,
            run,
            spans,
            issues,
            seen_issues,
            next_span_id);
    }
}

std::optional<std::uint32_t> parseUnsignedTwips(const pugi::xml_node& node,
                                                const char* attribute) {
    const auto encoded = wordAttribute(node, attribute);
    if (!encoded || encoded->empty()) return std::nullopt;
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(
        encoded->data(), encoded->data() + encoded->size(), value, 10);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != encoded->data() + encoded->size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<PageSettings> parsePageSettingsFromSection(
    const pugi::xml_node& section) {
    if (!isWordElement(section, "sectPr")) return std::nullopt;
    PageSettings page;
    bool found = false;
    for (pugi::xml_node child : section.children()) {
        if (isWordElement(child, "pgSz")) {
            const auto width = parseUnsignedTwips(child, "w");
            const auto height = parseUnsignedTwips(child, "h");
            if (!width || !height) return std::nullopt;
            page.width_twips = *width;
            page.height_twips = *height;
            found = true;
        } else if (isWordElement(child, "pgMar")) {
            const auto top = parseUnsignedTwips(child, "top");
            const auto right = parseUnsignedTwips(child, "right");
            const auto bottom = parseUnsignedTwips(child, "bottom");
            const auto left = parseUnsignedTwips(child, "left");
            if (!top || !right || !bottom || !left) return std::nullopt;
            page.margin_top_twips = *top;
            page.margin_right_twips = *right;
            page.margin_bottom_twips = *bottom;
            page.margin_left_twips = *left;
            found = true;
        }
    }
    const bool valid =
        page.width_twips >= 1440 && page.height_twips >= 1440 &&
        page.width_twips <= 63360 && page.height_twips <= 63360 &&
        static_cast<std::uint64_t>(page.margin_top_twips) + page.margin_bottom_twips <
            page.height_twips &&
        static_cast<std::uint64_t>(page.margin_left_twips) + page.margin_right_twips <
            page.width_twips;
    return found && valid ? std::optional<PageSettings>(page) : std::nullopt;
}

std::optional<SectionBreakKind> parseSectionBreakKind(
    const pugi::xml_node& section) {
    for (const pugi::xml_node child : section.children()) {
        if (!isWordElement(child, "type")) continue;
        const auto value = wordAttribute(child, "val");
        if (!value || *value == "nextPage") {
            return SectionBreakKind::next_page;
        }
        if (*value == "continuous") return SectionBreakKind::continuous;
        if (*value == "evenPage") return SectionBreakKind::even_page;
        if (*value == "oddPage") return SectionBreakKind::odd_page;
        return std::nullopt;
    }
    // CT_SectPr defaults to a next-page section when w:type is absent.
    return SectionBreakKind::next_page;
}

pugi::xml_node paragraphSectionProperties(const pugi::xml_node& paragraph) {
    for (const pugi::xml_node child : paragraph.children()) {
        if (!isWordElement(child, "pPr")) continue;
        for (const pugi::xml_node property : child.children()) {
            if (isWordElement(property, "sectPr")) return property;
        }
    }
    return {};
}

std::vector<ImportedSection> parseBodySections(
    std::string_view document_xml) {
    pugi::xml_document document;
    if (!document.load_buffer(document_xml.data(), document_xml.size(),
                              pugi::parse_default, pugi::encoding_auto)) {
        return {};
    }
    pugi::xml_node body;
    for (const pugi::xml_node child : document.document_element().children()) {
        if (isWordElement(child, "body")) {
            body = child;
            break;
        }
    }
    if (!body) return {};

    std::vector<ImportedSection> sections;
    std::size_t first_block = 0;
    std::size_t block_count = 0;
    bool saw_body_section = false;
    for (const pugi::xml_node child : body.children()) {
        if (ignorableNode(child)) continue;
        pugi::xml_node section;
        if (isWordElement(child, "sectPr")) {
            if (saw_body_section) return {};
            saw_body_section = true;
            section = child;
        } else {
            if (saw_body_section) return {};
            ++block_count;
            if (isWordElement(child, "p")) {
                section = paragraphSectionProperties(child);
            }
        }
        if (!section) continue;
        const auto page = parsePageSettingsFromSection(section);
        const auto break_kind = parseSectionBreakKind(section);
        if (!page || !break_kind) return {};
        sections.push_back(ImportedSection{
            first_block, block_count - first_block, *page, *break_kind});
        first_block = block_count;
    }
    if (!saw_body_section || first_block != block_count) return {};
    return sections;
}

std::optional<PageSettings> parseBodyPageSettings(std::string_view document_xml) {
    const auto sections = parseBodySections(document_xml);
    if (sections.empty()) return std::nullopt;
    return sections.back().page;
}

struct XmlNodeHash {
    std::size_t operator()(const pugi::xml_node& node) const noexcept {
        return node.hash_value();
    }
};

using ParagraphIndexByNode =
    std::unordered_map<pugi::xml_node, std::size_t, XmlNodeHash>;

std::vector<std::size_t> descendantParagraphIndices(
    const pugi::xml_node& node,
    const ParagraphIndexByNode& paragraph_indices) {
    std::vector<pugi::xml_node> nodes;
    collectParagraphNodes(node, nodes);
    std::vector<std::size_t> indices;
    indices.reserve(nodes.size());
    for (const auto& paragraph : nodes) {
        const auto found = paragraph_indices.find(paragraph);
        if (found != paragraph_indices.end()) {
            indices.push_back(found->second);
        }
    }
    return indices;
}

bool parseBooleanLexical(std::string_view value, bool& parsed) {
    if (value == "1" || value == "true" || value == "on") {
        parsed = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off") {
        parsed = false;
        return true;
    }
    return false;
}

bool paragraphContainsFragment(
    const Paragraph& paragraph, FragmentKind kind) {
    for (const auto& run : paragraph.runs) {
        if (std::any_of(
                run.fragments.begin(), run.fragments.end(),
                [kind](const RunFragment& fragment) {
                    return fragment.kind == kind;
                })) {
            return true;
        }
    }
    return false;
}

std::optional<ImportedTableBlock> parseSimpleImportedTable(
    const pugi::xml_node& table_node,
    const ParagraphIndexByNode& paragraph_indices,
    const std::vector<Paragraph>& paragraphs,
    const ThemeData* theme,
    std::string& reason) {
    std::vector<pugi::xml_node> rows;
    std::optional<std::size_t> declared_columns;
    std::vector<std::uint32_t> declared_column_widths;
    std::optional<BasicParagraphAlignment> table_alignment;
    std::optional<std::string> table_style_id;
    bool look_first_row = false;
    bool saw_table_properties = false;
    bool saw_table_grid = false;

    for (const auto& child : table_node.children()) {
        if (ignorableNode(child)) continue;
        if (isWordElement(child, "tblPr")) {
            if (saw_table_properties) {
                reason = "the table has more than one w:tblPr element";
                return std::nullopt;
            }
            saw_table_properties = true;
            for (const auto& property : child.children()) {
                if (isWordElement(property, "tblStyle")) {
                    if (table_style_id.has_value()) {
                        reason = "the table has more than one w:tblStyle element";
                        return std::nullopt;
                    }
                    const auto value = wordAttribute(property, "val");
                    if (!value || value->empty()) {
                        reason = "w:tblStyle does not contain a style ID";
                        return std::nullopt;
                    }
                    table_style_id = *value;
                } else if (isWordElement(property, "tblLook")) {
                    const auto first_row = wordAttribute(property, "firstRow");
                    if (!first_row) continue;
                    bool enabled = false;
                    if (!parseBooleanLexical(*first_row, enabled)) {
                        reason = "w:tblLook has an unrecognized firstRow value";
                        return std::nullopt;
                    }
                    look_first_row = enabled;
                } else if (isWordElement(property, "jc")) {
                    const auto value = wordAttribute(property, "val");
                    if (value && (*value == "left" || *value == "start")) {
                        table_alignment = BasicParagraphAlignment::left;
                    } else if (value && *value == "center") {
                        table_alignment = BasicParagraphAlignment::center;
                    } else if (value && (*value == "right" || *value == "end")) {
                        table_alignment = BasicParagraphAlignment::right;
                    }
                }
            }
            continue;
        }
        if (isWordElement(child, "tblGrid")) {
            if (saw_table_grid) {
                reason = "the table has more than one w:tblGrid element";
                return std::nullopt;
            }
            saw_table_grid = true;
            std::size_t columns = 0;
            for (const auto& grid_child : child.children()) {
                if (ignorableNode(grid_child)) continue;
                if (!isWordElement(grid_child, "gridCol")) {
                    reason = "w:tblGrid contains non-column markup";
                    return std::nullopt;
                }
                ++columns;
                bool recognized = true;
                const auto width = parseUnsignedIntegerAttribute(
                    grid_child, "w", recognized);
                if (recognized && width && *width > 0) {
                    declared_column_widths.push_back(*width);
                } else {
                    declared_column_widths.clear();
                }
            }
            if (columns != 0) declared_columns = columns;
            continue;
        }
        if (isWordElement(child, "tr")) {
            rows.push_back(child);
            continue;
        }
        reason = "the table contains a direct child outside tblPr/tblGrid/tr";
        return std::nullopt;
    }

    if (rows.empty() || rows.size() > NewTable::maximum_rows) {
        reason = "the table row count is outside Owl Docs' editable range";
        return std::nullopt;
    }

    ImportedTableBlock result;
    result.rows = rows.size();
    result.alignment = table_alignment;
    result.source_style_id = table_style_id;
    if (table_style_id) {
        result.style = basicTableStyleFromWordId(*table_style_id);
    }
    bool first_row_repeat_header = false;
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        std::vector<pugi::xml_node> cells;
        bool row_is_repeat_header = false;
        bool saw_row_properties = false;
        for (const auto& row_child : rows[row_index].children()) {
            if (ignorableNode(row_child)) continue;
            if (isWordElement(row_child, "trPr")) {
                if (saw_row_properties) {
                    reason = "a row has more than one w:trPr element";
                    return std::nullopt;
                }
                saw_row_properties = true;
                for (const auto& property : row_child.children()) {
                    if (isWordElement(property, "gridBefore") ||
                        isWordElement(property, "gridAfter")) {
                        reason = "a row starts or ends with omitted grid cells";
                        return std::nullopt;
                    }
                    if (!isWordElement(property, "tblHeader")) continue;
                    bool recognized = true;
                    row_is_repeat_header = onOffValue(property, recognized);
                    if (!recognized) {
                        reason = "w:tblHeader has an unrecognized value";
                        return std::nullopt;
                    }
                }
                continue;
            }
            if (isWordElement(row_child, "tc")) {
                cells.push_back(row_child);
                continue;
            }
            reason = "a row contains a direct child outside trPr/tc";
            return std::nullopt;
        }

        if (row_is_repeat_header && row_index != 0) {
            reason = "more than the first row is marked as a repeating header";
            return std::nullopt;
        }
        if (row_index == 0) first_row_repeat_header = row_is_repeat_header;
        if (cells.empty() || cells.size() > NewTable::maximum_columns) {
            reason = "the table column count is outside Owl Docs' editable range";
            return std::nullopt;
        }
        if (row_index == 0) {
            result.columns = cells.size();
            if (declared_columns && *declared_columns != result.columns) {
                reason = "w:tblGrid does not match the first row's cell count";
                return std::nullopt;
            }
            result.cells.reserve(result.rows * result.columns);
        } else if (cells.size() != result.columns) {
            reason = "rows contain different numbers of cells";
            return std::nullopt;
        }

        for (const auto& cell : cells) {
            pugi::xml_node cell_paragraph;
            ImportedTableCell imported_cell;
            bool saw_cell_properties = false;
            for (const auto& cell_child : cell.children()) {
                if (ignorableNode(cell_child)) continue;
                if (isWordElement(cell_child, "tcPr")) {
                    if (saw_cell_properties) {
                        reason = "a cell has more than one w:tcPr element";
                        return std::nullopt;
                    }
                    saw_cell_properties = true;
                    for (const auto& property : cell_child.children()) {
                        if (isWordElement(property, "vMerge") ||
                            isWordElement(property, "hMerge")) {
                            reason = "the table contains merged cells";
                            return std::nullopt;
                        }
                        if (isWordElement(property, "gridSpan")) {
                            bool recognized = true;
                            const auto span = parseUnsignedIntegerAttribute(
                                property, "val", recognized);
                            if (!recognized || !span || *span != 1) {
                                reason = "the table contains horizontally spanned cells";
                                return std::nullopt;
                            }
                        } else if (isWordElement(property, "tcW")) {
                            const auto type = wordAttribute(property, "type");
                            bool recognized = true;
                            const auto width = parseUnsignedIntegerAttribute(
                                property, "w", recognized);
                            if (recognized && width && (!type || *type == "dxa")) {
                                imported_cell.width_twips = *width;
                            }
                        } else if (isWordElement(property, "vAlign")) {
                            const auto value = wordAttribute(property, "val");
                            if (value && *value == "top") {
                                imported_cell.vertical_alignment =
                                    BasicVerticalAlignment::top;
                            } else if (value && *value == "center") {
                                imported_cell.vertical_alignment =
                                    BasicVerticalAlignment::center;
                            } else if (value && *value == "bottom") {
                                imported_cell.vertical_alignment =
                                    BasicVerticalAlignment::bottom;
                            }
                        } else if (isWordElement(property, "shd")) {
                            bool recognized = true;
                            const auto fill = parseRgbAttribute(
                                property, "fill", recognized);
                            if (recognized && fill) imported_cell.fill_rgb = *fill;
                        } else if (isWordElement(property, "tcMar")) {
                            for (const auto& margin : property.children()) {
                                const auto type = wordAttribute(margin, "type");
                                bool recognized = true;
                                const auto width = parseUnsignedIntegerAttribute(
                                    margin, "w", recognized);
                                if (!recognized || !width || (type && *type != "dxa")) {
                                    continue;
                                }
                                if (isWordElement(margin, "top")) {
                                    imported_cell.margin_top_twips = *width;
                                } else if (isWordElement(margin, "bottom")) {
                                    imported_cell.margin_bottom_twips = *width;
                                } else if (isWordElement(margin, "left") ||
                                           isWordElement(margin, "start")) {
                                    imported_cell.margin_left_twips = *width;
                                } else if (isWordElement(margin, "right") ||
                                           isWordElement(margin, "end")) {
                                    imported_cell.margin_right_twips = *width;
                                }
                            }
                        } else if (isWordElement(property, "tcBorders")) {
                            for (const auto& border : property.children()) {
                                const auto value = wordAttribute(border, "val");
                                if (!value || *value != "single") continue;
                                bool color_recognized = true;
                                bool width_recognized = true;
                                const auto color = parseRgbAttribute(
                                    border, "color", color_recognized);
                                const auto width = parseUnsignedIntegerAttribute(
                                    border, "sz", width_recognized);
                                if (!color_recognized || !width_recognized ||
                                    !color || !width || *width == 0 ||
                                    *width > std::numeric_limits<std::uint16_t>::max()) {
                                    continue;
                                }
                                ImportedTableBorder parsed_border{
                                    *color, static_cast<std::uint16_t>(*width)};
                                if (isWordElement(border, "top")) {
                                    imported_cell.border_top = parsed_border;
                                } else if (isWordElement(border, "bottom")) {
                                    imported_cell.border_bottom = parsed_border;
                                } else if (isWordElement(border, "left") ||
                                           isWordElement(border, "start")) {
                                    imported_cell.border_left = parsed_border;
                                } else if (isWordElement(border, "right") ||
                                           isWordElement(border, "end")) {
                                    imported_cell.border_right = parsed_border;
                                }
                            }
                        }
                    }
                    continue;
                }
                if (isWordElement(cell_child, "p")) {
                    if (cell_paragraph) {
                        reason = "a cell contains more than one paragraph";
                        return std::nullopt;
                    }
                    cell_paragraph = cell_child;
                    continue;
                }
                reason = "a cell contains nested or non-paragraph body content";
                return std::nullopt;
            }
            if (!cell_paragraph ||
                !directParagraphStructureSupported(cell_paragraph, theme)) {
                reason = "a cell does not contain one basic direct paragraph";
                return std::nullopt;
            }
            const auto source = paragraph_indices.find(cell_paragraph);
            if (source == paragraph_indices.end() ||
                source->second >= paragraphs.size()) {
                reason = "a cell paragraph could not be mapped to parsed text";
                return std::nullopt;
            }
            if (paragraphContainsFragment(
                    paragraphs[source->second], FragmentKind::equation)) {
                reason = "equations inside table cells are preserved view-only";
                return std::nullopt;
            }
            if (paragraphContainsFragment(
                    paragraphs[source->second], FragmentKind::inline_image)) {
                reason =
                    "inline images inside table cells are preserved view-only";
                return std::nullopt;
            }
            imported_cell.source_paragraph_index = source->second;
            result.cells.push_back(std::move(imported_cell));
        }
    }

    result.header_row = first_row_repeat_header || look_first_row;
    if (declared_column_widths.size() == result.columns) {
        result.column_widths_twips = std::move(declared_column_widths);
    }
    if (result.cells.size() != result.rows * result.columns) {
        reason = "the table is not a complete rectangular grid";
        return std::nullopt;
    }
    return result;
}

bool parseDocumentXml(
    std::string_view document_xml,
    std::vector<Paragraph>& paragraphs,
    std::vector<ImportedBodyBlock>& body_blocks,
    std::vector<SpanLocation>& spans,
    CompatibilityReport& report,
    const ImportContext& context,
    const OpenOptions& options,
    Error* error) {
    pugi::xml_document document;
    const pugi::xml_parse_result parse_result = document.load_buffer(
        document_xml.data(),
        document_xml.size(),
        pugi::parse_default | pugi::parse_ws_pcdata,
        pugi::encoding_auto);
    if (!parse_result) {
        std::ostringstream message;
        message << "Malformed word/document.xml at byte " << parse_result.offset << ": "
                << parse_result.description();
        setError(error, ErrorCode::malformed_document_xml, message.str());
        return false;
    }
    if (!enforceXmlComplexity(document, options, kDocumentPart, error)) {
        return false;
    }

    pugi::xml_node root = document.document_element();
    if (!isWordElement(root, "document")) {
        setError(error, ErrorCode::invalid_word_document,
                 "word/document.xml does not have a WordprocessingML document root");
        return false;
    }
    pugi::xml_node body;
    for (pugi::xml_node child : root.children()) {
        if (isWordElement(child, "body")) {
            body = child;
            break;
        }
    }
    if (!body) {
        setError(error, ErrorCode::invalid_word_document,
                 "word/document.xml does not contain a WordprocessingML body");
        return false;
    }

    const bool utf8_source = isUtf8Xml(document_xml);
    std::set<std::pair<IssueCode, std::string>> seen_issues;
    if (!utf8_source) {
        report.issues.push_back(CompatibilityIssue{
            IssueSeverity::blocking,
            IssueCode::non_utf8_document_xml,
            std::string(kDocumentPart),
            "The XML can be read, but exact source-range text patches require UTF-8 encoding",
            std::nullopt});
    }

    std::vector<pugi::xml_node> paragraph_nodes;
    collectParagraphNodes(body, paragraph_nodes);
    ParagraphIndexByNode paragraph_indices;
    paragraph_indices.reserve(paragraph_nodes.size());
    for (std::size_t index = 0; index < paragraph_nodes.size(); ++index) {
        paragraph_indices.emplace(paragraph_nodes[index], index);
    }
    bool basic_body = true;
    TextSpanId next_span_id = 1;
    paragraphs.reserve(paragraph_nodes.size());
    for (std::size_t paragraph_index = 0; paragraph_index < paragraph_nodes.size(); ++paragraph_index) {
        const pugi::xml_node paragraph_node = paragraph_nodes[paragraph_index];
        Paragraph direct_paragraph;
        parseParagraphFormat(
            paragraph_node, direct_paragraph, &context.theme);

        Paragraph paragraph = context.default_paragraph_format;
        BasicRunFormat paragraph_run_format = context.default_run_format;
        bool inherited_format_is_basic = true;
        const auto effective_style = direct_paragraph.style_id
            ? direct_paragraph.style_id
            : context.default_paragraph_style;
        if (effective_style) {
            std::unordered_set<std::string> visiting;
            applyParagraphStyleChain(
                context, *effective_style, paragraph,
                paragraph_run_format, inherited_format_is_basic, visiting);
        }

        // Capture the resolved paragraph-style contribution before numbering
        // and direct pPr are overlaid.  An explicit source pStyle is required:
        // provenance must never float independently from the identity it
        // describes.
        if (direct_paragraph.style_id && inherited_format_is_basic) {
            ParagraphStyleProvenance provenance;
            provenance.inherited_character_format = paragraph_run_format;
            provenance.inherited_paragraph_mark_format =
                paragraph_run_format;
            if (paragraph.paragraph_mark_format) {
                overlayRunFormat(
                    provenance.inherited_paragraph_mark_format,
                    *paragraph.paragraph_mark_format);
            }
            provenance.inherited_alignment = paragraph.alignment;
            provenance.inherited_left_indent_twips =
                paragraph.left_indent_twips;
            provenance.inherited_right_indent_twips =
                paragraph.right_indent_twips;
            provenance.inherited_first_line_indent_twips =
                paragraph.first_line_indent_twips;
            provenance.inherited_space_before_twips =
                paragraph.space_before_twips;
            provenance.inherited_space_after_twips =
                paragraph.space_after_twips;
            provenance.inherited_line_spacing = paragraph.line_spacing;
            provenance.inherited_line_spacing_rule =
                paragraph.line_spacing_rule;
            provenance.inherited_keep_with_next = paragraph.keep_with_next;
            provenance.inherited_keep_lines = paragraph.keep_lines;
            provenance.inherited_page_break_before =
                paragraph.page_break_before;
            provenance.direct_alignment = direct_paragraph.alignment;
            provenance.direct_left_indent_twips =
                direct_paragraph.left_indent_twips;
            provenance.direct_right_indent_twips =
                direct_paragraph.right_indent_twips;
            provenance.direct_first_line_indent_twips =
                direct_paragraph.first_line_indent_twips;
            provenance.direct_space_before_twips =
                direct_paragraph.space_before_twips;
            provenance.direct_space_after_twips =
                direct_paragraph.space_after_twips;
            provenance.direct_line_spacing = direct_paragraph.line_spacing;
            provenance.direct_line_spacing_rule =
                direct_paragraph.line_spacing_rule;
            provenance.direct_keep_with_next =
                direct_paragraph.keep_with_next;
            provenance.direct_keep_lines = direct_paragraph.keep_lines;
            provenance.direct_page_break_before =
                direct_paragraph.page_break_before;
            if (direct_paragraph.paragraph_mark_format) {
                provenance.direct_paragraph_mark_format =
                    *direct_paragraph.paragraph_mark_format;
            }
            paragraph.style_provenance = std::move(provenance);
        }

        const auto effective_num_id = direct_paragraph.numbering_id
            ? direct_paragraph.numbering_id
            : paragraph.numbering_id;
        const auto effective_num_level = direct_paragraph.numbering_level
            ? direct_paragraph.numbering_level
            : paragraph.numbering_level;
        bool numbering_is_basic = true;
        if (effective_num_id && *effective_num_id > 0) {
            const auto level = effective_num_level.value_or(0);
            if (const auto* definition = numberLevel(
                    context, *effective_num_id, level);
                definition && definition->supported) {
                overlayOptional(
                    paragraph.left_indent_twips,
                    definition->value.left_indent_twips);
                overlayOptional(
                    paragraph.first_line_indent_twips,
                    definition->value.first_line_indent_twips);
                if (!definition->value.left_tab_stops_twips.empty()) {
                    paragraph.left_tab_stops_twips =
                        definition->value.left_tab_stops_twips;
                }
            } else {
                numbering_is_basic = false;
            }
        }
        overlayParagraphFormat(paragraph, direct_paragraph);
        paragraph.style_id = direct_paragraph.style_id;
        paragraph.numbering_id = effective_num_id;
        paragraph.numbering_level = effective_num_id
            ? std::optional<std::uint8_t>(effective_num_level.value_or(0))
            : std::nullopt;
        paragraph.direct_body_child = paragraph_node.parent() == body;
        paragraph.has_unsupported_content =
            !directParagraphStructureSupported(paragraph_node, &context.theme);
        paragraph.format_is_basic = direct_paragraph.format_is_basic &&
                                    inherited_format_is_basic &&
                                    numbering_is_basic;
        if (!paragraph.format_is_basic) {
            basic_body = false;
            appendIssueOnce(
                report.issues,
                seen_issues,
                CompatibilityIssue{
                    IssueSeverity::warning,
                    IssueCode::unsupported_formatting,
                    std::string(kDocumentPart),
                    "Paragraph contains unsupported or non-basic w:pPr formatting",
                    paragraph_index});
        }

        std::vector<pugi::xml_node> content_nodes;
        collectContentForParagraph(paragraph_node, paragraph_node, content_nodes);
        const bool enclosing_sensitive = hasWordAncestorBetween(
            paragraph_node,
            body,
            {"ins", "del", "moveFrom", "moveTo", "sdt", "customXml", "altChunk"});
        const bool field_sensitive = hasDangerousFieldMarkup(paragraph_node) || enclosing_sensitive;
        paragraph.runs.reserve(content_nodes.size());
        for (std::size_t run_index = 0; run_index < content_nodes.size(); ++run_index) {
            const pugi::xml_node content_node = content_nodes[run_index];
            Run run;
            if (isMathElement(content_node, "oMath") ||
                isMathElement(content_node, "oMathPara")) {
                const auto equation = parseOmmlEquation(
                    content_node, &context.theme);
                run.has_unsupported_content = !equation.has_value() ||
                                              content_node.parent() != paragraph_node;
                if (equation) {
                    run.style_id = equation->style_id;
                    run.format = paragraph_run_format;
                    bool run_style_is_basic = true;
                    if (run.style_id) {
                        std::unordered_set<std::string> visiting;
                        applyRunStyleChain(
                            context, *run.style_id, run.format,
                            run_style_is_basic, visiting);
                        std::unordered_set<std::string> override_visiting;
                        applyRunStyleChain(
                            context, *run.style_id,
                            run.paragraph_style_overrides,
                            run_style_is_basic, override_visiting);
                    }
                    overlayRunFormat(run.format, equation->direct_format);
                    overlayRunFormat(
                        run.paragraph_style_overrides,
                        equation->direct_format);
                    run.format_is_basic = run_style_is_basic &&
                                          inherited_format_is_basic;
                    if (run.format_is_basic) {
                        RunFragment fragment;
                        fragment.kind = FragmentKind::equation;
                        fragment.equation = equation->payload;
                        run.fragments.push_back(std::move(fragment));
                    } else {
                        run.has_unsupported_content = true;
                        paragraph.has_unsupported_content = true;
                        appendIssueOnce(
                            report.issues,
                            seen_issues,
                            CompatibilityIssue{
                                IssueSeverity::warning,
                                IssueCode::unsupported_formatting,
                                std::string(kDocumentPart),
                                "Office Math object uses an unresolved or unsupported character style and was preserved unchanged",
                                paragraph_index});
                    }
                }
                if (!equation || !run.format_is_basic) {
                    basic_body = false;
                    appendIssueOnce(
                        report.issues,
                        seen_issues,
                        CompatibilityIssue{
                            IssueSeverity::warning,
                            IssueCode::unsupported_body_content,
                            std::string(kDocumentPart),
                            "Office Math object is outside Owl Docs' safe editable subset and was preserved unchanged",
                            paragraph_index});
                }
                paragraph.runs.push_back(std::move(run));
                continue;
            }

            Run direct_run;
            parseRunFormat(content_node, direct_run, &context.theme);
            run.has_unsupported_content = !directRunStructureSupported(content_node) ||
                                          content_node.parent() != paragraph_node;
            run.style_id = direct_run.style_id;
            run.format = paragraph_run_format;
            bool run_style_is_basic = true;
            if (direct_run.style_id) {
                std::unordered_set<std::string> visiting;
                applyRunStyleChain(
                    context, *direct_run.style_id, run.format,
                    run_style_is_basic, visiting);
                std::unordered_set<std::string> override_visiting;
                applyRunStyleChain(
                    context, *direct_run.style_id,
                    run.paragraph_style_overrides,
                    run_style_is_basic, override_visiting);
            }
            overlayRunFormat(run.format, direct_run.format);
            overlayRunFormat(
                run.paragraph_style_overrides, direct_run.format);
            run.format_is_basic = direct_run.format_is_basic &&
                                  run_style_is_basic &&
                                  inherited_format_is_basic;
            if (!run.format_is_basic) {
                basic_body = false;
                appendIssueOnce(
                    report.issues,
                    seen_issues,
                    CompatibilityIssue{
                        IssueSeverity::warning,
                        IssueCode::unsupported_formatting,
                        std::string(kDocumentPart),
                        "Run contains unsupported or non-basic w:rPr formatting",
                        paragraph_index});
            }
            collectRunTokens(
                content_node,
                content_node,
                paragraph_node,
                utf8_source,
                field_sensitive,
                paragraph_index,
                run_index,
                document_xml,
                run,
                spans,
                report.issues,
                seen_issues,
                next_span_id);
            paragraph.runs.push_back(std::move(run));
        }

        std::size_t visible_fragment_count = 0;
        std::optional<RunFragment> only_fragment;
        std::optional<FragmentKind> last_layout_fragment;
        for (const Run& run : paragraph.runs) {
            for (const RunFragment& fragment : run.fragments) {
                if (fragment.kind != FragmentKind::text ||
                    !fragment.text.empty()) {
                    last_layout_fragment = fragment.kind;
                }
                ++visible_fragment_count;
                only_fragment = fragment;
            }
        }
        // Mapping a terminal page break onto page-break-before for the next
        // semantic paragraph preserves its pagination. A break followed by
        // content in the same OOXML paragraph cannot be represented by the
        // current paragraph-only core and must not be moved to a later block.
        paragraph.hard_page_break_after =
            last_layout_fragment == FragmentKind::page_break;
        paragraph.whole_text_editable =
            visible_fragment_count == 1 && only_fragment.has_value() &&
            only_fragment->kind == FragmentKind::text && only_fragment->editable &&
            !paragraph.has_unsupported_content;
        paragraphs.push_back(std::move(paragraph));
    }

    assignNumberingMarkers(paragraphs, context);

    body_blocks.reserve(body_blocks.size() +
                        static_cast<std::size_t>(std::distance(
                            body.children().begin(), body.children().end())));
    for (const auto& child : body.children()) {
        if (ignorableNode(child) || isWordElement(child, "sectPr")) continue;
        if (isWordElement(child, "p")) {
            const auto source = paragraph_indices.find(child);
            if (source != paragraph_indices.end()) {
                body_blocks.emplace_back(ImportedParagraphBlock{source->second});
            }
            if (!directParagraphStructureSupported(child, &context.theme)) {
                basic_body = false;
                appendIssueOnce(
                    report.issues, seen_issues,
                    CompatibilityIssue{
                        IssueSeverity::warning,
                        IssueCode::unsupported_body_content,
                        std::string(kDocumentPart),
                        "Paragraph contains markup outside the basic pPr/r/rPr/text/tab/break subset",
                        source == paragraph_indices.end()
                            ? std::nullopt
                            : std::optional<std::size_t>(source->second)});
            }
            continue;
        }

        if (isWordElement(child, "tbl")) {
            std::string reason;
            auto table = parseSimpleImportedTable(
                child, paragraph_indices, paragraphs, &context.theme, reason);
            if (table) {
                body_blocks.emplace_back(std::move(*table));
                continue;
            }
            basic_body = false;
            const auto fallback = descendantParagraphIndices(
                child, paragraph_indices);
            const std::string detail =
                "Table is preserved as view-only fallback because " + reason;
            body_blocks.emplace_back(ImportedUnsupportedBodyBlock{
                std::string(child.name()), reason, fallback});
            appendIssueOnce(
                report.issues, seen_issues,
                CompatibilityIssue{
                    IssueSeverity::warning,
                    IssueCode::unsupported_body_content,
                    std::string(kDocumentPart), detail,
                    fallback.empty()
                        ? std::nullopt
                        : std::optional<std::size_t>(fallback.front())});
            continue;
        }

        basic_body = false;
        const auto fallback = descendantParagraphIndices(
            child, paragraph_indices);
        const std::string reason =
            "the direct body element is outside Owl Docs' editable block subset";
        body_blocks.emplace_back(ImportedUnsupportedBodyBlock{
            std::string(child.name()), reason, fallback});
        appendIssueOnce(
            report.issues, seen_issues,
            CompatibilityIssue{
                IssueSeverity::warning,
                IssueCode::unsupported_body_content,
                std::string(kDocumentPart),
                "Preserved non-basic body element <" +
                    std::string(child.name()) + ">",
                fallback.empty()
                    ? std::nullopt
                    : std::optional<std::size_t>(fallback.front())});
    }

    const bool has_global_blocker = std::any_of(
        report.issues.begin(), report.issues.end(), [](const CompatibilityIssue& issue) {
            return issue.severity == IssueSeverity::blocking;
        });
    const bool has_editable_span =
        std::any_of(spans.begin(), spans.end(), [](const SpanLocation& span) { return span.editable; });
    if (has_global_blocker || (!has_editable_span && !basic_body)) {
        report.classification = CompatibilityClass::exact_round_trip_only;
    } else if (basic_body) {
        report.classification = CompatibilityClass::basic_body_text_patch;
    } else if (has_editable_span) {
        report.classification = CompatibilityClass::safe_text_patch;
    } else {
        report.classification = CompatibilityClass::exact_round_trip_only;
    }
    return true;
}

bool validateOpcMetadata(
    std::string_view content_types_xml,
    std::string_view relationships_xml,
    const OpenOptions& options,
    Error* error) {
    pugi::xml_document content_types;
    const pugi::xml_parse_result content_types_result = content_types.load_buffer(
        content_types_xml.data(),
        content_types_xml.size(),
        pugi::parse_default,
        pugi::encoding_auto);
    if (!content_types_result) {
        setError(error, ErrorCode::invalid_opc_metadata,
                 "[Content_Types].xml is malformed: " +
                     std::string(content_types_result.description()));
        return false;
    }
    if (!enforceXmlComplexity(
            content_types, options, kContentTypesPart, error)) {
        return false;
    }
    const pugi::xml_node types_root = content_types.document_element();
    constexpr std::string_view content_types_namespace =
        "http://schemas.openxmlformats.org/package/2006/content-types";
    if (localName(types_root.name()) != "Types" ||
        namespaceUri(types_root) != content_types_namespace) {
        setError(error, ErrorCode::invalid_opc_metadata,
                 "[Content_Types].xml does not have the OPC Types root");
        return false;
    }
    bool has_document_override = false;
    std::optional<std::string> rejected_word_main_type;
    for (pugi::xml_node override_node : types_root.children()) {
        if (override_node.type() != pugi::node_element ||
            localName(override_node.name()) != "Override" ||
            namespaceUri(override_node) != content_types_namespace) {
            continue;
        }
        if (std::string_view(override_node.attribute("PartName").value()) !=
            "/word/document.xml") {
            continue;
        }
        const std::string_view content_type = override_node.attribute("ContentType").value();
        constexpr std::string_view supported_main_type =
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
        static constexpr std::array<std::string_view, 3> excluded_word_main_types{
            "application/vnd.ms-word.document.macroEnabled.main+xml",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml",
            "application/vnd.ms-word.template.macroEnabledTemplate.main+xml"};
        has_document_override = content_type == supported_main_type;
        if (!has_document_override &&
            std::find(
                excluded_word_main_types.begin(), excluded_word_main_types.end(), content_type) !=
                excluded_word_main_types.end()) {
            rejected_word_main_type = content_type;
        }
        break;
    }
    if (!has_document_override) {
        if (rejected_word_main_type.has_value()) {
            setError(
                error,
                ErrorCode::invalid_opc_metadata,
                "Only macro-free DOCX documents are supported; macro-enabled documents "
                "and Word templates are not accepted, even when renamed with a .docx suffix");
            return false;
        }
        setError(error, ErrorCode::invalid_opc_metadata,
                 "[Content_Types].xml does not identify word/document.xml as a macro-free DOCX main part");
        return false;
    }

    pugi::xml_document relationships;
    const pugi::xml_parse_result relationships_result = relationships.load_buffer(
        relationships_xml.data(),
        relationships_xml.size(),
        pugi::parse_default,
        pugi::encoding_auto);
    if (!relationships_result) {
        setError(error, ErrorCode::invalid_opc_metadata,
                 "_rels/.rels is malformed: " + std::string(relationships_result.description()));
        return false;
    }
    if (!enforceXmlComplexity(
            relationships, options, kRootRelationshipsPart, error)) {
        return false;
    }
    const pugi::xml_node relationships_root = relationships.document_element();
    const std::string relationships_namespace = namespaceUri(relationships_root);
    if (localName(relationships_root.name()) != "Relationships" ||
        (relationships_namespace !=
             "http://schemas.openxmlformats.org/package/2006/relationships" &&
         relationships_namespace != "http://purl.oclc.org/ooxml/package/relationships")) {
        setError(error, ErrorCode::invalid_opc_metadata,
                 "_rels/.rels does not have an OPC Relationships root");
        return false;
    }
    bool has_office_document_relationship = false;
    for (pugi::xml_node relationship : relationships_root.children()) {
        if (relationship.type() != pugi::node_element ||
            localName(relationship.name()) != "Relationship" ||
            namespaceUri(relationship) != relationships_namespace) {
            continue;
        }
        const std::string_view type = relationship.attribute("Type").value();
        const bool office_document_type =
            type ==
                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" ||
            type == "http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument";
        std::string_view target = relationship.attribute("Target").value();
        if (target.starts_with("./")) {
            target.remove_prefix(2);
        }
        if (target.starts_with('/')) {
            target.remove_prefix(1);
        }
        const bool internal = std::string_view(relationship.attribute("TargetMode").value()) != "External";
        if (office_document_type && internal && target == kDocumentPart) {
            has_office_document_relationship = true;
            break;
        }
    }
    if (!has_office_document_relationship) {
        setError(error, ErrorCode::invalid_opc_metadata,
                 "_rels/.rels does not target word/document.xml as the office document");
        return false;
    }
    return true;
}

std::optional<std::string> safeDocumentRelationshipTarget(
    std::string_view encoded_target) {
    if (encoded_target.empty() || encoded_target.find('\\') != std::string_view::npos ||
        encoded_target.find('\0') != std::string_view::npos ||
        encoded_target.find('?') != std::string_view::npos ||
        encoded_target.find('#') != std::string_view::npos) {
        return std::nullopt;
    }
    std::filesystem::path target(encoded_target);
    std::filesystem::path resolved = target.is_absolute()
        ? target.relative_path()
        : std::filesystem::path("word") / target;
    resolved = resolved.lexically_normal();
    const std::string member = resolved.generic_string();
    if (member == "word" || !member.starts_with("word/") ||
        member.find("../") != std::string::npos) {
        return std::nullopt;
    }
    return member;
}

struct AuxiliaryPartTargets {
    std::optional<std::string> styles;
    std::optional<std::string> numbering;
    std::optional<std::string> theme;
    std::optional<std::string> header;
    std::optional<std::string> footer;
};

AuxiliaryPartTargets auxiliaryPartTargets(
    std::string_view relationships_xml, const OpenOptions& options) {
    AuxiliaryPartTargets targets;
    if (relationships_xml.empty()) return targets;
    pugi::xml_document relationships;
    if (!relationships.load_buffer(
            relationships_xml.data(), relationships_xml.size(),
            pugi::parse_default, pugi::encoding_auto) ||
        !::docxstudio::xml::inspectComplexity(
             relationships, options.max_xml_depth, options.max_xml_nodes)
             .accepted()) {
        return targets;
    }
    const auto root = relationships.document_element();
    const std::string root_namespace = namespaceUri(root);
    constexpr std::string_view transitional_package_relationships =
        "http://schemas.openxmlformats.org/package/2006/relationships";
    constexpr std::string_view strict_package_relationships =
        "http://purl.oclc.org/ooxml/package/relationships";
    if (localName(root.name()) != "Relationships" ||
        (root_namespace != transitional_package_relationships &&
         root_namespace != strict_package_relationships)) {
        return targets;
    }
    for (const auto relationship : root.children()) {
        if (!isNamespacedElement(
                relationship, "Relationship", root_namespace) ||
            std::string_view(relationship.attribute("TargetMode").value()) ==
                "External") {
            continue;
        }
        const std::string_view type = relationship.attribute("Type").value();
        if (!(type.starts_with(kOfficeRelationshipsNamespace) ||
              type.starts_with(kStrictOfficeRelationshipsNamespace))) {
            continue;
        }
        const auto target = safeDocumentRelationshipTarget(
            relationship.attribute("Target").value());
        if (!target) continue;
        if (type.ends_with("/styles")) {
            targets.styles = *target;
        } else if (type.ends_with("/numbering")) {
            targets.numbering = *target;
        } else if (type.ends_with("/theme")) {
            targets.theme = *target;
        } else if (type.ends_with("/header") && !targets.header) {
            targets.header = *target;
        } else if (type.ends_with("/footer") && !targets.footer) {
            targets.footer = *target;
        }
    }
    return targets;
}

std::optional<std::string> parseSimpleStoryText(
    std::string_view xml, const OpenOptions& options) {
    if (xml.empty()) return std::nullopt;
    pugi::xml_document story;
    if (!story.load_buffer(xml.data(), xml.size(), pugi::parse_default,
                           pugi::encoding_auto) ||
        !::docxstudio::xml::inspectComplexity(
             story, options.max_xml_depth, options.max_xml_nodes)
             .accepted()) {
        return std::nullopt;
    }
    std::string result;
    const auto appendNode = [&](const auto& self,
                                const pugi::xml_node& node) -> void {
        if (isWordElement(node, "pPr") || isWordElement(node, "rPr")) {
            return;
        }
        if (isWordElement(node, "t")) {
            result += node.child_value();
            return;
        }
        if (isWordElement(node, "tab")) {
            result.push_back('\t');
            return;
        }
        if (isWordElement(node, "br") || isWordElement(node, "cr")) {
            result.push_back('\n');
            return;
        }
        if (isWordElement(node, "fldSimple")) {
            std::string instruction = wordAttribute(node, "instr").value_or("");
            instruction.erase(std::remove_if(
                instruction.begin(), instruction.end(),
                [](unsigned char value) { return std::isspace(value); }),
                instruction.end());
            std::transform(instruction.begin(), instruction.end(),
                           instruction.begin(), [](unsigned char value) {
                               return static_cast<char>(std::toupper(value));
                           });
            if (instruction == "PAGE") result += "{PAGE}";
            else if (instruction == "NUMPAGES") result += "{PAGES}";
            else for (const auto child : node.children()) self(self, child);
            return;
        }
        for (const auto child : node.children()) self(self, child);
    };
    bool firstParagraph = true;
    for (const auto node : story.document_element().children()) {
        if (!isWordElement(node, "p")) continue;
        if (!firstParagraph) result.push_back('\n');
        firstParagraph = false;
        appendNode(appendNode, node);
    }
    return result;
}

std::string imageContentTypeForMember(std::string_view member) {
    const auto dot = member.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string extension(member.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (extension == "png") return "image/png";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "gif") return "image/gif";
    if (extension == "bmp") return "image/bmp";
    if (extension == "tif" || extension == "tiff") return "image/tiff";
    if (extension == "webp") return "image/webp";
    return {};
}

void resolveInlineImages(ParsedPackage& parsed, const OpenOptions& options) {
    bool needs_images = false;
    for (const auto& paragraph : parsed.paragraphs) {
        for (const auto& run : paragraph.runs) {
            for (const auto& fragment : run.fragments) {
                needs_images = needs_images ||
                    (fragment.kind == FragmentKind::inline_image &&
                     fragment.inline_image.has_value());
            }
        }
    }
    if (!needs_images) return;

    const auto relationships_entry = std::find_if(
        parsed.entries.begin(), parsed.entries.end(), [](const EntryRecord& entry) {
            return entry.member.name == kDocumentRelationshipsPart;
        });
    if (relationships_entry == parsed.entries.end()) return;

    Error ignored_error;
    zip_t* archive = openZipFromBytes(parsed.bytes, &ignored_error);
    if (!archive) return;
    std::string relationships_xml;
    if (!readEntry(
            archive, relationships_entry->index,
            options.max_document_xml_bytes, relationships_xml,
            &ignored_error)) {
        zip_discard(archive);
        return;
    }

    pugi::xml_document relationships;
    if (!relationships.load_buffer(
            relationships_xml.data(), relationships_xml.size(),
            pugi::parse_default, pugi::encoding_auto)) {
        zip_discard(archive);
        return;
    }
    const auto complexity = ::docxstudio::xml::inspectComplexity(
        relationships, options.max_xml_depth, options.max_xml_nodes);
    if (!complexity.accepted()) {
        zip_discard(archive);
        return;
    }
    const pugi::xml_node root = relationships.document_element();
    const std::string relationship_namespace = namespaceUri(root);
    constexpr std::string_view transitional_package_relationships =
        "http://schemas.openxmlformats.org/package/2006/relationships";
    constexpr std::string_view strict_package_relationships =
        "http://purl.oclc.org/ooxml/package/relationships";
    if (localName(root.name()) != "Relationships" ||
        (relationship_namespace != transitional_package_relationships &&
         relationship_namespace != strict_package_relationships)) {
        zip_discard(archive);
        return;
    }

    std::unordered_map<std::string, std::string> image_members;
    for (const pugi::xml_node relationship : root.children()) {
        if (!isNamespacedElement(
                relationship, "Relationship", relationship_namespace)) {
            continue;
        }
        const std::string_view mode = relationship.attribute("TargetMode").value();
        if (mode == "External") continue;
        const std::string_view type = relationship.attribute("Type").value();
        if (!(type.ends_with("/image") &&
              (type.starts_with(kOfficeRelationshipsNamespace) ||
               type.starts_with(kStrictOfficeRelationshipsNamespace)))) {
            continue;
        }
        const std::string id = relationship.attribute("Id").value();
        const auto member = safeDocumentRelationshipTarget(
            relationship.attribute("Target").value());
        if (!id.empty() && member && !imageContentTypeForMember(*member).empty()) {
            image_members.emplace(id, *member);
        }
    }

    constexpr std::uint64_t kMaximumRenderedImageBytes =
        64ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMaximumAggregateRenderedImageBytes =
        256ULL * 1024ULL * 1024ULL;
    constexpr std::size_t kMaximumResolvedImageReferences = 4'096U;
    constexpr std::size_t kMaximumUniqueRenderedImages = 1'024U;
    std::unordered_map<
        std::string,
        std::shared_ptr<const std::vector<std::uint8_t>>> cached_bytes;
    std::uint64_t aggregate_cached_bytes = 0U;
    std::size_t attempted_references = 0U;
    for (auto& paragraph : parsed.paragraphs) {
        for (auto& run : paragraph.runs) {
            for (auto& fragment : run.fragments) {
                if (fragment.kind != FragmentKind::inline_image ||
                    !fragment.inline_image) {
                    continue;
                }
                if (attempted_references >=
                    kMaximumResolvedImageReferences) {
                    continue;
                }
                ++attempted_references;
                auto& image = *fragment.inline_image;
                const auto related = image_members.find(image.relationship_id);
                if (related == image_members.end()) continue;
                const auto package_entry = std::find_if(
                    parsed.entries.begin(), parsed.entries.end(),
                    [&related](const EntryRecord& entry) {
                        return entry.member.name == related->second;
                    });
                if (package_entry == parsed.entries.end() ||
                    package_entry->member.uncompressed_size >
                        std::min(options.max_member_uncompressed_bytes,
                                 kMaximumRenderedImageBytes)) {
                    continue;
                }
                auto cached = cached_bytes.find(related->second);
                if (cached == cached_bytes.end()) {
                    if (cached_bytes.size() >=
                            kMaximumUniqueRenderedImages ||
                        package_entry->member.uncompressed_size >
                            kMaximumAggregateRenderedImageBytes -
                                aggregate_cached_bytes) {
                        continue;
                    }
                    std::string contents;
                    if (!readEntry(
                            archive, package_entry->index,
                            std::min(options.max_member_uncompressed_bytes,
                                     kMaximumRenderedImageBytes),
                            contents, &ignored_error)) {
                        cached_bytes.emplace(related->second, nullptr);
                        continue;
                    }
                    auto shared_contents =
                        std::make_shared<const std::vector<std::uint8_t>>(
                            contents.begin(), contents.end());
                    cached = cached_bytes.emplace(
                        related->second, std::move(shared_contents)).first;
                    aggregate_cached_bytes +=
                        static_cast<std::uint64_t>(contents.size());
                }
                if (!cached->second) continue;
                image.package_member = related->second;
                image.content_type = imageContentTypeForMember(related->second);
                image.bytes = SharedImageBytes(cached->second);
            }
        }
    }
    zip_discard(archive);
}

bool isCanonicalWriterWordElement(
    const pugi::xml_node& node, std::string_view expected_local_name) {
    return node.type() == pugi::node_element &&
           prefixName(node.name()) == "w" &&
           localName(node.name()) == expected_local_name &&
           namespaceUri(node) == kWordNamespace;
}

bool hasNoAttributes(const pugi::xml_node& node) {
    return node.attributes().begin() == node.attributes().end();
}

bool hasOnlyCanonicalWriterWordAttributes(
    const pugi::xml_node& node,
    std::initializer_list<std::string_view> allowed_names) {
    for (const pugi::xml_attribute attribute : node.attributes()) {
        const std::string_view qualified_name = attribute.name();
        if (prefixName(qualified_name) != "w" ||
            namespaceUriForName(node, qualified_name) != kWordNamespace ||
            std::find(
                allowed_names.begin(), allowed_names.end(),
                localName(qualified_name)) == allowed_names.end()) {
            return false;
        }
    }
    return true;
}

bool canonicalWriterEmptyLeaf(
    const pugi::xml_node& node, std::string_view expected_local_name,
    std::initializer_list<std::string_view> allowed_attributes) {
    return isCanonicalWriterWordElement(node, expected_local_name) &&
           hasOnlyCanonicalWriterWordAttributes(node, allowed_attributes) &&
           !node.first_child();
}

bool canonicalWriterOnOffProperty(
    const pugi::xml_node& node, std::string_view expected_local_name) {
    if (!canonicalWriterEmptyLeaf(node, expected_local_name, {"val"})) {
        return false;
    }
    const auto value = wordAttribute(node, "val");
    // The current writer uses an absent value for true and the canonical
    // decimal zero spelling for false. Broader ST_OnOff spellings are valid
    // OOXML, but accepting them here would no longer identify writer-owned
    // canonical markup.
    return !value || *value == "0";
}

bool canonicalWriterRgb(std::string_view value) {
    return value.size() == 6U &&
           std::all_of(value.begin(), value.end(), [](unsigned char digit) {
               return (digit >= '0' && digit <= '9') ||
                      (digit >= 'A' && digit <= 'F');
           });
}

bool canonicalWriterRunProperties(const pugi::xml_node& properties) {
    if (!isCanonicalWriterWordElement(properties, "rPr") ||
        !hasNoAttributes(properties)) {
        return false;
    }

    int previous_order = -1;
    bool saw_property = false;
    std::optional<std::string> size;
    std::optional<std::string> complex_script_size;
    for (const pugi::xml_node property : properties.children()) {
        if (property.type() != pugi::node_element) return false;

        int order = -1;
        bool recognized = false;
        if (isCanonicalWriterWordElement(property, "rFonts")) {
            order = 0;
            recognized = canonicalWriterEmptyLeaf(
                property, "rFonts", {"ascii", "hAnsi"});
            const auto ascii = wordAttribute(property, "ascii");
            const auto high_ansi = wordAttribute(property, "hAnsi");
            recognized = recognized && ascii && high_ansi &&
                         !ascii->empty() && ascii == high_ansi;
        } else if (isCanonicalWriterWordElement(property, "b")) {
            order = 1;
            recognized = canonicalWriterOnOffProperty(property, "b");
        } else if (isCanonicalWriterWordElement(property, "i")) {
            order = 2;
            recognized = canonicalWriterOnOffProperty(property, "i");
        } else if (isCanonicalWriterWordElement(property, "strike")) {
            order = 3;
            recognized = canonicalWriterOnOffProperty(property, "strike");
        } else if (isCanonicalWriterWordElement(property, "color")) {
            order = 4;
            recognized = canonicalWriterEmptyLeaf(property, "color", {"val"});
            const auto value = wordAttribute(property, "val");
            recognized = recognized && value && canonicalWriterRgb(*value);
        } else if (isCanonicalWriterWordElement(property, "sz")) {
            order = 5;
            recognized = canonicalWriterEmptyLeaf(property, "sz", {"val"});
            size = wordAttribute(property, "val");
            recognized = recognized && size.has_value();
        } else if (isCanonicalWriterWordElement(property, "szCs")) {
            order = 6;
            recognized = canonicalWriterEmptyLeaf(property, "szCs", {"val"});
            complex_script_size = wordAttribute(property, "val");
            recognized = recognized && complex_script_size.has_value();
        } else if (isCanonicalWriterWordElement(property, "u")) {
            order = 7;
            recognized = canonicalWriterEmptyLeaf(property, "u", {"val"});
            const auto value = wordAttribute(property, "val");
            recognized = recognized && value &&
                         (*value == "single" || *value == "none");
        } else if (isCanonicalWriterWordElement(property, "shd")) {
            order = 8;
            recognized = canonicalWriterEmptyLeaf(
                property, "shd", {"val", "color", "fill"});
            const auto value = wordAttribute(property, "val");
            const auto color = wordAttribute(property, "color");
            const auto fill = wordAttribute(property, "fill");
            recognized = recognized && value && *value == "clear" &&
                         color && *color == "auto" && fill &&
                         canonicalWriterRgb(*fill);
        } else if (isCanonicalWriterWordElement(property, "vertAlign")) {
            order = 9;
            recognized = canonicalWriterEmptyLeaf(
                property, "vertAlign", {"val"});
            const auto value = wordAttribute(property, "val");
            recognized = recognized && value &&
                         (*value == "baseline" ||
                          *value == "superscript" ||
                          *value == "subscript");
        }
        if (!recognized || order <= previous_order) return false;
        previous_order = order;
        saw_property = true;
    }
    // appendBasicRunProperties always emits w:sz and w:szCs together with
    // the same value. Keeping that invariant here prevents a regeneration
    // from silently changing complex-script sizing.
    if (size.has_value() != complex_script_size.has_value() ||
        (size && size != complex_script_size)) {
        return false;
    }
    return saw_property;
}

bool canonicalWriterTextElement(const pugi::xml_node& text_node) {
    if (!isCanonicalWriterWordElement(text_node, "t")) return false;

    bool preserve_space = false;
    for (const pugi::xml_attribute attribute : text_node.attributes()) {
        if (preserve_space ||
            std::string_view(attribute.name()) != "xml:space" ||
            std::string_view(attribute.value()) != "preserve") {
            return false;
        }
        preserve_space = true;
    }

    std::string text;
    for (const pugi::xml_node child : text_node.children()) {
        if (child.type() != pugi::node_pcdata) return false;
        text += child.value();
    }
    if (text.find_first_of("\t\r\n") != std::string::npos ||
        text.find("\xe2\x80\xa8") != std::string::npos) {
        return false;
    }
    const bool needs_preserve = !text.empty() &&
                                (text.front() == ' ' || text.back() == ' ');
    return preserve_space == needs_preserve;
}

bool canonicalWriterRun(const pugi::xml_node& run) {
    if (!isCanonicalWriterWordElement(run, "r") ||
        !hasNoAttributes(run)) {
        return false;
    }

    bool saw_properties = false;
    bool saw_content = false;
    bool previous_was_text = false;
    for (const pugi::xml_node child : run.children()) {
        if (child.type() != pugi::node_element) return false;
        if (isCanonicalWriterWordElement(child, "rPr")) {
            if (saw_properties || saw_content ||
                !canonicalWriterRunProperties(child)) {
                return false;
            }
            saw_properties = true;
            previous_was_text = false;
            continue;
        }
        if (isCanonicalWriterWordElement(child, "t")) {
            // appendWordRunContents never creates adjacent w:t nodes inside
            // one run; it joins those characters into one text segment.
            if (previous_was_text || !canonicalWriterTextElement(child)) {
                return false;
            }
            previous_was_text = true;
        } else if (isCanonicalWriterWordElement(child, "tab")) {
            if (!canonicalWriterEmptyLeaf(child, "tab", {})) return false;
            previous_was_text = false;
        } else if (isCanonicalWriterWordElement(child, "br")) {
            if (!canonicalWriterEmptyLeaf(child, "br", {})) return false;
            previous_was_text = false;
        } else {
            // In particular, w:cr is imported as a soft line break but is not
            // emitted by the current writer, so it is not canonical-owned.
            return false;
        }
        saw_content = true;
    }
    return saw_content;
}

bool canonicalWriterParagraphProperties(const pugi::xml_node& properties) {
    if (!isCanonicalWriterWordElement(properties, "pPr") ||
        !hasNoAttributes(properties)) {
        return false;
    }

    int previous_order = -1;
    bool saw_property = false;
    for (const pugi::xml_node property : properties.children()) {
        if (property.type() != pugi::node_element) return false;

        int order = -1;
        bool recognized = false;
        if (isCanonicalWriterWordElement(property, "pStyle")) {
            order = 0;
            recognized =
                canonicalWriterEmptyLeaf(property, "pStyle", {"val"});
            const auto value = wordAttribute(property, "val");
            recognized = recognized && value &&
                         safeParagraphStyleIdToken(*value) &&
                         supportedBuiltInParagraphStyle(*value);
        } else if (isCanonicalWriterWordElement(property, "keepNext")) {
            order = 1;
            recognized = canonicalWriterOnOffProperty(property, "keepNext");
        } else if (isCanonicalWriterWordElement(property, "keepLines")) {
            order = 2;
            recognized = canonicalWriterOnOffProperty(property, "keepLines");
        } else if (isCanonicalWriterWordElement(property, "pageBreakBefore")) {
            order = 3;
            recognized = canonicalWriterOnOffProperty(
                property, "pageBreakBefore");
        } else if (isCanonicalWriterWordElement(property, "tabs")) {
            order = 4;
            recognized = hasNoAttributes(property);
            bool saw_tab = false;
            std::uint32_t previous_position = 0;
            for (const pugi::xml_node tab : property.children()) {
                if (tab.type() != pugi::node_element ||
                    !canonicalWriterEmptyLeaf(tab, "tab", {"val", "pos"})) {
                    recognized = false;
                    break;
                }
                const auto value = wordAttribute(tab, "val");
                bool parsed = true;
                const auto position = parseUnsignedIntegerAttribute(
                    tab, "pos", parsed);
                if (!value || *value != "left" || !parsed || !position ||
                    *position == 0 || *position > 31'680U ||
                    *position <= previous_position) {
                    recognized = false;
                    break;
                }
                previous_position = *position;
                saw_tab = true;
            }
            recognized = recognized && saw_tab;
        } else if (isCanonicalWriterWordElement(property, "spacing")) {
            order = 5;
            recognized = canonicalWriterEmptyLeaf(
                property, "spacing",
                {"before", "after", "line", "lineRule"});
            const auto before = wordAttribute(property, "before");
            const auto after = wordAttribute(property, "after");
            const auto line = wordAttribute(property, "line");
            const auto rule = wordAttribute(property, "lineRule");
            recognized = recognized && (before || after || line) &&
                         (!rule ||
                          (*rule == "auto" || *rule == "atLeast" ||
                           *rule == "exact")) &&
                         (!rule || line);
        } else if (isCanonicalWriterWordElement(property, "ind")) {
            order = 6;
            recognized = canonicalWriterEmptyLeaf(
                property, "ind",
                {"left", "right", "firstLine", "hanging"});
            const auto left = wordAttribute(property, "left");
            const auto right = wordAttribute(property, "right");
            const auto first_line = wordAttribute(property, "firstLine");
            const auto hanging = wordAttribute(property, "hanging");
            recognized = recognized &&
                         (left || right || first_line || hanging) &&
                         !(first_line && hanging);
        } else if (isCanonicalWriterWordElement(property, "jc")) {
            order = 7;
            recognized = canonicalWriterEmptyLeaf(property, "jc", {"val"});
            const auto value = wordAttribute(property, "val");
            recognized = recognized && value &&
                         (*value == "left" || *value == "center" ||
                          *value == "right" || *value == "both");
        } else if (isCanonicalWriterWordElement(property, "rPr")) {
            order = 8;
            recognized = canonicalWriterRunProperties(property);
        }
        if (!recognized || order <= previous_order) return false;
        previous_order = order;
        saw_property = true;
    }
    return saw_property;
}

bool canonicalWriterParagraph(const pugi::xml_node& paragraph) {
    if (!isCanonicalWriterWordElement(paragraph, "p") ||
        !hasNoAttributes(paragraph)) {
        return false;
    }

    bool saw_properties = false;
    bool saw_run = false;
    for (const pugi::xml_node child : paragraph.children()) {
        if (child.type() != pugi::node_element) return false;
        if (isCanonicalWriterWordElement(child, "pPr")) {
            if (saw_properties || saw_run ||
                !canonicalWriterParagraphProperties(child)) {
                return false;
            }
            saw_properties = true;
        } else if (isCanonicalWriterWordElement(child, "r")) {
            if (!canonicalWriterRun(child)) return false;
            saw_run = true;
        } else {
            return false;
        }
    }
    return true;
}

std::optional<std::uint32_t> canonicalDefaultTabStop(
    std::string_view settings_xml) {
    pugi::xml_document document;
    if (!document.load_buffer(
            settings_xml.data(), settings_xml.size(), pugi::parse_default,
            pugi::encoding_auto)) {
        return std::nullopt;
    }
    const pugi::xml_node root = document.document_element();
    if (!isWordElement(root, "settings")) return std::nullopt;

    pugi::xml_node tab_stop;
    for (const pugi::xml_node child : root.children()) {
        if (ignorableNode(child)) continue;
        if (!isWordElement(child, "defaultTabStop") || tab_stop) {
            return std::nullopt;
        }
        tab_stop = child;
    }
    if (!tab_stop || !hasOnlyIgnorableChildren(tab_stop) ||
        !hasOnlyWordAttributes(tab_stop, {"val"})) {
        return std::nullopt;
    }
    bool recognized = true;
    const auto value = parseUnsignedIntegerAttribute(
        tab_stop, "val", recognized);
    if (!recognized || !value || *value == 0 || *value > 31'680U) {
        return std::nullopt;
    }
    return value;
}

bool canonicalSimpleDocumentEnvelope(
    std::string_view document_xml, const ParsedPackage& parsed) {
    const std::string prefix =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"" + std::string(kWordNamespace) +
        "\" xmlns:m=\"" + std::string(kOfficeMathNamespace) +
        "\" xmlns:r=\"" + std::string(kOfficeRelationshipsNamespace) +
        "\" xmlns:wp=\"" + std::string(kWordprocessingDrawingNamespace) +
        "\" xmlns:a=\"" + std::string(kDrawingMainNamespace) +
        "\" xmlns:pic=\"" + std::string(kDrawingPictureNamespace) +
        "\"><w:body>";
    constexpr std::string_view suffix = "</w:body></w:document>";
    if (!document_xml.starts_with(prefix) ||
        !document_xml.ends_with(suffix)) {
        return false;
    }

    pugi::xml_document document;
    if (!document.load_buffer(
            document_xml.data(), document_xml.size(), pugi::parse_full,
            pugi::encoding_auto)) {
        return false;
    }
    const pugi::xml_node root = document.document_element();
    if (!isWordElement(root, "document")) return false;

    pugi::xml_node body;
    for (const pugi::xml_node child : root.children()) {
        if (!isCanonicalWriterWordElement(child, "body") || body) {
            return false;
        }
        body = child;
    }
    if (!body) return false;

    std::size_t paragraph_count = 0;
    pugi::xml_node section;
    for (const pugi::xml_node child : body.children()) {
        if (isCanonicalWriterWordElement(child, "p") && !section &&
            canonicalWriterParagraph(child)) {
            ++paragraph_count;
            continue;
        }
        if (isCanonicalWriterWordElement(child, "sectPr") && !section &&
            hasNoAttributes(child)) {
            section = child;
            continue;
        }
        return false;
    }
    if (!section || paragraph_count != parsed.body_blocks.size() ||
        parsed.sections.size() != 1U || !parsed.page_settings ||
        parsed.sections.front().first_body_block_index != 0U ||
        parsed.sections.front().body_block_count != paragraph_count) {
        return false;
    }
    if (!std::all_of(
            parsed.body_blocks.begin(), parsed.body_blocks.end(),
            [](const ImportedBodyBlock& block) {
                return std::holds_alternative<ImportedParagraphBlock>(block);
            })) {
        return false;
    }

    pugi::xml_node header_reference;
    pugi::xml_node footer_reference;
    pugi::xml_node page_size;
    pugi::xml_node page_margins;
    for (const pugi::xml_node child : section.children()) {
        if (isCanonicalWriterWordElement(child, "headerReference") &&
            !header_reference && !footer_reference && !page_size) {
            header_reference = child;
        } else if (isCanonicalWriterWordElement(child, "footerReference") &&
                   !footer_reference && !page_size) {
            footer_reference = child;
        } else if (isCanonicalWriterWordElement(child, "pgSz") && !page_size &&
            !page_margins) {
            page_size = child;
        } else if (isCanonicalWriterWordElement(child, "pgMar") &&
                   page_size && !page_margins) {
            page_margins = child;
        } else {
            return false;
        }
    }
    const auto canonical_story_reference = [](
        const pugi::xml_node& node, std::string_view expected_id) {
        if (!node || node.first_child()) return false;
        std::size_t attributes = 0;
        bool type = false;
        bool id = false;
        for (const pugi::xml_attribute attribute : node.attributes()) {
            ++attributes;
            const std::string_view name(attribute.name());
            if (name == "w:type" &&
                std::string_view(attribute.value()) == "default") {
                type = true;
            } else if (name == "r:id" &&
                       std::string_view(attribute.value()) == expected_id) {
                id = true;
            } else {
                return false;
            }
        }
        return attributes == 2U && type && id;
    };
    if (static_cast<bool>(header_reference) != parsed.header_text.has_value() ||
        static_cast<bool>(footer_reference) != parsed.footer_text.has_value() ||
        (header_reference && !canonical_story_reference(
             header_reference, "rIdHeader1")) ||
        (footer_reference && !canonical_story_reference(
             footer_reference, "rIdFooter1"))) {
        return false;
    }
    if (!page_size || !page_margins ||
        page_size.first_child() || page_margins.first_child() ||
        !hasOnlyCanonicalWriterWordAttributes(
            page_size, {"w", "h", "orient"}) ||
        !hasOnlyCanonicalWriterWordAttributes(
            page_margins,
            {"top", "right", "bottom", "left", "header", "footer",
             "gutter"}) ||
        !wordAttribute(page_size, "w") ||
        !wordAttribute(page_size, "h") ||
        !wordAttribute(page_margins, "top") ||
        !wordAttribute(page_margins, "right") ||
        !wordAttribute(page_margins, "bottom") ||
        !wordAttribute(page_margins, "left") ||
        wordAttribute(page_margins, "header") !=
            std::optional<std::string>{"720"} ||
        wordAttribute(page_margins, "footer") !=
            std::optional<std::string>{"720"} ||
        wordAttribute(page_margins, "gutter") !=
            std::optional<std::string>{"0"}) {
        return false;
    }
    const auto parsed_page = parsePageSettingsFromSection(section);
    if (!parsed_page || *parsed_page != *parsed.page_settings ||
        parsed.sections.front().page != *parsed.page_settings ||
        parsed.sections.front().break_kind != SectionBreakKind::next_page) {
        return false;
    }
    const auto orientation = wordAttribute(page_size, "orient");
    const bool landscape = parsed_page->width_twips > parsed_page->height_twips;
    if ((landscape && orientation !=
                          std::optional<std::string>{"landscape"}) ||
        (!landscape && orientation.has_value())) {
        return false;
    }

    if (parsed.paragraphs.size() != paragraph_count) return false;
    for (const auto& paragraph : parsed.paragraphs) {
        if (!paragraph.direct_body_child || paragraph.has_unsupported_content ||
            !paragraph.format_is_basic ||
            (paragraph.style_id &&
             (!safeParagraphStyleIdToken(*paragraph.style_id) ||
              !supportedBuiltInParagraphStyle(*paragraph.style_id))) ||
            paragraph.numbering || paragraph.numbering_id ||
            paragraph.numbering_level) {
            return false;
        }
        for (const auto& run : paragraph.runs) {
            if (run.has_unsupported_content || !run.format_is_basic ||
                run.style_id) {
                return false;
            }
            if (std::any_of(
                    run.fragments.begin(), run.fragments.end(),
                    [](const RunFragment& fragment) {
                        return fragment.kind == FragmentKind::equation ||
                               fragment.kind == FragmentKind::inline_image ||
                               fragment.kind == FragmentKind::page_break;
                    })) {
                return false;
            }
        }
    }
    return true;
}

std::optional<DocumentDefaults> canonicalSimpleRegenerationDefaults(
    const ParsedPackage& parsed, std::string_view content_types_xml,
    std::string_view root_relationships_xml,
    std::string_view document_relationships_xml,
    std::string_view styles_xml, std::string_view settings_xml,
    const ImportContext& context) {
    static constexpr std::array<std::string_view, 6> base_members{
        kContentTypesPart,
        kRootRelationshipsPart,
        kDocumentPart,
        kDocumentRelationshipsPart,
        "word/styles.xml",
        "word/settings.xml",
    };
    std::vector<std::string_view> expected_members(
        base_members.begin(), base_members.end());
    if (parsed.header_text) expected_members.push_back("word/header1.xml");
    if (parsed.footer_text) expected_members.push_back("word/footer1.xml");

    std::string expected_content_types(kNewContentTypes);
    std::string expected_relationships(kNewDocumentRelationships);
    constexpr std::string_view content_types_end = "</Types>";
    constexpr std::string_view relationships_end = "</Relationships>";
    const auto append_before = [](std::string& target, std::string_view ending,
                                  std::string_view addition) {
        const auto position = target.rfind(ending);
        if (position != std::string::npos) target.insert(position, addition);
    };
    if (parsed.header_text) {
        append_before(expected_content_types, content_types_end,
                      "<Override PartName=\"/word/header1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>");
        append_before(expected_relationships, relationships_end,
                      "<Relationship Id=\"rIdHeader1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/header\" Target=\"header1.xml\"/>");
    }
    if (parsed.footer_text) {
        append_before(expected_content_types, content_types_end,
                      "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>");
        append_before(expected_relationships, relationships_end,
                      "<Relationship Id=\"rIdFooter1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer\" Target=\"footer1.xml\"/>");
    }
    if (parsed.entries.size() != expected_members.size() ||
        parsed.compatibility.classification !=
            CompatibilityClass::basic_body_text_patch ||
        !parsed.compatibility.issues.empty() ||
        content_types_xml != expected_content_types ||
        root_relationships_xml != kNewRootRelationships ||
        document_relationships_xml != expected_relationships ||
        (parsed.header_text &&
         (!parsed.page_settings || parsed.header_xml != buildStoryXml(
             *parsed.header_text, true, *parsed.page_settings))) ||
        (parsed.footer_text &&
         (!parsed.page_settings || parsed.footer_xml != buildStoryXml(
             *parsed.footer_text, false, *parsed.page_settings)))) {
        return std::nullopt;
    }
    for (const auto name : expected_members) {
        if (std::none_of(
                parsed.entries.begin(), parsed.entries.end(),
                [name](const EntryRecord& entry) {
                    return entry.member.name == name;
                })) {
            return std::nullopt;
        }
    }

    const auto tab_stop = canonicalDefaultTabStop(settings_xml);
    if (!tab_stop || !context.default_run_format.font_family ||
        !context.default_run_format.font_size_half_points) {
        return std::nullopt;
    }
    DocumentDefaults defaults;
    defaults.font_family = *context.default_run_format.font_family;
    defaults.font_size_half_points =
        *context.default_run_format.font_size_half_points;
    defaults.default_tab_stop_twips = *tab_stop;
    NewParagraphStyleCatalog paragraph_styles;
    for (const auto& paragraph : parsed.paragraphs) {
        if (!paragraph.style_id) continue;
        if (!safeParagraphStyleIdToken(*paragraph.style_id) ||
            !supportedBuiltInParagraphStyle(*paragraph.style_id)) {
            return std::nullopt;
        }
        paragraph_styles.insert(*paragraph.style_id);
    }
    if (styles_xml != buildNewStylesXml(defaults, paragraph_styles) ||
        settings_xml != buildNewSettingsXml(defaults)) {
        return std::nullopt;
    }
    return canonicalSimpleDocumentEnvelope(parsed.document_xml, parsed)
        ? std::optional<DocumentDefaults>{std::move(defaults)}
        : std::nullopt;
}

bool inspectPackage(
    std::vector<std::uint8_t> bytes,
    std::uint32_t source_mode,
    const OpenOptions& options,
    ParsedPackage& parsed,
    Error* error) {
    zip_t* archive = openZipFromBytes(bytes, error);
    if (archive == nullptr) {
        return false;
    }

    const zip_int64_t signed_entry_count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (signed_entry_count < 0) {
        const std::string message = zipArchiveError(archive);
        zip_discard(archive);
        setError(error, ErrorCode::invalid_zip, "Cannot enumerate DOCX package: " + message);
        return false;
    }
    const auto entry_count = static_cast<std::uint64_t>(signed_entry_count);
    if (entry_count > options.max_member_count) {
        zip_discard(archive);
        setError(error, ErrorCode::package_limit_exceeded,
                 "DOCX package has more members than the configured limit");
        return false;
    }

    std::unordered_set<std::string> names;
    std::optional<std::size_t> document_index;
    std::optional<std::size_t> content_types_index;
    std::optional<std::size_t> root_relationships_index;
    std::optional<std::size_t> document_relationships_index;
    std::optional<std::size_t> styles_index;
    std::optional<std::size_t> settings_index;
    std::optional<std::size_t> numbering_index;
    std::optional<std::size_t> theme_index;
    bool has_encrypted_member = false;
    bool has_signature = false;
    std::uint64_t total_uncompressed_size = 0;
    parsed.entries.reserve(static_cast<std::size_t>(entry_count));

    for (zip_uint64_t index = 0; index < entry_count; ++index) {
        zip_stat_t status;
        zip_stat_init(&status);
        if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &status) != 0 ||
            (status.valid & ZIP_STAT_NAME) == 0 || status.name == nullptr) {
            const std::string message = zipArchiveError(archive);
            zip_discard(archive);
            setError(error, ErrorCode::invalid_zip, "Cannot inspect ZIP member: " + message);
            return false;
        }

        const char* utf8_name = zip_get_name(archive, index, ZIP_FL_ENC_UTF_8 | ZIP_FL_UNCHANGED);
        if (utf8_name == nullptr) {
            zip_discard(archive);
            setError(error, ErrorCode::invalid_zip,
                     "An OPC package member name is not valid UTF-8");
            return false;
        }
        const std::string name(utf8_name);
        if (!names.insert(name).second) {
            zip_discard(archive);
            setError(error, ErrorCode::duplicate_package_member,
                     "DOCX package contains duplicate member '" + name + "'");
            return false;
        }

        const std::uint64_t member_size =
            (status.valid & ZIP_STAT_SIZE) != 0 ? status.size : 0;
        if (member_size > options.max_member_uncompressed_bytes ||
            total_uncompressed_size > options.max_total_uncompressed_bytes -
                                          std::min(
                                              member_size,
                                              options.max_total_uncompressed_bytes)) {
            zip_discard(archive);
            setError(error, ErrorCode::package_limit_exceeded,
                     "DOCX package exceeds configured expansion limits");
            return false;
        }
        total_uncompressed_size += member_size;
        if (total_uncompressed_size > options.max_total_uncompressed_bytes) {
            zip_discard(archive);
            setError(error, ErrorCode::package_limit_exceeded,
                     "DOCX package exceeds the configured total expansion limit");
            return false;
        }

        EntryRecord entry;
        entry.index = index;
        entry.valid_fields = status.valid;
        entry.modified_time =
            (status.valid & ZIP_STAT_MTIME) != 0 ? static_cast<std::int64_t>(status.mtime) : 0;
        entry.member.name = name;
        entry.member.uncompressed_size = member_size;
        entry.member.compressed_size =
            (status.valid & ZIP_STAT_COMP_SIZE) != 0 ? status.comp_size : 0;
        entry.member.crc32 = (status.valid & ZIP_STAT_CRC) != 0 ? status.crc : 0;
        entry.member.compression_method =
            (status.valid & ZIP_STAT_COMP_METHOD) != 0 ? status.comp_method : 0;
        entry.member.encryption_method =
            (status.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 ? status.encryption_method : ZIP_EM_NONE;
        parsed.entries.push_back(std::move(entry));

        if (name == kDocumentPart) {
            document_index = static_cast<std::size_t>(index);
        } else if (name == kContentTypesPart) {
            content_types_index = static_cast<std::size_t>(index);
        } else if (name == kRootRelationshipsPart) {
            root_relationships_index = static_cast<std::size_t>(index);
        } else if (name == kDocumentRelationshipsPart) {
            document_relationships_index = static_cast<std::size_t>(index);
        } else if (name == "word/styles.xml") {
            styles_index = static_cast<std::size_t>(index);
        } else if (name == "word/settings.xml") {
            settings_index = static_cast<std::size_t>(index);
        } else if (name == "word/numbering.xml") {
            numbering_index = static_cast<std::size_t>(index);
        } else if (name == "word/theme/theme1.xml") {
            theme_index = static_cast<std::size_t>(index);
        }
        if (name.starts_with("_xmlsignatures/") || name == "_xmlsignatures") {
            has_signature = true;
        }
        if ((status.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 &&
            status.encryption_method != ZIP_EM_NONE) {
            has_encrypted_member = true;
        }
    }

    if (!content_types_index.has_value() || !root_relationships_index.has_value() ||
        !document_index.has_value()) {
        zip_discard(archive);
        std::string missing;
        if (!content_types_index.has_value()) {
            missing += " [Content_Types].xml";
        }
        if (!root_relationships_index.has_value()) {
            missing += " _rels/.rels";
        }
        if (!document_index.has_value()) {
            missing += " word/document.xml";
        }
        setError(error, ErrorCode::missing_opc_part, "DOCX package is missing required OPC part(s):" + missing);
        return false;
    }

    if (!readEntry(
            archive,
            static_cast<zip_uint64_t>(*document_index),
            options.max_document_xml_bytes,
            parsed.document_xml,
            error)) {
        zip_discard(archive);
        return false;
    }
    std::string content_types_xml;
    std::string relationships_xml;
    if (!readEntry(
            archive,
            static_cast<zip_uint64_t>(*content_types_index),
            options.max_document_xml_bytes,
            content_types_xml,
            error) ||
        !readEntry(
            archive,
            static_cast<zip_uint64_t>(*root_relationships_index),
            options.max_document_xml_bytes,
            relationships_xml,
            error)) {
        zip_discard(archive);
        return false;
    }
    std::string document_relationships_xml;
    if (document_relationships_index &&
        !readEntry(
            archive,
            static_cast<zip_uint64_t>(*document_relationships_index),
            options.max_document_xml_bytes,
            document_relationships_xml,
            error)) {
        zip_discard(archive);
        return false;
    }
    const auto related_parts = auxiliaryPartTargets(
        document_relationships_xml, options);
    const auto related_index = [&](const std::optional<std::string>& name)
        -> std::optional<std::size_t> {
        if (!name) return std::nullopt;
        const auto found = std::find_if(
            parsed.entries.begin(), parsed.entries.end(),
            [&name](const EntryRecord& entry) {
                return entry.member.name == *name;
            });
        return found == parsed.entries.end()
            ? std::nullopt
            : std::optional<std::size_t>(
                  static_cast<std::size_t>(found->index));
    };
    if (const auto related = related_index(related_parts.styles)) {
        styles_index = related;
    }
    if (const auto related = related_index(related_parts.numbering)) {
        numbering_index = related;
    }
    if (const auto related = related_index(related_parts.theme)) {
        theme_index = related;
    }
    const auto header_index = related_index(related_parts.header);
    const auto footer_index = related_index(related_parts.footer);
    std::string styles_xml;
    std::string settings_xml;
    std::string numbering_xml;
    std::string theme_xml;
    std::string header_xml;
    std::string footer_xml;
    const auto read_optional = [&](const std::optional<std::size_t>& index,
                                   std::string& destination) {
        return !index || readEntry(
            archive, static_cast<zip_uint64_t>(*index),
            options.max_document_xml_bytes, destination, error);
    };
    if (!read_optional(styles_index, styles_xml) ||
        !read_optional(settings_index, settings_xml) ||
        !read_optional(numbering_index, numbering_xml) ||
        !read_optional(theme_index, theme_xml) ||
        !read_optional(header_index, header_xml) ||
        !read_optional(footer_index, footer_xml)) {
        zip_discard(archive);
        return false;
    }
    zip_discard(archive);

    if (!validateOpcMetadata(content_types_xml, relationships_xml, options, error)) {
        return false;
    }

    parsed.bytes = std::move(bytes);
    parsed.source_mode = source_mode;
    parsed.document_entry_index = *document_index;
    parsed.compatibility.byte_identical_unchanged_save = true;
    parsed.compatibility.preserves_unmodified_package_members = true;

    if (has_encrypted_member) {
        parsed.compatibility.issues.push_back(CompatibilityIssue{
            IssueSeverity::blocking,
            IssueCode::encrypted_package_member,
            "",
            "Encrypted ZIP members can be retained by exact Save As but are not rebuilt after edits",
            std::nullopt});
    }
    if (has_signature) {
        parsed.compatibility.issues.push_back(CompatibilityIssue{
            IssueSeverity::blocking,
            IssueCode::digital_signature,
            "_xmlsignatures/",
            "Editing word/document.xml would invalidate the OPC digital signature",
            std::nullopt});
    }

    const EntryRecord& document_entry = parsed.entries[*document_index];
    if ((document_entry.valid_fields & ZIP_STAT_COMP_METHOD) != 0 &&
        document_entry.member.compression_method != ZIP_CM_STORE &&
        document_entry.member.compression_method != ZIP_CM_DEFLATE) {
        parsed.compatibility.issues.push_back(CompatibilityIssue{
            IssueSeverity::blocking,
            IssueCode::unsupported_document_compression,
            std::string(kDocumentPart),
            "The document part uses a compression method that this writer will not recreate",
            std::nullopt});
    }

    ImportContext import_context;
    import_context.theme = parseThemeData(theme_xml, options);
    parseStylesData(styles_xml, options, import_context);
    parseNumberingData(numbering_xml, options, import_context);

    if (!parseDocumentXml(
            parsed.document_xml,
            parsed.paragraphs,
            parsed.body_blocks,
            parsed.spans,
            parsed.compatibility,
            import_context,
            options,
            error)) {
        return false;
    }
    resolveInlineImages(parsed, options);
    parsed.sections = parseBodySections(parsed.document_xml);
    parsed.page_settings = parseBodyPageSettings(parsed.document_xml);
    parsed.header_xml = header_xml;
    parsed.footer_xml = footer_xml;
    parsed.header_text = parseSimpleStoryText(header_xml, options);
    parsed.footer_text = parseSimpleStoryText(footer_xml, options);
    parsed.canonical_simple_regeneration_defaults =
        canonicalSimpleRegenerationDefaults(
            parsed, content_types_xml, relationships_xml,
            document_relationships_xml, styles_xml, settings_xml,
            import_context);

    const bool global_blocker = std::any_of(
        parsed.compatibility.issues.begin(),
        parsed.compatibility.issues.end(),
        [](const CompatibilityIssue& issue) { return issue.severity == IssueSeverity::blocking; });
    if (global_blocker) {
        parsed.compatibility.classification = CompatibilityClass::exact_round_trip_only;
    }
    return true;
}

bool isValidUtf8XmlText(std::string_view text, IssueCode& issue_code, std::string& detail) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        if (first <= 0x7fU) {
            code_point = first;
            length = 1;
        } else if ((first & 0xe0U) == 0xc0U) {
            code_point = first & 0x1fU;
            length = 2;
            if (code_point == 0) {
                issue_code = IssueCode::invalid_utf8;
                detail = "Replacement text contains an overlong UTF-8 sequence";
                return false;
            }
        } else if ((first & 0xf0U) == 0xe0U) {
            code_point = first & 0x0fU;
            length = 3;
        } else if ((first & 0xf8U) == 0xf0U) {
            code_point = first & 0x07U;
            length = 4;
        } else {
            issue_code = IssueCode::invalid_utf8;
            detail = "Replacement text is not valid UTF-8";
            return false;
        }
        if (index + length > text.size()) {
            issue_code = IssueCode::invalid_utf8;
            detail = "Replacement text ends inside a UTF-8 sequence";
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                issue_code = IssueCode::invalid_utf8;
                detail = "Replacement text contains an invalid UTF-8 continuation byte";
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if ((length == 2 && code_point < 0x80U) || (length == 3 && code_point < 0x800U) ||
            (length == 4 && code_point < 0x10000U) || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            issue_code = IssueCode::invalid_utf8;
            detail = "Replacement text contains a non-canonical UTF-8 scalar value";
            return false;
        }
        const bool xml_character = code_point == 0x09U || code_point == 0x0aU || code_point == 0x0dU ||
                                   (code_point >= 0x20U && code_point <= 0xd7ffU) ||
                                   (code_point >= 0xe000U && code_point <= 0xfffdU) ||
                                   (code_point >= 0x10000U && code_point <= 0x10ffffU);
        if (!xml_character) {
            issue_code = IssueCode::invalid_xml_character;
            detail = "Replacement text contains a character forbidden by XML 1.0";
            return false;
        }
        index += length;
    }
    return true;
}

std::string escapeXmlText(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

void appendWordRunContents(std::ostringstream& output, std::string_view text) {
    const auto append_text = [&](std::string_view segment) {
        const bool preserve_space = !segment.empty() &&
                                    (segment.front() == ' ' || segment.back() == ' ');
        output << "<w:t" << (preserve_space ? " xml:space=\"preserve\"" : "")
               << ">" << escapeXmlText(segment) << "</w:t>";
    };

    if (text.empty()) {
        append_text({});
        return;
    }

    std::size_t segment_begin = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::string_view element;
        std::size_t separator_size = 0;
        if (text[cursor] == '\t') {
            element = "<w:tab/>";
            separator_size = 1;
        } else if (text[cursor] == '\r') {
            element = "<w:br/>";
            separator_size = cursor + 1 < text.size() && text[cursor + 1] == '\n' ? 2 : 1;
        } else if (text[cursor] == '\n') {
            element = "<w:br/>";
            separator_size = 1;
        } else if (cursor + 3 <= text.size() &&
                   static_cast<unsigned char>(text[cursor]) == 0xe2U &&
                   static_cast<unsigned char>(text[cursor + 1]) == 0x80U &&
                   static_cast<unsigned char>(text[cursor + 2]) == 0xa8U) {
            element = "<w:br/>";
            separator_size = 3;
        }

        if (separator_size == 0) {
            ++cursor;
            continue;
        }
        if (cursor > segment_begin) {
            append_text(text.substr(segment_begin, cursor - segment_begin));
        }
        output << element;
        cursor += separator_size;
        segment_begin = cursor;
    }
    if (segment_begin < text.size()) {
        append_text(text.substr(segment_begin));
    }
}

std::string escapeXmlAttribute(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

LossReport globalBlockingIssues(const CompatibilityReport& report) {
    LossReport loss;
    std::copy_if(
        report.issues.begin(),
        report.issues.end(),
        std::back_inserter(loss.issues),
        [](const CompatibilityIssue& issue) { return issue.severity == IssueSeverity::blocking; });
    return loss;
}

CompatibilityIssue blockingIssue(
    IssueCode code,
    std::string detail,
    std::optional<std::size_t> paragraph = std::nullopt) {
    return CompatibilityIssue{
        IssueSeverity::blocking, code, std::string(kDocumentPart), std::move(detail), paragraph};
}

class AtomicTempFile {
public:
    AtomicTempFile() = default;
    AtomicTempFile(std::filesystem::path path, int descriptor)
        : path_(std::move(path)), descriptor_(descriptor) {}
    ~AtomicTempFile() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        if (!committed_ && !path_.empty()) {
            ::unlink(path_.c_str());
        }
    }
    AtomicTempFile(const AtomicTempFile&) = delete;
    AtomicTempFile& operator=(const AtomicTempFile&) = delete;
    AtomicTempFile(AtomicTempFile&& other) noexcept
        : path_(std::move(other.path_)), descriptor_(std::exchange(other.descriptor_, -1)),
          committed_(std::exchange(other.committed_, true)) {}
    AtomicTempFile& operator=(AtomicTempFile&&) = delete;

    static std::optional<AtomicTempFile> create(
        const std::filesystem::path& target,
        std::uint32_t preferred_mode,
        Error* error) {
        std::error_code filesystem_error;
        const std::filesystem::path absolute_target = std::filesystem::absolute(target, filesystem_error);
        if (filesystem_error || absolute_target.filename().empty()) {
            setError(error, ErrorCode::io_error, "Invalid DOCX save target");
            return std::nullopt;
        }
        const std::filesystem::path parent = absolute_target.parent_path();
        std::string pattern =
            (parent / ("." + absolute_target.filename().string() + ".tmp.XXXXXX")).string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const int descriptor = ::mkstemp(writable.data());
        if (descriptor < 0) {
            setError(error, ErrorCode::io_error, errnoMessage("Cannot create temporary save file for", target));
            return std::nullopt;
        }
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        if (descriptor_flags >= 0) {
            (void)::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC);
        }

        struct stat target_status {};
        std::uint32_t mode = preferred_mode == 0 ? 0600U : preferred_mode;
        if (::stat(absolute_target.c_str(), &target_status) == 0 && S_ISREG(target_status.st_mode)) {
            mode = static_cast<std::uint32_t>(target_status.st_mode & 0777U);
        }
        (void)::fchmod(descriptor, static_cast<mode_t>(mode));
        return AtomicTempFile(std::filesystem::path(writable.data()), descriptor);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

    bool closeDescriptor(Error* error) {
        if (descriptor_ < 0) {
            return true;
        }
        if (::close(descriptor_) != 0) {
            descriptor_ = -1;
            setError(error, ErrorCode::io_error, errnoMessage("Cannot close temporary file", path_));
            return false;
        }
        descriptor_ = -1;
        return true;
    }

    bool syncClosedFile(Error* error) const {
        const int descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
            setError(error, ErrorCode::io_error, errnoMessage("Cannot reopen temporary file", path_));
            return false;
        }
        const bool success = ::fsync(descriptor) == 0;
        const int saved_errno = errno;
        ::close(descriptor);
        if (!success) {
            errno = saved_errno;
            setError(error, ErrorCode::io_error, errnoMessage("Cannot sync temporary file", path_));
        }
        return success;
    }

    bool commit(const std::filesystem::path& target, Error* error) {
        std::error_code filesystem_error;
        const std::filesystem::path absolute_target = std::filesystem::absolute(target, filesystem_error);
        if (filesystem_error) {
            setError(error, ErrorCode::atomic_commit_failed,
                     "Cannot resolve atomic save target: " + filesystem_error.message());
            return false;
        }
        if (::rename(path_.c_str(), absolute_target.c_str()) != 0) {
            setError(error, ErrorCode::atomic_commit_failed,
                     errnoMessage("Cannot atomically replace", target));
            return false;
        }
        committed_ = true;

        const int directory = ::open(
            absolute_target.parent_path().c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) {
            const int native_error = errno;
            setError(
                error,
                ErrorCode::atomic_commit_failed,
                "DOCX was renamed into place, but its destination directory could not be "
                "opened for a durability sync: " +
                    std::string(std::strerror(native_error)));
            return false;
        }
        int sync_result = 0;
        do {
            sync_result = ::fsync(directory);
        } while (sync_result != 0 && errno == EINTR);
        const int sync_error = sync_result == 0 ? 0 : errno;
        const int close_result = ::close(directory);
        const int close_error = close_result == 0 ? 0 : errno;
        if (sync_error != 0 || close_error != 0) {
            std::ostringstream message;
            message << "DOCX was renamed into place, but the destination directory "
                    << (sync_error != 0 ? "could not be synced" : "could not be closed")
                    << ": " << std::strerror(sync_error != 0 ? sync_error : close_error);
            setError(error, ErrorCode::atomic_commit_failed, message.str());
            return false;
        }
        return true;
    }

private:
    std::filesystem::path path_;
    int descriptor_{-1};
    bool committed_{false};
};

bool writeAll(int descriptor, const std::vector<std::uint8_t>& bytes, Error* error) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(
            descriptor,
            bytes.data() + written,
            std::min<std::size_t>(bytes.size() - written, static_cast<std::size_t>(SSIZE_MAX)));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            setError(error, ErrorCode::io_error, "Cannot write complete temporary DOCX file");
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        setError(error, ErrorCode::io_error, "Cannot sync temporary DOCX file");
        return false;
    }
    return true;
}

bool copyEntryMetadata(
    zip_t* destination,
    zip_uint64_t destination_index,
    zip_t* source,
    zip_uint64_t source_index,
    const EntryRecord& record,
    Error* error) {
    if ((record.valid_fields & ZIP_STAT_MTIME) != 0 &&
        zip_file_set_mtime(
            destination,
            destination_index,
            static_cast<time_t>(record.modified_time),
            0) != 0) {
        setError(error, ErrorCode::io_error,
                 "Cannot retain ZIP member timestamp: " + zipArchiveError(destination));
        return false;
    }

    zip_uint8_t operating_system = 0;
    zip_uint32_t attributes = 0;
    if (zip_file_get_external_attributes(
            source, source_index, ZIP_FL_UNCHANGED, &operating_system, &attributes) == 0 &&
        zip_file_set_external_attributes(
            destination, destination_index, 0, operating_system, attributes) != 0) {
        setError(error, ErrorCode::io_error,
                 "Cannot retain ZIP member attributes: " + zipArchiveError(destination));
        return false;
    }

    zip_uint32_t comment_length = 0;
    const char* comment =
        zip_file_get_comment(source, source_index, &comment_length, ZIP_FL_UNCHANGED | ZIP_FL_ENC_UTF_8);
    if (comment != nullptr && comment_length > 0 &&
        zip_file_set_comment(
            destination,
            destination_index,
            comment,
            static_cast<zip_uint16_t>(comment_length),
            ZIP_FL_ENC_UTF_8) != 0) {
        setError(error, ErrorCode::io_error,
                 "Cannot retain ZIP member comment: " + zipArchiveError(destination));
        return false;
    }
    return true;
}

bool writeModifiedArchive(
    const std::filesystem::path& path,
    const ParsedPackage& package,
    std::string_view modified_document_xml,
    Error* error) {
    zip_t* source = openZipFromBytes(package.bytes, error);
    if (source == nullptr) {
        return false;
    }

    int open_error = 0;
    zip_t* destination = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &open_error);
    if (destination == nullptr) {
        zip_discard(source);
        setError(error, ErrorCode::io_error,
                 "Cannot create temporary DOCX archive: " + zipCodeError(open_error));
        return false;
    }

    int archive_comment_length = 0;
    const char* archive_comment =
        zip_get_archive_comment(source, &archive_comment_length, ZIP_FL_UNCHANGED | ZIP_FL_ENC_UTF_8);
    if (archive_comment != nullptr && archive_comment_length > 0 &&
        zip_set_archive_comment(
            destination,
            archive_comment,
            static_cast<zip_uint16_t>(archive_comment_length)) != 0) {
        setError(error, ErrorCode::io_error,
                 "Cannot retain ZIP archive comment: " + zipArchiveError(destination));
        zip_discard(destination);
        zip_discard(source);
        return false;
    }

    for (std::size_t position = 0; position < package.entries.size(); ++position) {
        const EntryRecord& record = package.entries[position];
        zip_source_t* member_source = nullptr;
        if (position == package.document_entry_index) {
            member_source = zip_source_buffer(
                destination,
                modified_document_xml.empty() ? nullptr : modified_document_xml.data(),
                static_cast<zip_uint64_t>(modified_document_xml.size()),
                0);
        } else {
            member_source = zip_source_zip(
                destination,
                source,
                record.index,
                ZIP_FL_UNCHANGED,
                0,
                -1);
        }
        if (member_source == nullptr) {
            setError(error, ErrorCode::io_error,
                     "Cannot copy package member '" + record.member.name + "': " +
                         zipArchiveError(destination));
            zip_discard(destination);
            zip_discard(source);
            return false;
        }

        const zip_int64_t added = zip_file_add(
            destination, record.member.name.c_str(), member_source, ZIP_FL_ENC_UTF_8);
        if (added < 0) {
            zip_source_free(member_source);
            setError(error, ErrorCode::io_error,
                     "Cannot add package member '" + record.member.name + "': " +
                         zipArchiveError(destination));
            zip_discard(destination);
            zip_discard(source);
            return false;
        }
        const auto destination_index = static_cast<zip_uint64_t>(added);

        if (position == package.document_entry_index &&
            zip_set_file_compression(
                destination,
                destination_index,
                static_cast<zip_int32_t>(record.member.compression_method),
                0) != 0) {
            setError(error, ErrorCode::io_error,
                     "Cannot retain document.xml compression method: " + zipArchiveError(destination));
            zip_discard(destination);
            zip_discard(source);
            return false;
        }
        if (!copyEntryMetadata(
                destination, destination_index, source, record.index, record, error)) {
            zip_discard(destination);
            zip_discard(source);
            return false;
        }
    }

    if (zip_close(destination) != 0) {
        const std::string message = zipArchiveError(destination);
        zip_discard(destination);
        zip_discard(source);
        setError(error, ErrorCode::io_error, "Cannot finalize temporary DOCX archive: " + message);
        return false;
    }
    zip_discard(source);
    return true;
}

bool validateSavedPackage(
    const std::filesystem::path& path,
    const ParsedPackage* expected_package,
    std::string_view expected_document_xml,
    const std::vector<std::uint8_t>* expected_exact_bytes,
    const OpenOptions& options,
    Error* error) {
    if (expected_exact_bytes != nullptr) {
        std::vector<std::uint8_t> actual_bytes;
        std::uint32_t ignored_mode = 0;
        if (!readFile(path, options.max_package_bytes, actual_bytes, ignored_mode, error)) {
            return false;
        }
        if (actual_bytes != *expected_exact_bytes) {
            setError(error, ErrorCode::save_validation_failed,
                     "Unchanged Save As did not reproduce the opened file byte for byte");
            return false;
        }
    }

    int open_error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &open_error);
    if (archive == nullptr) {
        setError(error, ErrorCode::save_validation_failed,
                 "Saved file cannot be reopened as ZIP: " + zipCodeError(open_error));
        return false;
    }

    const zip_int64_t entry_count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (entry_count < 0 ||
        (expected_package != nullptr &&
         static_cast<std::size_t>(entry_count) != expected_package->entries.size())) {
        zip_discard(archive);
        setError(error, ErrorCode::save_validation_failed,
                 "Saved package member count differs from the opened package");
        return false;
    }

    std::optional<zip_uint64_t> document_index;
    bool has_content_types = false;
    bool has_root_relationships = false;
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(entry_count); ++index) {
        const char* name = zip_get_name(archive, index, ZIP_FL_UNCHANGED | ZIP_FL_ENC_UTF_8);
        if (name == nullptr) {
            zip_discard(archive);
            setError(error, ErrorCode::save_validation_failed,
                     "Saved package contains a non-UTF-8 member name");
            return false;
        }
        if (std::string_view(name) == kDocumentPart) {
            document_index = index;
        } else if (std::string_view(name) == kContentTypesPart) {
            has_content_types = true;
        } else if (std::string_view(name) == kRootRelationshipsPart) {
            has_root_relationships = true;
        }

        if (expected_package == nullptr) {
            continue;
        }
        const EntryRecord& expected = expected_package->entries[static_cast<std::size_t>(index)];
        if (expected.member.name != name) {
            zip_discard(archive);
            setError(error, ErrorCode::save_validation_failed,
                     "Saved package changed member order or names");
            return false;
        }

        zip_stat_t actual;
        zip_stat_init(&actual);
        if (zip_stat_index(archive, index, ZIP_FL_UNCHANGED, &actual) != 0) {
            zip_discard(archive);
            setError(error, ErrorCode::save_validation_failed,
                     "Cannot inspect a member in the saved package");
            return false;
        }
        if (index != expected_package->document_entry_index) {
            const bool same_size =
                (expected.valid_fields & ZIP_STAT_SIZE) == 0 ||
                ((actual.valid & ZIP_STAT_SIZE) != 0 && actual.size == expected.member.uncompressed_size);
            const bool same_crc =
                (expected.valid_fields & ZIP_STAT_CRC) == 0 ||
                ((actual.valid & ZIP_STAT_CRC) != 0 && actual.crc == expected.member.crc32);
            const bool same_method =
                (expected.valid_fields & ZIP_STAT_COMP_METHOD) == 0 ||
                ((actual.valid & ZIP_STAT_COMP_METHOD) != 0 &&
                 actual.comp_method == expected.member.compression_method);
            const bool same_encryption =
                (expected.valid_fields & ZIP_STAT_ENCRYPTION_METHOD) == 0 ||
                ((actual.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 &&
                 actual.encryption_method == expected.member.encryption_method);
            if (!same_size || !same_crc || !same_method || !same_encryption) {
                zip_discard(archive);
                setError(error, ErrorCode::save_validation_failed,
                         "Saved package changed an opaque member: " + expected.member.name);
                return false;
            }
        }
    }

    if (!document_index.has_value() || !has_content_types || !has_root_relationships) {
        zip_discard(archive);
        setError(error, ErrorCode::save_validation_failed,
                 "Saved package lost a required OPC part");
        return false;
    }
    std::string actual_document_xml;
    if (!readEntry(
            archive,
            *document_index,
            options.max_document_xml_bytes,
            actual_document_xml,
            error)) {
        zip_discard(archive);
        if (error != nullptr) {
            error->code = ErrorCode::save_validation_failed;
        }
        return false;
    }
    zip_discard(archive);
    if (actual_document_xml != expected_document_xml) {
        setError(error, ErrorCode::save_validation_failed,
                 "Saved word/document.xml differs from the requested patch");
        return false;
    }

    std::vector<Paragraph> ignored_paragraphs;
    std::vector<ImportedBodyBlock> ignored_body_blocks;
    std::vector<SpanLocation> ignored_spans;
    CompatibilityReport ignored_report;
    const ImportContext empty_import_context;
    Error parse_error;
    if (!parseDocumentXml(
            actual_document_xml,
            ignored_paragraphs,
            ignored_body_blocks,
            ignored_spans,
            ignored_report,
            empty_import_context,
            options,
            &parse_error)) {
        setError(error, ErrorCode::save_validation_failed,
                 "Saved document XML failed validation: " + parse_error.message);
        return false;
    }
    return true;
}

std::string applyTextPatches(
    std::string document_xml,
    const std::vector<SpanLocation>& spans,
    const std::unordered_map<TextSpanId, std::string>& edits) {
    struct Patch {
        std::size_t begin;
        std::size_t end;
        std::string replacement;
    };
    std::vector<Patch> patches;
    patches.reserve(edits.size());
    for (const SpanLocation& span : spans) {
        const auto found = edits.find(span.id);
        if (found != edits.end()) {
            patches.push_back(Patch{span.content_begin, span.content_end, escapeXmlText(found->second)});
        }
    }
    std::sort(patches.begin(), patches.end(), [](const Patch& left, const Patch& right) {
        return left.begin > right.begin;
    });
    for (const Patch& patch : patches) {
        document_xml.replace(patch.begin, patch.end - patch.begin, patch.replacement);
    }
    return document_xml;
}

bool addBufferMember(
    zip_t* archive,
    const std::string& name,
    std::string_view contents,
    Error* error) {
    zip_source_t* source = zip_source_buffer(
        archive,
        contents.empty() ? nullptr : contents.data(),
        static_cast<zip_uint64_t>(contents.size()),
        0);
    if (source == nullptr) {
        setError(error, ErrorCode::io_error,
                 "Cannot allocate ZIP member '" + name + "': " + zipArchiveError(archive));
        return false;
    }
    const zip_int64_t index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        setError(error, ErrorCode::io_error,
                 "Cannot add ZIP member '" + name + "': " + zipArchiveError(archive));
        return false;
    }
    if (zip_set_file_compression(
            archive, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 6) != 0) {
        setError(error, ErrorCode::io_error,
                 "Cannot compress ZIP member '" + name + "': " + zipArchiveError(archive));
        return false;
    }
    return true;
}

struct AuthoredImagePart {
    const NewInlineImage* image{nullptr};
    std::string relationship_id;
    std::string package_member;
    std::string relationship_target;
    std::string content_type;
};

bool validImageLayout(const ImageLayout& layout) {
    switch (layout.placement) {
        case ImagePlacement::inline_with_text:
        case ImagePlacement::square:
        case ImagePlacement::top_and_bottom:
            break;
        default:
            return false;
    }
    const auto valid_distance = [](std::int64_t distance) {
        return distance >= 0 &&
            distance <= kMaximumImageWrapDistanceEmu;
    };
    return valid_distance(layout.distance_top_emu) &&
        valid_distance(layout.distance_right_emu) &&
        valid_distance(layout.distance_bottom_emu) &&
        valid_distance(layout.distance_left_emu) &&
        (layout.placement != ImagePlacement::inline_with_text ||
         layout.move_with_text);
}

bool validateNewInlineImage(
    const NewInlineImage& image, std::size_t paragraph_index,
    LossReport& loss, Error* error) {
    IssueCode name_issue_code = IssueCode::invalid_utf8;
    std::string name_detail;
    const bool valid_name = !image.name.empty() && image.name.size() <= 255U &&
        isValidUtf8XmlText(image.name, name_issue_code, name_detail) &&
        image.name != "." && image.name != ".." &&
        image.name.find('/') == std::string::npos &&
        image.name.find('\\') == std::string::npos;
    IssueCode accessible_name_issue_code = IssueCode::invalid_utf8;
    std::string accessible_name_detail;
    const bool valid_accessible_name =
        image.accessible_name.size() <=
            kMaximumImageAccessibleNameBytes &&
        isValidUtf8XmlText(
            image.accessible_name, accessible_name_issue_code,
            accessible_name_detail);
    const std::int64_t maximum_extent =
        image.layout.placement == ImagePlacement::inline_with_text
        ? kMaximumInlineExtentEmu
        : kMaximumImageDimensionEmu;
    const bool valid_extent = image.width_emu > 0 && image.height_emu > 0 &&
        image.width_emu <= maximum_extent &&
        image.height_emu <= maximum_extent;
    const bool valid_size = !image.bytes.empty() &&
        image.bytes.size() <= NewInlineImage::maximum_encoded_bytes;
    const bool supported_format =
        image.format == RasterImageFormat::png ||
        image.format == RasterImageFormat::jpeg;
    raster::ValidationLimits raster_limits;
    raster_limits.maximum_encoded_bytes =
        NewInlineImage::maximum_encoded_bytes;
    const auto raster_inspection = supported_format
        ? raster::inspect(image.bytes, image.format, raster_limits)
        : raster::Inspection{};
    const bool valid_encoding = supported_format && raster_inspection.ok();
    const bool valid_layout = validImageLayout(image.layout);
    if (valid_name && valid_accessible_name && valid_extent && valid_size &&
        valid_encoding && valid_layout) {
        return true;
    }

    std::string message;
    IssueCode issue_code = IssueCode::structural_rewrite_required;
    if (!valid_name) {
        message = name_detail.empty()
            ? "Inline image name must be safe metadata, not a path"
            : name_detail;
        if (!name_detail.empty()) issue_code = name_issue_code;
    } else if (!valid_accessible_name) {
        message = accessible_name_detail.empty()
            ? "Image alt text exceeds the 4 KiB metadata limit"
            : accessible_name_detail;
        if (!accessible_name_detail.empty()) {
            issue_code = accessible_name_issue_code;
        }
    } else if (!valid_extent) {
        message = "Inline image display dimensions are outside the supported range";
    } else if (!valid_size) {
        message = "Inline image encoded bytes are empty or exceed 64 MiB";
    } else if (!supported_format) {
        message = "Inline image format is unsupported";
    } else if (!valid_encoding) {
        message = image.format == RasterImageFormat::png
            ? "Inline image bytes do not match a supported bounded PNG"
            : "Inline image bytes do not match a supported bounded JPEG";
    } else {
        message =
            "Image placement or wrap distance is outside the supported subset";
    }
    loss.issues.push_back(blockingIssue(
        issue_code, message, paragraph_index));
    setError(error, ErrorCode::unsafe_edit, message);
    return false;
}

bool collectParagraphImages(
    const NewParagraph& paragraph, std::size_t paragraph_index,
    std::vector<AuthoredImagePart>& images, LossReport& loss, Error* error) {
    for (const auto& run : paragraph.runs) {
        if (!run.inline_image) continue;
        if (!validateNewInlineImage(
                *run.inline_image, paragraph_index, loss, error)) {
            return false;
        }
        const std::size_t number = images.size() + 1U;
        const bool png = run.inline_image->format == RasterImageFormat::png;
        const std::string extension = png ? "png" : "jpg";
        images.push_back(AuthoredImagePart{
            &*run.inline_image,
            "rIdImage" + std::to_string(number),
            "word/media/image" + std::to_string(number) + "." + extension,
            "media/image" + std::to_string(number) + "." + extension,
            png ? "image/png" : "image/jpeg"});
    }
    return true;
}

bool collectBodyImages(
    const NewDocumentBody& body, std::vector<AuthoredImagePart>& images,
    LossReport& loss, Error* error) {
    std::size_t paragraph_index = 0;
    for (const auto& block : body.blocks) {
        if (const auto* paragraph = std::get_if<NewParagraph>(&block)) {
            if (!collectParagraphImages(
                    *paragraph, paragraph_index++, images, loss, error)) {
                return false;
            }
            continue;
        }
        for (const auto& paragraph : std::get<NewTable>(block).cell_paragraphs) {
            if (!collectParagraphImages(
                    paragraph, paragraph_index++, images, loss, error)) {
                return false;
            }
        }
    }
    return true;
}

bool writeNewArchive(
    const std::filesystem::path& path,
    std::string_view content_types,
    std::string_view root_relationships,
    std::string_view document_relationships,
    std::string_view styles_xml,
    std::string_view settings_xml,
    std::string_view numbering_xml,
    std::string_view document_xml,
    std::string_view header_xml,
    std::string_view footer_xml,
    const std::vector<AuthoredImagePart>& images,
    Error* error) {
    int open_error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &open_error);
    if (archive == nullptr) {
        setError(error, ErrorCode::io_error,
                 "Cannot create new DOCX archive: " + zipCodeError(open_error));
        return false;
    }
    if (!addBufferMember(archive, std::string(kContentTypesPart), content_types, error) ||
        !addBufferMember(archive, std::string(kRootRelationshipsPart), root_relationships, error) ||
        !addBufferMember(archive, std::string(kDocumentPart), document_xml, error) ||
        !addBufferMember(
            archive, "word/_rels/document.xml.rels", document_relationships, error) ||
        !addBufferMember(archive, "word/styles.xml", styles_xml, error) ||
        !addBufferMember(archive, "word/settings.xml", settings_xml, error) ||
        (!header_xml.empty() &&
         !addBufferMember(archive, "word/header1.xml", header_xml, error)) ||
        (!footer_xml.empty() &&
         !addBufferMember(archive, "word/footer1.xml", footer_xml, error)) ||
        (!numbering_xml.empty() &&
         !addBufferMember(
             archive, "word/numbering.xml", numbering_xml, error))) {
        zip_discard(archive);
        return false;
    }
    for (const auto& part : images) {
        const std::string_view contents(
            reinterpret_cast<const char*>(part.image->bytes.data()),
            part.image->bytes.size());
        if (!addBufferMember(
                archive, part.package_member, contents, error)) {
            zip_discard(archive);
            return false;
        }
    }
    if (zip_close(archive) != 0) {
        const std::string message = zipArchiveError(archive);
        zip_discard(archive);
        setError(error, ErrorCode::io_error, "Cannot finalize new DOCX archive: " + message);
        return false;
    }
    return true;
}

std::string rgbHex(std::uint32_t rgb) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << rgb;
    return stream.str();
}

void appendBasicRunProperties(std::ostringstream& output, const BasicRunFormat& format) {
    const bool has_format = format.font_family.has_value() ||
                            format.font_size_half_points.has_value() ||
                            format.bold.has_value() || format.italic.has_value() ||
                            format.underline.has_value() || format.strike.has_value() ||
                            format.foreground_rgb.has_value() ||
                            format.highlight_rgb.has_value() || format.baseline.has_value();
    if (!has_format) return;

    output << "<w:rPr>";
    if (format.font_family.has_value()) {
        const std::string family = escapeXmlAttribute(*format.font_family);
        output << "<w:rFonts w:ascii=\"" << family << "\" w:hAnsi=\"" << family
               << "\"/>";
    }
    const auto append_on_off = [&](std::string_view name,
                                   const std::optional<bool>& value) {
        if (!value.has_value()) return;
        output << "<w:" << name;
        if (!*value) output << " w:val=\"0\"";
        output << "/>";
    };
    append_on_off("b", format.bold);
    append_on_off("i", format.italic);
    append_on_off("strike", format.strike);
    if (format.foreground_rgb.has_value()) {
        output << "<w:color w:val=\"" << rgbHex(*format.foreground_rgb) << "\"/>";
    }
    if (format.font_size_half_points.has_value()) {
        output << "<w:sz w:val=\"" << *format.font_size_half_points << "\"/>"
               << "<w:szCs w:val=\"" << *format.font_size_half_points << "\"/>";
    }
    if (format.underline.has_value()) {
        output << "<w:u w:val=\""
               << (*format.underline ? "single" : "none") << "\"/>";
    }
    if (format.highlight_rgb.has_value()) {
        output << "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\""
               << rgbHex(*format.highlight_rgb) << "\"/>";
    }
    if (format.baseline.has_value()) {
        std::string_view value = "baseline";
        if (*format.baseline == BasicBaseline::superscript) {
            value = "superscript";
        } else if (*format.baseline == BasicBaseline::subscript) {
            value = "subscript";
        }
        output << "<w:vertAlign w:val=\"" << value << "\"/>";
    }
    output << "</w:rPr>";
}

std::string ommlTextForLatexToken(std::string_view token) {
    static constexpr std::pair<std::string_view, std::string_view> symbols[]{
        {"\\alpha", "α"},       {"\\beta", "β"},       {"\\gamma", "γ"},
        {"\\delta", "δ"},       {"\\epsilon", "ϵ"},    {"\\varepsilon", "ε"},
        {"\\zeta", "ζ"},        {"\\eta", "η"},        {"\\theta", "θ"},
        {"\\vartheta", "ϑ"},    {"\\iota", "ι"},       {"\\kappa", "κ"},
        {"\\lambda", "λ"},      {"\\mu", "μ"},         {"\\nu", "ν"},
        {"\\xi", "ξ"},          {"\\pi", "π"},         {"\\varpi", "ϖ"},
        {"\\rho", "ρ"},         {"\\sigma", "σ"},      {"\\tau", "τ"},
        {"\\upsilon", "υ"},     {"\\phi", "ϕ"},        {"\\varphi", "φ"},
        {"\\chi", "χ"},         {"\\psi", "ψ"},        {"\\omega", "ω"},
        {"\\Gamma", "Γ"},       {"\\Delta", "Δ"},      {"\\Theta", "Θ"},
        {"\\Lambda", "Λ"},      {"\\Xi", "Ξ"},         {"\\Pi", "Π"},
        {"\\Sigma", "Σ"},       {"\\Upsilon", "Υ"},    {"\\Phi", "Φ"},
        {"\\Psi", "Ψ"},         {"\\Omega", "Ω"},      {"\\infty", "∞"},
        {"\\times", "×"},       {"\\cdot", "⋅"},       {"\\pm", "±"},
        {"\\mp", "∓"},          {"\\div", "÷"},        {"\\le", "≤"},
        {"\\leq", "≤"},         {"\\ge", "≥"},         {"\\geq", "≥"},
        {"\\neq", "≠"},         {"\\approx", "≈"},     {"\\sim", "∼"},
        {"\\to", "→"},          {"\\in", "∈"},         {"\\notin", "∉"},
        {"\\subset", "⊂"},      {"\\subseteq", "⊆"},   {"\\cup", "∪"},
        {"\\cap", "∩"},         {"\\land", "∧"},       {"\\lor", "∨"},
        {"\\partial", "∂"},     {"\\nabla", "∇"},      {"\\{", "{"},
        {"\\}", "}"},                {"\\|", "‖"},
    };
    for (const auto& [command, glyph] : symbols) {
        if (token == command) return std::string(glyph);
    }
    if (token == "\\sin" || token == "\\cos" || token == "\\tan" ||
        token == "\\log" || token == "\\ln" || token == "\\exp" ||
        token == "\\lim") {
        return std::string(token.substr(1));
    }
    return std::string(token);
}

std::string ommlDelimiterGlyph(std::string_view token) {
    if (token == ".") return {};
    if (token == "\\langle") return "⟨";
    if (token == "\\rangle") return "⟩";
    if (token == "\\lbrace" || token == "\\{") return "{";
    if (token == "\\rbrace" || token == "\\}") return "}";
    if (token == "\\vert" || token == "\\|") return "|";
    if (token == "\\Vert") return "‖";
    return std::string(token);
}

void appendOmmlNode(
    std::ostringstream& output,
    const math::MathNodePtr& node,
    const BasicRunFormat& format);

void appendOmmlTextRun(
    std::ostringstream& output,
    std::string_view token,
    const BasicRunFormat& format,
    bool plain_style = false) {
    output << "<m:r>";
    if (plain_style) output << "<m:rPr><m:sty m:val=\"p\"/></m:rPr>";
    appendBasicRunProperties(output, format);
    output << "<m:t>" << escapeXmlText(ommlTextForLatexToken(token)) << "</m:t></m:r>";
}

void appendOmmlNary(
    std::ostringstream& output,
    const math::LargeOperator& operation,
    const std::optional<math::MathNodePtr>& subscript,
    const std::optional<math::MathNodePtr>& superscript,
    const BasicRunFormat& format) {
    output << "<m:nary><m:naryPr><m:chr m:val=\""
           << (operation.kind == math::LargeOperatorKind::sum ? "∑" : "∫")
           << "\"/><m:limLoc m:val=\"undOvr\"/></m:naryPr><m:sub>";
    if (subscript && *subscript) appendOmmlNode(output, *subscript, format);
    output << "</m:sub><m:sup>";
    if (superscript && *superscript) appendOmmlNode(output, *superscript, format);
    output << "</m:sup><m:e/></m:nary>";
}

void appendOmmlMatrixBody(
    std::ostringstream& output,
    const math::Matrix& matrix,
    const BasicRunFormat& format) {
    output << "<m:m>";
    for (const auto& row : matrix.rows) {
        output << "<m:mr>";
        for (const auto& cell : row) {
            output << "<m:e>";
            appendOmmlNode(output, cell, format);
            output << "</m:e>";
        }
        output << "</m:mr>";
    }
    output << "</m:m>";
}

void appendOmmlNode(
    std::ostringstream& output,
    const math::MathNodePtr& node,
    const BasicRunFormat& format) {
    if (!node) return;
    std::visit(
        [&](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, math::Row>) {
                for (const auto& child : value.children) appendOmmlNode(output, child, format);
            } else if constexpr (std::is_same_v<Value, math::Group>) {
                output << "<m:box><m:e>";
                appendOmmlNode(output, value.body, format);
                output << "</m:e></m:box>";
            } else if constexpr (std::is_same_v<Value, math::Identifier>) {
                const bool plain = value.text == "\\sin" || value.text == "\\cos" ||
                                   value.text == "\\tan" || value.text == "\\log" ||
                                   value.text == "\\ln" || value.text == "\\exp" ||
                                   value.text == "\\lim";
                appendOmmlTextRun(output, value.text, format, plain);
            } else if constexpr (std::is_same_v<Value, math::Number> ||
                                 std::is_same_v<Value, math::Operator>) {
                appendOmmlTextRun(output, value.text, format);
            } else if constexpr (std::is_same_v<Value, math::Fraction>) {
                output << "<m:f><m:fPr><m:type m:val=\"bar\"/></m:fPr><m:num>";
                appendOmmlNode(output, value.numerator, format);
                output << "</m:num><m:den>";
                appendOmmlNode(output, value.denominator, format);
                output << "</m:den></m:f>";
            } else if constexpr (std::is_same_v<Value, math::Radical>) {
                output << "<m:rad>";
                if (!value.index || !*value.index) {
                    output << "<m:radPr><m:degHide m:val=\"1\"/></m:radPr>";
                }
                output << "<m:deg>";
                if (value.index && *value.index) appendOmmlNode(output, *value.index, format);
                output << "</m:deg><m:e>";
                appendOmmlNode(output, value.radicand, format);
                output << "</m:e></m:rad>";
            } else if constexpr (std::is_same_v<Value, math::Script>) {
                if (value.base) {
                    if (const auto* operation =
                            std::get_if<math::LargeOperator>(&value.base->value)) {
                        appendOmmlNary(
                            output, *operation, value.subscript, value.superscript, format);
                        return;
                    }
                }
                const char* tag = value.subscript && value.superscript
                                      ? "sSubSup"
                                      : (value.subscript ? "sSub" : "sSup");
                output << "<m:" << tag << "><m:e>";
                appendOmmlNode(output, value.base, format);
                output << "</m:e>";
                if (value.subscript) {
                    output << "<m:sub>";
                    appendOmmlNode(output, *value.subscript, format);
                    output << "</m:sub>";
                }
                if (value.superscript) {
                    output << "<m:sup>";
                    appendOmmlNode(output, *value.superscript, format);
                    output << "</m:sup>";
                }
                output << "</m:" << tag << ">";
            } else if constexpr (std::is_same_v<Value, math::LargeOperator>) {
                appendOmmlNary(output, value, std::nullopt, std::nullopt, format);
            } else if constexpr (std::is_same_v<Value, math::Delimited>) {
                output << "<m:d><m:dPr><m:begChr m:val=\""
                       << escapeXmlAttribute(ommlDelimiterGlyph(value.left))
                       << "\"/><m:endChr m:val=\""
                       << escapeXmlAttribute(ommlDelimiterGlyph(value.right))
                       << "\"/></m:dPr><m:e>";
                appendOmmlNode(output, value.body, format);
                output << "</m:e></m:d>";
            } else if constexpr (std::is_same_v<Value, math::Matrix>) {
                std::string left;
                std::string right;
                switch (value.environment) {
                    case math::MatrixEnvironment::matrix: break;
                    case math::MatrixEnvironment::pmatrix: left = "("; right = ")"; break;
                    case math::MatrixEnvironment::bmatrix: left = "["; right = "]"; break;
                    case math::MatrixEnvironment::Bmatrix: left = "{"; right = "}"; break;
                    case math::MatrixEnvironment::vmatrix: left = "|"; right = "|"; break;
                    case math::MatrixEnvironment::Vmatrix: left = "‖"; right = "‖"; break;
                }
                if (!left.empty()) {
                    output << "<m:d><m:dPr><m:begChr m:val=\""
                           << escapeXmlAttribute(left) << "\"/><m:endChr m:val=\""
                           << escapeXmlAttribute(right) << "\"/></m:dPr><m:e>";
                }
                appendOmmlMatrixBody(output, value, format);
                if (!left.empty()) output << "</m:e></m:d>";
            }
        },
        node->value);
}

void appendOmmlEquation(
    std::ostringstream& output,
    const math::MathAst& ast,
    const EquationPayload& equation,
    const BasicRunFormat& format) {
    if (equation.display) {
        output << "<m:oMathPara><m:oMathParaPr><m:jc m:val=\"center\"/>"
                  "</m:oMathParaPr><m:oMath>";
    } else {
        output << "<m:oMath>";
    }
    appendOmmlNode(output, ast.root, format);
    output << "</m:oMath>";
    if (equation.display) output << "</m:oMathPara>";
}

bool validateBasicRunFormat(
    const BasicRunFormat& format,
    std::size_t paragraph_index,
    LossReport& loss,
    Error* error) {
    if (format.font_size_half_points.has_value() &&
        (*format.font_size_half_points < 2 ||
         *format.font_size_half_points > 3276)) {
        const std::string message =
            "Font size must be between 1 and 1638 points";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message,
            paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    if (format.foreground_rgb.has_value() &&
        *format.foreground_rgb > 0x00ffffffU) {
        const std::string message =
            "Foreground color must be an 0xRRGGBB value";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message,
            paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    if (format.highlight_rgb.has_value() &&
        *format.highlight_rgb > 0x00ffffffU) {
        const std::string message =
            "Highlight color must be an 0xRRGGBB value";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message,
            paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    if (format.font_family.has_value()) {
        IssueCode issue_code = IssueCode::invalid_utf8;
        std::string detail;
        if (!isValidUtf8XmlText(*format.font_family, issue_code, detail) ||
            format.font_family->empty()) {
            const std::string message =
                detail.empty() ? "Font family cannot be empty" : detail;
            loss.issues.push_back(blockingIssue(
                detail.empty() ? IssueCode::structural_rewrite_required
                               : issue_code,
                message, paragraph_index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }
    }
    return true;
}

void appendInlineImageDrawing(
    std::ostringstream& output, const NewRun& run,
    const AuthoredImagePart& part, std::size_t image_number) {
    const auto& image = *part.image;
    const std::string name = escapeXmlAttribute(image.name);
    const std::string accessible_name = escapeXmlAttribute(
        image.accessible_name.empty() ? image.name
                                      : image.accessible_name);
    output << "<w:r>";
    appendBasicRunProperties(output, run.format);
    output << "<w:drawing>";
    if (image.layout.placement == ImagePlacement::inline_with_text) {
        output << "<wp:inline distT=\"" << image.layout.distance_top_emu
               << "\" distB=\"" << image.layout.distance_bottom_emu
               << "\" distL=\"" << image.layout.distance_left_emu
               << "\" distR=\"" << image.layout.distance_right_emu
               << "\">";
    } else {
        const std::string_view horizontal_relative =
            image.layout.move_with_text ? "character" : "page";
        const std::string_view vertical_relative =
            image.layout.move_with_text ? "paragraph" : "page";
        output << "<wp:anchor distT=\"" << image.layout.distance_top_emu
               << "\" distB=\"" << image.layout.distance_bottom_emu
               << "\" distL=\"" << image.layout.distance_left_emu
               << "\" distR=\"" << image.layout.distance_right_emu
               << "\" simplePos=\"0\" relativeHeight=\"0\" behindDoc=\"0\""
                  " locked=\"0\" layoutInCell=\"1\" allowOverlap=\"0\">"
                  "<wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\""
               << horizontal_relative
               << "\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                  "<wp:positionV relativeFrom=\""
               << vertical_relative
               << "\"><wp:posOffset>0</wp:posOffset></wp:positionV>";
    }
    output << "<wp:extent cx=\"" << image.width_emu << "\" cy=\""
           << image.height_emu
           << "\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>";
    if (image.layout.placement == ImagePlacement::square) {
        output << "<wp:wrapSquare wrapText=\"bothSides\"/>";
    } else if (image.layout.placement ==
               ImagePlacement::top_and_bottom) {
        output << "<wp:wrapTopAndBottom/>";
    }
    output << "<wp:docPr id=\""
        << image_number << "\" name=\"" << name
        << "\" descr=\"" << accessible_name
        << "\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/>"
           "</wp:cNvGraphicFramePr><a:graphic><a:graphicData uri=\""
        << kDrawingPictureNamespace
        << "\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\""
        << name
        << "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\""
        << part.relationship_id
        << "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
           "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\""
        << image.width_emu << "\" cy=\"" << image.height_emu
        << "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
           "</a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
        << (image.layout.placement == ImagePlacement::inline_with_text
                ? "</wp:inline>"
                : "</wp:anchor>")
        << "</w:drawing></w:r>";
}

bool buildNewDocumentXml(
    const std::vector<NewParagraph>& paragraphs,
    const PageSettings& page,
    std::string& xml,
    LossReport& loss,
    Error* error,
    const std::vector<AuthoredImagePart>& images,
    std::size_t& image_index) {
    std::ostringstream document;
    document << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             << "<w:document xmlns:w=\"" << kWordNamespace
             << "\" xmlns:m=\"" << kOfficeMathNamespace
             << "\" xmlns:r=\"" << kOfficeRelationshipsNamespace
             << "\" xmlns:wp=\"" << kWordprocessingDrawingNamespace
             << "\" xmlns:a=\"" << kDrawingMainNamespace
             << "\" xmlns:pic=\"" << kDrawingPictureNamespace
             << "\"><w:body>";

    for (std::size_t paragraph_index = 0; paragraph_index < paragraphs.size(); ++paragraph_index) {
        const NewParagraph& paragraph = paragraphs[paragraph_index];
        document << "<w:p>";
        const bool has_paragraph_format =
            paragraph.style_id.has_value() ||
            paragraph.alignment.has_value() || paragraph.left_indent_twips.has_value() ||
            paragraph.right_indent_twips.has_value() ||
            paragraph.first_line_indent_twips.has_value() ||
            paragraph.space_before_twips.has_value() ||
            paragraph.space_after_twips.has_value() || paragraph.line_spacing.has_value() ||
            paragraph.line_spacing_rule.has_value() || paragraph.keep_with_next.has_value() ||
            paragraph.keep_lines.has_value() || paragraph.page_break_before.has_value() ||
            !paragraph.left_tab_stops_twips.empty() ||
            paragraph.numbering.has_value() ||
            paragraph.paragraph_mark_format.has_value();
        if (paragraph.line_spacing_rule.has_value() && !paragraph.line_spacing.has_value()) {
            const std::string message =
                "A paragraph line-spacing rule requires a line-spacing value";
            loss.issues.push_back(blockingIssue(
                IssueCode::structural_rewrite_required, message, paragraph_index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }
        if (paragraph.line_spacing.has_value() && *paragraph.line_spacing == 0) {
            const std::string message = "Paragraph line spacing must be positive";
            loss.issues.push_back(blockingIssue(
                IssueCode::structural_rewrite_required, message, paragraph_index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }
        if (paragraph.paragraph_mark_format.has_value() &&
            !validateBasicRunFormat(
                *paragraph.paragraph_mark_format, paragraph_index, loss,
                error)) {
            return false;
        }
        if (paragraph.numbering) {
            const auto& numbering = *paragraph.numbering;
            IssueCode issue_code = IssueCode::invalid_utf8;
            std::string detail;
            const bool valid_template =
                !numbering.level_text.empty() &&
                isValidUtf8XmlText(
                    numbering.level_text, issue_code, detail);
            const bool valid_numbering =
                numbering.num_id > 0 &&
                numbering.level <= kMaximumNativeNumberingLevel &&
                numbering.start >= 0 && valid_template &&
                numbering.text_indent_twips > 0 &&
                numbering.text_indent_twips <= 31680U &&
                numbering.hanging_indent_twips > 0 &&
                numbering.hanging_indent_twips <=
                    numbering.text_indent_twips &&
                (!numbering.tab_stop_twips ||
                 (*numbering.tab_stop_twips > 0 &&
                  *numbering.tab_stop_twips <= 31680U));
            if (!valid_numbering) {
                const std::string message = detail.empty()
                    ? "Native list numbering has an invalid ID, level, template, or indentation"
                    : detail;
                loss.issues.push_back(blockingIssue(
                    detail.empty()
                        ? IssueCode::structural_rewrite_required
                        : issue_code,
                    message, paragraph_index));
                setError(error, ErrorCode::unsafe_edit, message);
                return false;
            }
        }
        if (has_paragraph_format) document << "<w:pPr>";
        const auto write_on_off = [&](std::string_view name,
                                      const std::optional<bool>& value) {
            if (!value.has_value()) return;
            document << "<w:" << name;
            if (!*value) document << " w:val=\"0\"";
            document << "/>";
        };
        // CT_PPr children are emitted in schema order. Word is lenient about
        // this, but other validators and editors are not required to be.
        if (paragraph.style_id) {
            document << "<w:pStyle w:val=\""
                     << escapeXmlAttribute(*paragraph.style_id) << "\"/>";
        }
        write_on_off("keepNext", paragraph.keep_with_next);
        write_on_off("keepLines", paragraph.keep_lines);
        write_on_off("pageBreakBefore", paragraph.page_break_before);
        if (paragraph.numbering) {
            document << "<w:numPr><w:ilvl w:val=\""
                     << static_cast<unsigned int>(paragraph.numbering->level)
                     << "\"/><w:numId w:val=\""
                     << paragraph.numbering->num_id
                     << "\"/></w:numPr>";
        }
        if (!paragraph.left_tab_stops_twips.empty()) {
            std::uint32_t previous = 0;
            document << "<w:tabs>";
            for (const auto position : paragraph.left_tab_stops_twips) {
                if (position == 0 || position > 31680 || position <= previous) {
                    const std::string message =
                        "Paragraph left tab stops must be unique, increasing, and between 1 and 31680 twips";
                    loss.issues.push_back(blockingIssue(
                        IssueCode::structural_rewrite_required, message,
                        paragraph_index));
                    setError(error, ErrorCode::unsafe_edit, message);
                    return false;
                }
                document << "<w:tab w:val=\"left\" w:pos=\"" << position
                         << "\"/>";
                previous = position;
            }
            document << "</w:tabs>";
        }
        if (paragraph.space_before_twips.has_value() ||
            paragraph.space_after_twips.has_value() || paragraph.line_spacing.has_value()) {
            document << "<w:spacing";
            if (paragraph.space_before_twips) {
                document << " w:before=\"" << *paragraph.space_before_twips << "\"";
            }
            if (paragraph.space_after_twips) {
                document << " w:after=\"" << *paragraph.space_after_twips << "\"";
            }
            if (paragraph.line_spacing) {
                document << " w:line=\"" << *paragraph.line_spacing << "\"";
                if (paragraph.line_spacing_rule) {
                    std::string_view rule = "auto";
                    switch (*paragraph.line_spacing_rule) {
                        case BasicLineSpacingRule::automatic: rule = "auto"; break;
                        case BasicLineSpacingRule::at_least: rule = "atLeast"; break;
                        case BasicLineSpacingRule::exact: rule = "exact"; break;
                    }
                    document << " w:lineRule=\"" << rule << "\"";
                }
            }
            document << "/>";
        }
        if (paragraph.left_indent_twips.has_value() ||
            paragraph.right_indent_twips.has_value() ||
            paragraph.first_line_indent_twips.has_value()) {
            document << "<w:ind";
            if (paragraph.left_indent_twips) {
                document << " w:left=\"" << *paragraph.left_indent_twips << "\"";
            }
            if (paragraph.right_indent_twips) {
                document << " w:right=\"" << *paragraph.right_indent_twips << "\"";
            }
            if (paragraph.first_line_indent_twips) {
                if (*paragraph.first_line_indent_twips < 0) {
                    const auto hanging = -static_cast<std::int64_t>(
                        *paragraph.first_line_indent_twips);
                    document << " w:hanging=\"" << hanging << "\"";
                } else {
                    document << " w:firstLine=\"" << *paragraph.first_line_indent_twips << "\"";
                }
            }
            document << "/>";
        }
        if (paragraph.alignment.has_value()) {
            std::string_view value = "left";
            switch (*paragraph.alignment) {
                case BasicParagraphAlignment::left:
                    value = "left";
                    break;
                case BasicParagraphAlignment::center:
                    value = "center";
                    break;
                case BasicParagraphAlignment::right:
                    value = "right";
                    break;
                case BasicParagraphAlignment::justified:
                    value = "both";
                    break;
            }
            document << "<w:jc w:val=\"" << value << "\"/>";
        }
        if (paragraph.paragraph_mark_format.has_value()) {
            // CT_PPr requires its paragraph-mark run properties last.
            appendBasicRunProperties(
                document, *paragraph.paragraph_mark_format);
        }
        if (has_paragraph_format) {
            document << "</w:pPr>";
        }

        for (const NewRun& run : paragraph.runs) {
            IssueCode issue_code = IssueCode::invalid_utf8;
            std::string detail;
            if (!isValidUtf8XmlText(run.text, issue_code, detail)) {
                loss.issues.push_back(blockingIssue(issue_code, detail, paragraph_index));
                setError(error, ErrorCode::unsafe_edit, detail);
                return false;
            }
            if (!validateBasicRunFormat(
                    run.format, paragraph_index, loss, error)) {
                return false;
            }

            if (run.equation.has_value() && run.inline_image.has_value()) {
                const std::string message =
                    "A run cannot contain both an equation and an inline image";
                loss.issues.push_back(blockingIssue(
                    IssueCode::structural_rewrite_required, message,
                    paragraph_index));
                setError(error, ErrorCode::unsafe_edit, message);
                return false;
            }
            if (run.equation.has_value()) {
                if (!run.text.empty()) {
                    const std::string message =
                        "An equation run cannot also contain ordinary text";
                    loss.issues.push_back(blockingIssue(
                        IssueCode::structural_rewrite_required, message, paragraph_index));
                    setError(error, ErrorCode::unsafe_edit, message);
                    return false;
                }
                const auto parsed = math::parseLatex(run.equation->canonical_latex);
                if (!parsed) {
                    const std::string message =
                        "Equation is outside the safe LaTeX subset: " +
                        parsed.error().message;
                    loss.issues.push_back(blockingIssue(
                        IssueCode::structural_rewrite_required, message, paragraph_index));
                    setError(error, ErrorCode::unsafe_edit, message);
                    return false;
                }
                appendOmmlEquation(document, parsed.value(), *run.equation, run.format);
                continue;
            }

            if (run.inline_image.has_value()) {
                if (!run.text.empty()) {
                    const std::string message =
                        "An inline image run cannot also contain ordinary text";
                    loss.issues.push_back(blockingIssue(
                        IssueCode::structural_rewrite_required, message,
                        paragraph_index));
                    setError(error, ErrorCode::unsafe_edit, message);
                    return false;
                }
                if (image_index >= images.size()) {
                    setError(error, ErrorCode::save_validation_failed,
                             "Inline image catalog and document traversal differ");
                    return false;
                }
                appendInlineImageDrawing(
                    document, run, images[image_index], image_index + 1U);
                ++image_index;
                continue;
            }

            document << "<w:r>";
            appendBasicRunProperties(document, run.format);
            appendWordRunContents(document, run.text);
            document << "</w:r>";
        }
        document << "</w:p>";
    }
    document << "<w:sectPr><w:pgSz w:w=\"" << page.width_twips
             << "\" w:h=\"" << page.height_twips << "\"";
    if (page.width_twips > page.height_twips) {
        document << " w:orient=\"landscape\"";
    }
    document << "/><w:pgMar w:top=\"" << page.margin_top_twips
             << "\" w:right=\"" << page.margin_right_twips
             << "\" w:bottom=\"" << page.margin_bottom_twips
             << "\" w:left=\"" << page.margin_left_twips << "\" "
                "w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
             << "</w:sectPr></w:body></w:document>";
    xml = document.str();
    return true;
}

bool appendNewParagraphFragment(
    std::ostringstream& output,
    const NewParagraph& paragraph,
    const PageSettings& page,
    LossReport& loss,
    Error* error,
    const std::vector<AuthoredImagePart>& images,
    std::size_t& image_index) {
    std::string wrapped;
    if (!buildNewDocumentXml(
            {paragraph}, page, wrapped, loss, error, images,
            image_index)) {
        return false;
    }
    constexpr std::string_view body_marker = "<w:body>";
    constexpr std::string_view section_marker = "<w:sectPr>";
    const auto body = wrapped.find(body_marker);
    const auto section = wrapped.find(section_marker, body);
    if (body == std::string::npos || section == std::string::npos ||
        section < body + body_marker.size()) {
        setError(error, ErrorCode::save_validation_failed,
                 "Could not compose a body paragraph");
        return false;
    }
    output << std::string_view(wrapped).substr(
        body + body_marker.size(), section - body - body_marker.size());
    return true;
}

bool buildNewDocumentXml(
    const NewDocumentBody& body,
    const PageSettings& page,
    std::string& xml,
    LossReport& loss,
    Error* error,
    const std::vector<AuthoredImagePart>& images,
    std::size_t& image_index) {
    std::ostringstream document;
    document << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             << "<w:document xmlns:w=\"" << kWordNamespace
             << "\" xmlns:m=\"" << kOfficeMathNamespace
             << "\" xmlns:r=\"" << kOfficeRelationshipsNamespace
             << "\" xmlns:wp=\"" << kWordprocessingDrawingNamespace
             << "\" xmlns:a=\"" << kDrawingMainNamespace
             << "\" xmlns:pic=\"" << kDrawingPictureNamespace
             << "\"><w:body>";

    for (std::size_t block_index = 0; block_index < body.blocks.size();
         ++block_index) {
        const auto& block = body.blocks[block_index];
        if (const auto* paragraph = std::get_if<NewParagraph>(&block)) {
            if (!appendNewParagraphFragment(
                    document, *paragraph, page, loss, error, images,
                    image_index)) {
                return false;
            }
            continue;
        }

        const auto& table = std::get<NewTable>(block);
        const std::size_t required_cells =
            table.rows > 0 && table.columns > 0 &&
                    table.rows <= std::numeric_limits<std::size_t>::max() /
                                      table.columns
                ? table.rows * table.columns
                : 0;
        const bool has_formatted_cells = !table.cell_paragraphs.empty();
        const bool has_valid_cell_payload =
            has_formatted_cells
                ? table.cell_paragraphs.size() == required_cells &&
                      (table.cells.empty() || table.cells.size() == required_cells)
                : table.cells.size() == required_cells;
        if (table.rows == 0 || table.columns == 0 ||
            table.rows > NewTable::maximum_rows ||
            table.columns > NewTable::maximum_columns ||
            table.rows > std::numeric_limits<std::size_t>::max() / table.columns ||
            !has_valid_cell_payload) {
            const std::string message =
                "Table must be a rectangular grid between 1x1 and " +
                std::to_string(NewTable::maximum_rows) + "x" +
                std::to_string(NewTable::maximum_columns);
            loss.issues.push_back(blockingIssue(
                IssueCode::structural_rewrite_required, message, block_index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }

        const std::uint64_t available_width =
            static_cast<std::uint64_t>(page.width_twips) -
            page.margin_left_twips - page.margin_right_twips;
        const auto column_width = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(1, available_width / table.columns));
        const auto& style = tableStyleDescriptor(
            table.style.value_or(BasicTableStyle::grid));

        document << "<w:tbl><w:tblPr>";
        const auto imported_style = table.source_style_id
            ? basicTableStyleFromWordId(*table.source_style_id)
            : std::nullopt;
        const bool source_style_matches =
            imported_style.has_value() &&
            (!table.style.has_value() || table.style == imported_style);
        if (source_style_matches) {
            document << "<w:tblStyle w:val=\""
                     << escapeXmlAttribute(*table.source_style_id) << "\"/>";
        } else if (table.style) {
            document << "<w:tblStyle w:val=\"" << style.word_style_id << "\"/>";
        }
        if (table.source_style_id && !source_style_matches) {
            IssueCode issue_code = IssueCode::invalid_utf8;
            std::string detail;
            const bool valid_id = !table.source_style_id->empty() &&
                isValidUtf8XmlText(
                    *table.source_style_id, issue_code, detail);
            std::string message;
            if (!valid_id) {
                message =
                    "An invalid imported table style ID was omitted from the generated package";
            } else if (!imported_style) {
                message = "Imported table style '" + *table.source_style_id +
                    "' was omitted because its definition is not available in the generated package";
            } else {
                message = "Imported table style '" + *table.source_style_id +
                    "' was replaced by the table's edited semantic style";
            }
            loss.issues.push_back(CompatibilityIssue{
                IssueSeverity::warning,
                IssueCode::unsupported_formatting,
                std::string(kDocumentPart),
                std::move(message),
                block_index});
        }
        document
            <<
               "<w:tblW w:w=\"0\" w:type=\"auto\"/>"
               "<w:tblLayout w:type=\"fixed\"/>"
               "<w:tblBorders>"
               "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/><w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\""
            << style.border_rgb
            << "\"/></w:tblBorders>"
               "<w:tblCellMar>"
               "<w:top w:w=\"72\" w:type=\"dxa\"/>"
               "<w:left w:w=\"108\" w:type=\"dxa\"/>"
               "<w:bottom w:w=\"72\" w:type=\"dxa\"/>"
               "<w:right w:w=\"108\" w:type=\"dxa\"/>"
               "</w:tblCellMar>"
               "<w:tblLook w:val=\"04A0\" w:firstRow=\""
            << (table.header_row ? "1" : "0")
            << "\" w:lastRow=\"0\" w:firstColumn=\"0\" w:lastColumn=\"0\" "
               "w:noHBand=\"0\" w:noVBand=\"1\"/>"
               "</w:tblPr><w:tblGrid>";
        for (std::size_t column = 0; column < table.columns; ++column) {
            document << "<w:gridCol w:w=\"" << column_width << "\"/>";
        }
        document << "</w:tblGrid>";

        for (std::size_t row = 0; row < table.rows; ++row) {
            document << "<w:tr>";
            if (table.header_row && row == 0) {
                document << "<w:trPr><w:tblHeader/></w:trPr>";
            }
            for (std::size_t column = 0; column < table.columns; ++column) {
                const std::size_t cell_index = row * table.columns + column;
                document << "<w:tc><w:tcPr><w:tcW w:w=\"" << column_width
                         << "\" w:type=\"dxa\"/>";
                if (table.header_row && row == 0) {
                    document << "<w:shd w:val=\"clear\" w:color=\"auto\" "
                                "w:fill=\""
                             << style.header_fill_rgb << "\"/>";
                } else {
                    const std::size_t body_row =
                        row - (table.header_row ? 1U : 0U);
                    if ((body_row % 2U) == 1U) {
                        document << "<w:shd w:val=\"clear\" w:color=\"auto\" "
                                    "w:fill=\""
                                 << style.band_fill_rgb << "\"/>";
                    }
                }
                document << "</w:tcPr>";
                NewParagraph cell_paragraph;
                if (has_formatted_cells) {
                    cell_paragraph = table.cell_paragraphs[cell_index];
                } else {
                    BasicRunFormat format;
                    if (table.header_row && row == 0) format.bold = true;
                    if (table.cells[cell_index].empty()) {
                        // An empty cell's insertion properties belong to its
                        // paragraph mark; a content-less run is not equivalent.
                        if (format.bold.has_value()) {
                            cell_paragraph.paragraph_mark_format =
                                std::move(format);
                        }
                    } else {
                        cell_paragraph = NewParagraph{{NewRun{
                            table.cells[cell_index], std::move(format)}}};
                    }
                }
                if (std::any_of(
                        cell_paragraph.runs.begin(), cell_paragraph.runs.end(),
                        [](const NewRun& run) { return run.equation.has_value(); })) {
                    const std::string message =
                        "Equations inside table cells are not in the editable table subset";
                    loss.issues.push_back(blockingIssue(
                        IssueCode::structural_rewrite_required, message,
                        block_index));
                    setError(error, ErrorCode::unsafe_edit, message);
                    return false;
                }
                if (table.header_row && row == 0 &&
                    style.header_text_rgb != "000000") {
                    const auto header_text = static_cast<std::uint32_t>(
                        std::stoul(std::string(style.header_text_rgb), nullptr, 16));
                    if (cell_paragraph.paragraph_mark_format &&
                        !cell_paragraph.paragraph_mark_format
                             ->foreground_rgb) {
                        cell_paragraph.paragraph_mark_format
                            ->foreground_rgb = header_text;
                    }
                    for (auto& run : cell_paragraph.runs) {
                        if (!run.format.foreground_rgb) {
                            run.format.foreground_rgb = header_text;
                        }
                    }
                }
                if (!appendNewParagraphFragment(
                        document, cell_paragraph, page, loss, error, images,
                        image_index)) {
                    return false;
                }
                document << "</w:tc>";
            }
            document << "</w:tr>";
        }
        document << "</w:tbl>";
    }

    document << "<w:sectPr>";
    if (body.header_text && !body.header_text->empty()) {
        document << "<w:headerReference w:type=\"default\" r:id=\"rIdHeader1\"/>";
    }
    if (body.footer_text && !body.footer_text->empty()) {
        document << "<w:footerReference w:type=\"default\" r:id=\"rIdFooter1\"/>";
    }
    document << "<w:pgSz w:w=\"" << page.width_twips
             << "\" w:h=\"" << page.height_twips << "\"";
    if (page.width_twips > page.height_twips) {
        document << " w:orient=\"landscape\"";
    }
    document << "/><w:pgMar w:top=\"" << page.margin_top_twips
             << "\" w:right=\"" << page.margin_right_twips
             << "\" w:bottom=\"" << page.margin_bottom_twips
             << "\" w:left=\"" << page.margin_left_twips << "\" "
                "w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
             << "</w:sectPr></w:body></w:document>";
    xml = document.str();
    return true;
}

void appendStoryRunContents(std::ostringstream& output,
                            std::string_view text) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto page = text.find("{PAGE}", cursor);
        const auto pages = text.find("{PAGES}", cursor);
        std::size_t token = std::min(page, pages);
        if (page == std::string_view::npos) token = pages;
        if (pages == std::string_view::npos) token = page;
        if (token == std::string_view::npos) {
            if (cursor < text.size()) {
                output << "<w:r>";
                appendWordRunContents(output, text.substr(cursor));
                output << "</w:r>";
            }
            break;
        }
        if (token > cursor) {
            output << "<w:r>";
            appendWordRunContents(output, text.substr(cursor, token - cursor));
            output << "</w:r>";
        }
        const bool total = token == pages;
        output << "<w:fldSimple w:instr=\" "
               << (total ? "NUMPAGES" : "PAGE")
               << " \"><w:r><w:t>1</w:t></w:r></w:fldSimple>";
        cursor = token + (total ? 7U : 6U);
    }
}

std::string buildStoryXml(std::string_view text, bool header,
                          const PageSettings& page) {
    const std::uint32_t content_width = page.width_twips >
            page.margin_left_twips + page.margin_right_twips
        ? page.width_twips - page.margin_left_twips - page.margin_right_twips
        : 1U;
    std::ostringstream output;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           << (header ? "<w:hdr" : "<w:ftr")
           << " xmlns:w=\"" << kWordNamespace << "\"><w:p>"
           << "<w:pPr><w:tabs><w:tab w:val=\"center\" w:pos=\""
           << content_width / 2U
           << "\"/><w:tab w:val=\"right\" w:pos=\""
           << content_width
           << "\"/></w:tabs></w:pPr>";
    appendStoryRunContents(output, text);
    output << "</w:p>" << (header ? "</w:hdr>" : "</w:ftr>");
    return output.str();
}

bool validPageSettings(const PageSettings& page) {
    return page.width_twips >= 1440 && page.height_twips >= 1440 &&
           page.width_twips <= 63360 && page.height_twips <= 63360 &&
           static_cast<std::uint64_t>(page.margin_top_twips) +
                   page.margin_bottom_twips <
               page.height_twips &&
           static_cast<std::uint64_t>(page.margin_left_twips) +
                   page.margin_right_twips <
               page.width_twips;
}

std::string_view sectionBreakName(SectionBreakKind kind) {
    switch (kind) {
        case SectionBreakKind::next_page: return "nextPage";
        case SectionBreakKind::continuous: return "continuous";
        case SectionBreakKind::even_page: return "evenPage";
        case SectionBreakKind::odd_page: return "oddPage";
    }
    return {};
}

std::string sectionPropertiesXml(const NewSection& section) {
    std::ostringstream xml;
    xml << "<w:sectPr><w:type w:val=\""
        << sectionBreakName(section.break_kind)
        << "\"/><w:pgSz w:w=\"" << section.page.width_twips
        << "\" w:h=\"" << section.page.height_twips << "\"";
    if (section.page.width_twips > section.page.height_twips) {
        xml << " w:orient=\"landscape\"";
    }
    xml << "/><w:pgMar w:top=\"" << section.page.margin_top_twips
        << "\" w:right=\"" << section.page.margin_right_twips
        << "\" w:bottom=\"" << section.page.margin_bottom_twips
        << "\" w:left=\"" << section.page.margin_left_twips
        << "\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
           "</w:sectPr>";
    return xml.str();
}

bool extractGeneratedBody(std::string_view wrapped, std::string& body,
                          Error* error) {
    constexpr std::string_view body_marker = "<w:body>";
    constexpr std::string_view section_marker = "<w:sectPr>";
    const auto body_start = wrapped.find(body_marker);
    const auto section_start = wrapped.rfind(section_marker);
    if (body_start == std::string_view::npos ||
        section_start == std::string_view::npos ||
        section_start < body_start + body_marker.size()) {
        setError(error, ErrorCode::save_validation_failed,
                 "Could not compose a section body");
        return false;
    }
    body.assign(wrapped.substr(
        body_start + body_marker.size(),
        section_start - body_start - body_marker.size()));
    return true;
}

bool attachSectionToTerminalParagraph(
    std::string& body, std::string_view section_xml, Error* error) {
    constexpr std::string_view paragraph_start = "<w:p>";
    constexpr std::string_view paragraph_end = "</w:p>";
    constexpr std::string_view properties_start = "<w:pPr>";
    constexpr std::string_view properties_end = "</w:pPr>";
    const auto start = body.rfind(paragraph_start);
    const auto end = body.rfind(paragraph_end);
    if (start == std::string::npos || end == std::string::npos || start > end) {
        setError(error, ErrorCode::save_validation_failed,
                 "A non-final section has no terminating paragraph");
        return false;
    }
    const auto properties = body.find(properties_start, start);
    if (properties != std::string::npos && properties < end) {
        const auto close = body.find(properties_end, properties);
        if (close == std::string::npos || close > end) {
            setError(error, ErrorCode::save_validation_failed,
                     "A terminating paragraph has malformed properties");
            return false;
        }
        body.insert(close, section_xml);
    } else {
        body.insert(
            start + paragraph_start.size(),
            std::string(properties_start) + std::string(section_xml) +
                std::string(properties_end));
    }
    return true;
}

bool buildNewSectionedDocumentXml(
    const NewSectionedDocumentBody& body, std::string& xml,
    LossReport& loss, Error* error,
    const std::vector<AuthoredImagePart>& images) {
    if (body.sections.size() < 2U) {
        const std::string message =
            "Multi-section authoring requires at least two sections";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }

    std::ostringstream document;
    document << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             << "<w:document xmlns:w=\"" << kWordNamespace
             << "\" xmlns:m=\"" << kOfficeMathNamespace
             << "\" xmlns:r=\"" << kOfficeRelationshipsNamespace
             << "\" xmlns:wp=\"" << kWordprocessingDrawingNamespace
             << "\" xmlns:a=\"" << kDrawingMainNamespace
             << "\" xmlns:pic=\"" << kDrawingPictureNamespace
             << "\"><w:body>";

    std::size_t image_index = 0;
    for (std::size_t index = 0; index < body.sections.size(); ++index) {
        const auto& section = body.sections[index];
        const auto break_name = sectionBreakName(section.break_kind);
        if (!validPageSettings(section.page) || break_name.empty() ||
            section.body.blocks.empty()) {
            const std::string message =
                "Each section requires a valid page, break kind, and body";
            loss.issues.push_back(blockingIssue(
                IssueCode::structural_rewrite_required, message, index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }
        const bool final_section = index + 1U == body.sections.size();
        if (!final_section &&
            !std::holds_alternative<NewParagraph>(
                section.body.blocks.back())) {
            const std::string message =
                "A non-final section must end in a paragraph";
            loss.issues.push_back(blockingIssue(
                IssueCode::structural_rewrite_required, message, index));
            setError(error, ErrorCode::unsafe_edit, message);
            return false;
        }

        std::string wrapped;
        if (!buildNewDocumentXml(
                section.body, section.page, wrapped, loss, error, images,
                image_index)) {
            return false;
        }
        std::string section_body;
        if (!extractGeneratedBody(wrapped, section_body, error)) return false;
        if (!final_section &&
            !attachSectionToTerminalParagraph(
                section_body, sectionPropertiesXml(section), error)) {
            return false;
        }
        document << section_body;
    }
    document << sectionPropertiesXml(body.sections.back())
             << "</w:body></w:document>";
    if (image_index != images.size()) {
        setError(error, ErrorCode::save_validation_failed,
                 "Inline image catalog and section traversal differ");
        return false;
    }
    xml = document.str();
    return true;
}

std::string_view numberFormatName(BasicNumberFormat format) {
    switch (format) {
        case BasicNumberFormat::bullet: return "bullet";
        case BasicNumberFormat::decimal: return "decimal";
        case BasicNumberFormat::upper_letter: return "upperLetter";
        case BasicNumberFormat::lower_letter: return "lowerLetter";
        case BasicNumberFormat::upper_roman: return "upperRoman";
        case BasicNumberFormat::lower_roman: return "lowerRoman";
    }
    return "decimal";
}

std::string_view numberSuffixName(BasicNumberSuffix suffix) {
    switch (suffix) {
        case BasicNumberSuffix::tab: return "tab";
        case BasicNumberSuffix::space: return "space";
        case BasicNumberSuffix::nothing: return "nothing";
    }
    return "tab";
}

bool addNewParagraphStyleToCatalog(
    const NewParagraph& paragraph, NewParagraphStyleCatalog& catalog,
    LossReport& loss, Error* error,
    std::optional<std::size_t> paragraph_index = std::nullopt) {
    if (!paragraph.style_id) return true;

    IssueCode issue_code = IssueCode::invalid_utf8;
    std::string detail;
    if (!isValidUtf8XmlText(*paragraph.style_id, issue_code, detail)) {
        const std::string message =
            "Paragraph style ID is not valid UTF-8/XML text";
        loss.issues.push_back(blockingIssue(
            issue_code, message, paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    if (!safeParagraphStyleIdToken(*paragraph.style_id)) {
        const std::string message =
            "Paragraph style ID must be a 1-253 byte OOXML-safe token "
            "beginning with an ASCII letter or underscore";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message,
            paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    if (!supportedBuiltInParagraphStyle(*paragraph.style_id)) {
        const std::string message =
            "Paragraph style '" + *paragraph.style_id +
            "' has no supported deterministic definition";
        loss.issues.push_back(blockingIssue(
            IssueCode::unsupported_formatting, message, paragraph_index));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    catalog.insert(*paragraph.style_id);
    return true;
}

bool collectNewParagraphStyles(
    const NewDocumentBody& body, NewParagraphStyleCatalog& catalog,
    LossReport& loss, Error* error) {
    for (std::size_t block_index = 0; block_index < body.blocks.size();
         ++block_index) {
        const auto& block = body.blocks[block_index];
        if (const auto* paragraph = std::get_if<NewParagraph>(&block)) {
            if (!addNewParagraphStyleToCatalog(
                    *paragraph, catalog, loss, error, block_index)) {
                return false;
            }
            continue;
        }
        const auto& table = std::get<NewTable>(block);
        for (const auto& paragraph : table.cell_paragraphs) {
            if (!addNewParagraphStyleToCatalog(
                    paragraph, catalog, loss, error, block_index)) {
                return false;
            }
        }
    }
    return true;
}

using NewNumberingCatalog =
    std::map<std::int32_t, std::map<std::uint8_t, NewNumbering>>;

bool addNewNumberingToCatalog(
    const NewParagraph& paragraph, NewNumberingCatalog& catalog,
    LossReport& loss, Error* error) {
    if (!paragraph.numbering) return true;
    const auto& numbering = *paragraph.numbering;
    auto& levels = catalog[numbering.num_id];
    const auto [found, inserted] = levels.emplace(numbering.level, numbering);
    if (!inserted && found->second != numbering) {
        const std::string message =
            "Paragraphs in one native list use conflicting definitions for the same level";
        loss.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message));
        setError(error, ErrorCode::unsafe_edit, message);
        return false;
    }
    return true;
}

bool collectNewNumbering(
    const NewDocumentBody& body, NewNumberingCatalog& catalog,
    LossReport& loss, Error* error) {
    for (const auto& block : body.blocks) {
        if (const auto* paragraph = std::get_if<NewParagraph>(&block)) {
            if (!addNewNumberingToCatalog(
                    *paragraph, catalog, loss, error)) {
                return false;
            }
            continue;
        }
        const auto& table = std::get<NewTable>(block);
        for (const auto& paragraph : table.cell_paragraphs) {
            if (!addNewNumberingToCatalog(
                    paragraph, catalog, loss, error)) {
                return false;
            }
        }
    }
    return true;
}

void buildNewNumberingXmlFromCatalog(
    const NewNumberingCatalog& catalog, std::string& xml) {
    if (catalog.empty()) {
        xml.clear();
        return;
    }

    std::ostringstream numbering;
    numbering
        << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           "<w:numbering xmlns:w=\""
        << kWordNamespace << "\">";
    std::map<std::int32_t, std::int32_t> abstract_ids;
    std::int32_t next_abstract_id = 0;
    for (const auto& [num_id, levels] : catalog) {
        const auto abstract_id = next_abstract_id++;
        abstract_ids.emplace(num_id, abstract_id);
        numbering << "<w:abstractNum w:abstractNumId=\"" << abstract_id
                  << "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
        for (const auto& [level, definition] : levels) {
            numbering << "<w:lvl w:ilvl=\""
                      << static_cast<unsigned int>(level)
                      << "\"><w:start w:val=\"" << definition.start
                      << "\"/><w:numFmt w:val=\""
                      << numberFormatName(definition.format)
                      << "\"/><w:suff w:val=\""
                      << numberSuffixName(definition.suffix)
                      << "\"/><w:lvlText w:val=\""
                      << escapeXmlAttribute(definition.level_text)
                      << "\"/><w:lvlJc w:val=\"left\"/><w:pPr>";
            if (definition.tab_stop_twips) {
                numbering << "<w:tabs><w:tab w:val=\"num\" w:pos=\""
                          << *definition.tab_stop_twips
                          << "\"/></w:tabs>";
            }
            numbering << "<w:ind w:left=\""
                      << definition.text_indent_twips
                      << "\" w:hanging=\""
                      << definition.hanging_indent_twips
                      << "\"/></w:pPr></w:lvl>";
        }
        numbering << "</w:abstractNum>";
    }
    for (const auto& [num_id, abstract_id] : abstract_ids) {
        numbering << "<w:num w:numId=\"" << num_id
                  << "\"><w:abstractNumId w:val=\"" << abstract_id
                  << "\"/></w:num>";
    }
    numbering << "</w:numbering>";
    xml = numbering.str();
}

bool buildNewNumberingXml(
    const NewDocumentBody& body, std::string& xml, LossReport& loss,
    Error* error) {
    NewNumberingCatalog catalog;
    if (!collectNewNumbering(body, catalog, loss, error)) return false;
    buildNewNumberingXmlFromCatalog(catalog, xml);
    return true;
}

bool buildNewNumberingXml(
    const NewSectionedDocumentBody& body, std::string& xml,
    LossReport& loss, Error* error) {
    NewNumberingCatalog catalog;
    for (const auto& section : body.sections) {
        if (!collectNewNumbering(section.body, catalog, loss, error)) {
            return false;
        }
    }
    buildNewNumberingXmlFromCatalog(catalog, xml);
    return true;
}

std::string newContentTypes(
    bool has_numbering, bool has_header, bool has_footer,
    const std::vector<AuthoredImagePart>& images) {
    std::string result(kNewContentTypes);
    constexpr std::string_view closing = "</Types>";
    const auto position = result.rfind(closing);
    if (position != std::string::npos && has_numbering) {
        result.insert(
            position,
            "<Override PartName=\"/word/numbering.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>");
    }
    if (const auto header_position = result.rfind(closing);
        header_position != std::string::npos && has_header) {
        result.insert(header_position,
                      "<Override PartName=\"/word/header1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>");
    }
    if (const auto footer_position = result.rfind(closing);
        footer_position != std::string::npos && has_footer) {
        result.insert(footer_position,
                      "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>");
    }
    const bool has_png = std::any_of(
        images.begin(), images.end(), [](const auto& image) {
            return image.content_type == "image/png";
        });
    const bool has_jpeg = std::any_of(
        images.begin(), images.end(), [](const auto& image) {
            return image.content_type == "image/jpeg";
        });
    const auto updated_position = result.rfind(closing);
    if (updated_position != std::string::npos && has_png) {
        result.insert(updated_position,
                      "<Default Extension=\"png\" ContentType=\"image/png\"/>");
    }
    const auto jpeg_position = result.rfind(closing);
    if (jpeg_position != std::string::npos && has_jpeg) {
        result.insert(jpeg_position,
                      "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>");
    }
    return result;
}

std::string newDocumentRelationships(
    bool has_numbering, bool has_header, bool has_footer,
    const std::vector<AuthoredImagePart>& images) {
    std::string result(kNewDocumentRelationships);
    constexpr std::string_view closing = "</Relationships>";
    const auto position = result.rfind(closing);
    if (position != std::string::npos && has_numbering) {
        result.insert(
            position,
            "<Relationship Id=\"rId3\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" "
            "Target=\"numbering.xml\"/>");
    }
    if (const auto header_position = result.rfind(closing);
        header_position != std::string::npos && has_header) {
        result.insert(header_position,
                      "<Relationship Id=\"rIdHeader1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/header\" Target=\"header1.xml\"/>");
    }
    if (const auto footer_position = result.rfind(closing);
        footer_position != std::string::npos && has_footer) {
        result.insert(footer_position,
                      "<Relationship Id=\"rIdFooter1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer\" Target=\"footer1.xml\"/>");
    }
    for (const auto& image : images) {
        const auto image_position = result.rfind(closing);
        if (image_position == std::string::npos) break;
        result.insert(
            image_position,
            "<Relationship Id=\"" + image.relationship_id +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"" +
                image.relationship_target + "\"/>");
    }
    return result;
}

std::string buildNewStylesXml(
    const DocumentDefaults& defaults,
    const NewParagraphStyleCatalog& paragraph_styles) {
    const std::string family = escapeXmlAttribute(defaults.font_family);
    std::ostringstream styles;
    styles
        << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
           "<w:docDefaults><w:rPrDefault><w:rPr>"
           "<w:rFonts w:ascii=\""
        << family << "\" w:hAnsi=\"" << family << "\" w:cs=\"" << family
        << "\"/><w:sz w:val=\"" << defaults.font_size_half_points
        << "\"/><w:szCs w:val=\"" << defaults.font_size_half_points
        << "\"/></w:rPr></w:rPrDefault>"
           "<w:pPrDefault><w:pPr>"
           "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
           "</w:pPr></w:pPrDefault></w:docDefaults>"
           "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
           "<w:name w:val=\"Normal\"/><w:qFormat/>"
           "<w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr>"
           "<w:rPr><w:rFonts w:ascii=\""
        << family << "\" w:hAnsi=\"" << family << "\" w:cs=\"" << family
        << "\"/><w:sz w:val=\"" << defaults.font_size_half_points
        << "\"/><w:szCs w:val=\"" << defaults.font_size_half_points
        << "\"/></w:rPr></w:style>";

    for (const auto& descriptor : kBuiltInParagraphStyles) {
        if (!paragraph_styles.contains(std::string(descriptor.id))) {
            continue;
        }
        styles << "<w:style w:type=\"paragraph\" w:styleId=\""
               << descriptor.id << "\"><w:name w:val=\""
               << escapeXmlAttribute(descriptor.name)
               << "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\""
               << descriptor.next << "\"/><w:uiPriority w:val=\""
               << descriptor.ui_priority << "\"/><w:qFormat/><w:pPr>";
        if (descriptor.keep_with_next) styles << "<w:keepNext/>";
        if (descriptor.keep_lines) styles << "<w:keepLines/>";
        styles << "<w:spacing w:before=\""
               << descriptor.space_before_twips << "\" w:after=\""
               << descriptor.space_after_twips
               << "\" w:line=\"240\" w:lineRule=\"auto\"/>";
        if (descriptor.left_indent_twips != 0 ||
            descriptor.right_indent_twips != 0) {
            styles << "<w:ind";
            if (descriptor.left_indent_twips != 0) {
                styles << " w:left=\"" << descriptor.left_indent_twips
                       << "\"";
            }
            if (descriptor.right_indent_twips != 0) {
                styles << " w:right=\"" << descriptor.right_indent_twips
                       << "\"";
            }
            styles << "/>";
        }
        if (descriptor.alignment) {
            std::string_view value = "left";
            switch (*descriptor.alignment) {
                case BasicParagraphAlignment::left: value = "left"; break;
                case BasicParagraphAlignment::center: value = "center"; break;
                case BasicParagraphAlignment::right: value = "right"; break;
                case BasicParagraphAlignment::justified: value = "both"; break;
            }
            styles << "<w:jc w:val=\"" << value << "\"/>";
        }
        if (descriptor.outline_level >= 0) {
            styles << "<w:outlineLvl w:val=\""
                   << static_cast<unsigned int>(descriptor.outline_level)
                   << "\"/>";
        }
        styles << "</w:pPr>";

        const bool has_run_properties =
            descriptor.bold || descriptor.italic ||
            descriptor.foreground_rgb.has_value() ||
            descriptor.font_size_half_points > 0;
        if (has_run_properties) {
            styles << "<w:rPr>";
            if (descriptor.bold) styles << "<w:b/>";
            if (descriptor.italic) styles << "<w:i/>";
            if (descriptor.foreground_rgb) {
                styles << "<w:color w:val=\""
                       << rgbHex(*descriptor.foreground_rgb) << "\"/>";
            }
            if (descriptor.font_size_half_points > 0) {
                styles << "<w:sz w:val=\""
                       << descriptor.font_size_half_points
                       << "\"/><w:szCs w:val=\""
                       << descriptor.font_size_half_points << "\"/>";
            }
            styles << "</w:rPr>";
        }
        styles << "</w:style>";
    }
    styles << "</w:styles>";
    return styles.str();
}

std::string buildNewSettingsXml(const DocumentDefaults& defaults) {
    std::ostringstream settings;
    settings
        << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
           "<w:defaultTabStop w:val=\""
        << defaults.default_tab_stop_twips << "\"/></w:settings>";
    return settings.str();
}

}  // namespace

struct DocxDocument::Impl {
    std::filesystem::path source_path;
    OpenOptions options;
    ParsedPackage package;
    std::vector<PackageMember> public_members;
    std::unordered_map<TextSpanId, std::size_t> span_indices;
    std::unordered_map<TextSpanId, std::string> edits;
};

bool LossReport::hasBlockers() const noexcept {
    return std::any_of(issues.begin(), issues.end(), [](const CompatibilityIssue& issue) {
        return issue.severity == IssueSeverity::blocking;
    });
}

bool CompatibilityReport::allowsTextPatching() const noexcept {
    return classification == CompatibilityClass::safe_text_patch ||
           classification == CompatibilityClass::basic_body_text_patch;
}

std::string Run::plainText() const {
    std::string text;
    for (const RunFragment& fragment : fragments) {
        switch (fragment.kind) {
            case FragmentKind::text:
                text += fragment.text;
                break;
            case FragmentKind::tab:
                text += '\t';
                break;
            case FragmentKind::line_break:
                text += '\n';
                break;
            case FragmentKind::page_break:
            case FragmentKind::inline_image:
                // A page break affects pagination rather than visible text.
                // Inline pictures are exposed through their structured
                // payload and do not masquerade as editable characters.
                break;
            case FragmentKind::equation:
                // A single object-replacement character keeps document-model
                // offsets aligned without leaking serialization source into
                // visible text. The structured payload remains on the fragment.
                text += "\xef\xbf\xbc";
                break;
        }
    }
    return text;
}

std::string Paragraph::plainText() const {
    std::string text;
    for (const Run& run : runs) {
        text += run.plainText();
    }
    return text;
}

DocxDocument::DocxDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DocxDocument::~DocxDocument() = default;
DocxDocument::DocxDocument(DocxDocument&&) noexcept = default;
DocxDocument& DocxDocument::operator=(DocxDocument&&) noexcept = default;

std::unique_ptr<DocxDocument> DocxDocument::open(
    const std::filesystem::path& path,
    Error* error,
    const OpenOptions& options) {
    if (error != nullptr) {
        *error = {};
    }
    if (options.max_package_bytes == 0 || options.max_document_xml_bytes == 0 ||
        options.max_member_uncompressed_bytes == 0 || options.max_total_uncompressed_bytes == 0 ||
        options.max_member_count == 0 || options.max_xml_depth == 0 ||
        options.max_xml_nodes == 0) {
        setError(error, ErrorCode::package_limit_exceeded,
                 "All DOCX open limits must be greater than zero");
        return nullptr;
    }

    std::vector<std::uint8_t> bytes;
    std::uint32_t source_mode = 0600;
    if (!readFile(path, options.max_package_bytes, bytes, source_mode, error)) {
        return nullptr;
    }

    ParsedPackage parsed;
    if (!inspectPackage(std::move(bytes), source_mode, options, parsed, error)) {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->source_path = path;
    impl->options = options;
    impl->package = std::move(parsed);
    impl->public_members.reserve(impl->package.entries.size());
    for (const EntryRecord& entry : impl->package.entries) {
        impl->public_members.push_back(entry.member);
    }
    for (std::size_t index = 0; index < impl->package.spans.size(); ++index) {
        impl->span_indices.emplace(impl->package.spans[index].id, index);
    }
    return std::unique_ptr<DocxDocument>(new DocxDocument(std::move(impl)));
}

const std::filesystem::path& DocxDocument::sourcePath() const noexcept {
    return impl_->source_path;
}

const std::vector<PackageMember>& DocxDocument::packageMembers() const noexcept {
    return impl_->public_members;
}

const std::vector<Paragraph>& DocxDocument::paragraphs() const noexcept {
    return impl_->package.paragraphs;
}

const std::vector<ImportedBodyBlock>& DocxDocument::bodyBlocks() const noexcept {
    return impl_->package.body_blocks;
}

const std::optional<PageSettings>& DocxDocument::bodyPageSettings() const noexcept {
    return impl_->package.page_settings;
}

const std::vector<ImportedSection>& DocxDocument::sections() const noexcept {
    return impl_->package.sections;
}

const std::optional<std::string>& DocxDocument::headerText() const noexcept {
    return impl_->package.header_text;
}

const std::optional<std::string>& DocxDocument::footerText() const noexcept {
    return impl_->package.footer_text;
}

const CompatibilityReport& DocxDocument::compatibility() const noexcept {
    return impl_->package.compatibility;
}

bool DocxDocument::isCanonicalRegeneratableSimplePackage(
    const DocumentDefaults& regeneration_defaults) const noexcept {
    const auto& serialized_defaults =
        impl_->package.canonical_simple_regeneration_defaults;
    return serialized_defaults &&
           *serialized_defaults == regeneration_defaults;
}

bool DocxDocument::dirty() const noexcept {
    return !impl_->edits.empty();
}

EditResult DocxDocument::replaceText(TextSpanId id, std::string text) {
    EditResult result;
    result.loss_report = globalBlockingIssues(impl_->package.compatibility);
    if (result.loss_report.hasBlockers()) {
        result.error = Error{ErrorCode::unsafe_edit,
                             "This package is classified for exact unchanged round-trip only"};
        return result;
    }

    const auto found_index = impl_->span_indices.find(id);
    if (found_index == impl_->span_indices.end()) {
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::invalid_text_span, "The requested text span does not exist"));
        result.error = Error{ErrorCode::unsafe_edit, "The requested text span does not exist"};
        return result;
    }
    SpanLocation& span = impl_->package.spans[found_index->second];
    if (!span.editable) {
        const std::string detail = span.refusal_reason.empty()
                                       ? "The requested text span is not in a supported basic context"
                                       : span.refusal_reason;
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::unsupported_text_context, detail, span.paragraph_index));
        result.error = Error{ErrorCode::unsafe_edit, detail};
        return result;
    }

    RunFragment& current_fragment =
        impl_->package.paragraphs[span.paragraph_index]
            .runs[span.run_index]
            .fragments[span.fragment_index];
    if (current_fragment.text == text) {
        result.accepted = true;
        return result;
    }
    if (text == span.original_text) {
        current_fragment.text = span.original_text;
        impl_->edits.erase(id);
        result.accepted = true;
        return result;
    }

    IssueCode validation_issue = IssueCode::invalid_utf8;
    std::string validation_detail;
    if (!isValidUtf8XmlText(text, validation_issue, validation_detail)) {
        result.loss_report.issues.push_back(blockingIssue(
            validation_issue, validation_detail, span.paragraph_index));
        result.error = Error{ErrorCode::unsafe_edit, validation_detail};
        return result;
    }
    if (text.find_first_of("\t\r\n") != std::string::npos) {
        const std::string detail =
            "Tabs and line breaks require w:tab/w:br elements and cannot be inserted as a text-only patch";
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, detail, span.paragraph_index));
        result.error = Error{ErrorCode::unsafe_edit, detail};
        return result;
    }
    if (!span.preserves_space && !text.empty() &&
        (text.front() == ' ' || text.back() == ' ')) {
        const std::string detail =
            "Leading or trailing spaces require adding xml:space=\"preserve\"; this text-only patch refuses that structural change";
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::whitespace_preservation_required, detail, span.paragraph_index));
        result.error = Error{ErrorCode::unsafe_edit, detail};
        return result;
    }

    current_fragment.text = text;
    if (text == span.original_text) {
        impl_->edits.erase(id);
    } else {
        impl_->edits[id] = std::move(text);
    }
    result.accepted = true;
    return result;
}

EditResult DocxDocument::replaceParagraphText(
    std::size_t paragraph_index,
    std::string text) {
    if (paragraph_index >= impl_->package.paragraphs.size()) {
        EditResult result;
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::invalid_text_span, "The requested paragraph does not exist", paragraph_index));
        result.error = Error{ErrorCode::unsafe_edit, "The requested paragraph does not exist"};
        return result;
    }
    const Paragraph& paragraph = impl_->package.paragraphs[paragraph_index];
    if (!paragraph.whole_text_editable) {
        EditResult result;
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required,
            "Whole-paragraph replacement is safe only for one basic editable text span",
            paragraph_index));
        result.error = Error{
            ErrorCode::unsafe_edit,
            "Whole-paragraph replacement would require choosing how to redistribute formatting"};
        return result;
    }
    for (const Run& run : paragraph.runs) {
        for (const RunFragment& fragment : run.fragments) {
            if (fragment.text_span_id.has_value()) {
                return replaceText(*fragment.text_span_id, std::move(text));
            }
        }
    }
    EditResult result;
    result.loss_report.issues.push_back(blockingIssue(
        IssueCode::invalid_text_span, "The paragraph has no mapped text span", paragraph_index));
    result.error = Error{ErrorCode::unsafe_edit, "The paragraph has no mapped text span"};
    return result;
}

void DocxDocument::discardEdits() {
    for (const SpanLocation& span : impl_->package.spans) {
        impl_->package.paragraphs[span.paragraph_index]
            .runs[span.run_index]
            .fragments[span.fragment_index]
            .text = span.original_text;
    }
    impl_->edits.clear();
}

SaveResult DocxDocument::saveAs(const std::filesystem::path& target) {
    SaveResult result;
    const bool modified = dirty();
    if (modified) {
        result.loss_report = globalBlockingIssues(impl_->package.compatibility);
        if (result.loss_report.hasBlockers()) {
            result.error = Error{
                ErrorCode::unsafe_edit,
                "Modified save was refused because the compatibility report contains blockers"};
            return result;
        }
    }

    Error save_error;
    auto temporary = AtomicTempFile::create(target, impl_->package.source_mode, &save_error);
    if (!temporary.has_value()) {
        result.error = std::move(save_error);
        return result;
    }

    std::string output_document_xml = impl_->package.document_xml;
    if (!modified) {
        if (!writeAll(temporary->descriptor(), impl_->package.bytes, &save_error) ||
            !temporary->closeDescriptor(&save_error)) {
            result.error = std::move(save_error);
            return result;
        }
    } else {
        output_document_xml = applyTextPatches(
            impl_->package.document_xml, impl_->package.spans, impl_->edits);
        if (!temporary->closeDescriptor(&save_error) ||
            !writeModifiedArchive(temporary->path(), impl_->package, output_document_xml, &save_error) ||
            !temporary->syncClosedFile(&save_error)) {
            result.error = std::move(save_error);
            return result;
        }
    }

    const std::vector<std::uint8_t>* exact_bytes = modified ? nullptr : &impl_->package.bytes;
    if (!validateSavedPackage(
            temporary->path(),
            &impl_->package,
            output_document_xml,
            exact_bytes,
            impl_->options,
            &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    if (!temporary->commit(target, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }

    result.saved = true;
    result.byte_identical_to_opened_file = !modified;
    result.preserved_member_count = modified && !impl_->package.entries.empty()
                                        ? impl_->package.entries.size() - 1
                                        : impl_->package.entries.size();

    Error reopen_error;
    std::unique_ptr<DocxDocument> reopened = DocxDocument::open(target, &reopen_error, impl_->options);
    if (reopened != nullptr) {
        impl_ = std::move(reopened->impl_);
    } else {
        // The pre-commit copy was already reopened and validated. Keep the
        // saved result successful, but surface the unexpected post-rename
        // state and leave this instance on its prior baseline.
        result.error = std::move(reopen_error);
    }
    return result;
}

SaveResult DocxDocument::writeNew(
    const std::filesystem::path& target,
    const std::vector<NewParagraph>& paragraphs,
    const PageSettings& page,
    const DocumentDefaults& defaults) {
    NewDocumentBody body;
    body.blocks.reserve(paragraphs.size());
    for (const auto& paragraph : paragraphs) {
        body.blocks.emplace_back(paragraph);
    }
    return writeNew(target, body, page, defaults);
}

SaveResult DocxDocument::writeNew(
    const std::filesystem::path& target,
    const NewDocumentBody& body,
    const PageSettings& page,
    const DocumentDefaults& defaults) {
    SaveResult result;
    if (!validPageSettings(page)) {
        const std::string message =
            "Page size or margins are outside the supported range";
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message));
        result.error = Error{ErrorCode::unsafe_edit, message};
        return result;
    }
    IssueCode defaults_issue = IssueCode::invalid_utf8;
    std::string defaults_detail;
    const bool defaults_valid =
        !defaults.font_family.empty() &&
        isValidUtf8XmlText(defaults.font_family, defaults_issue,
                           defaults_detail) &&
        defaults.font_size_half_points >= 2 &&
        defaults.font_size_half_points <= 3276 &&
        defaults.default_tab_stop_twips >= 1 &&
        defaults.default_tab_stop_twips <= 31680;
    if (!defaults_valid) {
        const std::string message = defaults_detail.empty()
            ? "Document defaults contain an invalid font, size, or tab stop"
            : defaults_detail;
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message));
        result.error = Error{ErrorCode::unsafe_edit, message};
        return result;
    }
    Error save_error;
    NewParagraphStyleCatalog paragraph_styles;
    if (!collectNewParagraphStyles(
            body, paragraph_styles, result.loss_report, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    std::vector<AuthoredImagePart> images;
    if (!collectBodyImages(
            body, images, result.loss_report, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    std::string document_xml;
    std::size_t image_index = 0;
    if (!buildNewDocumentXml(body, page, document_xml,
                             result.loss_report, &save_error, images,
                             image_index) ||
        image_index != images.size()) {
        if (save_error.code == ErrorCode::none) {
            setError(&save_error, ErrorCode::save_validation_failed,
                     "Inline image catalog and document traversal differ");
        }
        result.error = std::move(save_error);
        return result;
    }
    std::string numbering_xml;
    if (!buildNewNumberingXml(
            body, numbering_xml, result.loss_report, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    const bool has_numbering = !numbering_xml.empty();
    const bool has_header = body.header_text && !body.header_text->empty();
    const bool has_footer = body.footer_text && !body.footer_text->empty();
    for (const auto* story : {body.header_text ? &*body.header_text : nullptr,
                              body.footer_text ? &*body.footer_text : nullptr}) {
        if (!story) continue;
        IssueCode issue = IssueCode::invalid_utf8;
        std::string detail;
        if (story->size() > 65536U ||
            !isValidUtf8XmlText(*story, issue, detail)) {
            const std::string message = detail.empty()
                ? "Header or footer text exceeds the size limit" : detail;
            result.loss_report.issues.push_back(blockingIssue(issue, message));
            result.error = Error{ErrorCode::unsafe_edit, message};
            return result;
        }
    }
    const std::string header_xml = has_header
        ? buildStoryXml(*body.header_text, true, page) : std::string{};
    const std::string footer_xml = has_footer
        ? buildStoryXml(*body.footer_text, false, page) : std::string{};
    const std::string content_types = newContentTypes(
        has_numbering, has_header, has_footer, images);
    const std::string document_relationships =
        newDocumentRelationships(has_numbering, has_header, has_footer, images);

    auto temporary = AtomicTempFile::create(target, 0600U, &save_error);
    if (!temporary.has_value()) {
        result.error = std::move(save_error);
        return result;
    }
    if (!temporary->closeDescriptor(&save_error) ||
        !writeNewArchive(
            temporary->path(),
            content_types,
            kNewRootRelationships,
            document_relationships,
            buildNewStylesXml(defaults, paragraph_styles),
            buildNewSettingsXml(defaults),
            numbering_xml,
            document_xml,
            header_xml,
            footer_xml,
            images,
            &save_error) ||
        !temporary->syncClosedFile(&save_error) ||
        !validateSavedPackage(
            temporary->path(), nullptr, document_xml, nullptr, OpenOptions{}, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    if (!temporary->commit(target, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    result.saved = true;
    result.byte_identical_to_opened_file = false;
    result.preserved_member_count = 0;
    return result;
}

SaveResult DocxDocument::writeNew(
    const std::filesystem::path& target,
    const NewSectionedDocumentBody& body,
    const DocumentDefaults& defaults) {
    SaveResult result;
    IssueCode defaults_issue = IssueCode::invalid_utf8;
    std::string defaults_detail;
    const bool defaults_valid =
        !defaults.font_family.empty() &&
        isValidUtf8XmlText(defaults.font_family, defaults_issue,
                           defaults_detail) &&
        defaults.font_size_half_points >= 2 &&
        defaults.font_size_half_points <= 3276 &&
        defaults.default_tab_stop_twips >= 1 &&
        defaults.default_tab_stop_twips <= 31680;
    if (!defaults_valid) {
        const std::string message = defaults_detail.empty()
            ? "Document defaults contain an invalid font, size, or tab stop"
            : defaults_detail;
        result.loss_report.issues.push_back(blockingIssue(
            IssueCode::structural_rewrite_required, message));
        result.error = Error{ErrorCode::unsafe_edit, message};
        return result;
    }

    Error save_error;
    NewParagraphStyleCatalog paragraph_styles;
    for (const auto& section : body.sections) {
        if (!collectNewParagraphStyles(
                section.body, paragraph_styles, result.loss_report,
                &save_error)) {
            result.error = std::move(save_error);
            return result;
        }
    }
    std::vector<AuthoredImagePart> images;
    for (const auto& section : body.sections) {
        if (!collectBodyImages(
                section.body, images, result.loss_report, &save_error)) {
            result.error = std::move(save_error);
            return result;
        }
    }
    std::string document_xml;
    if (!buildNewSectionedDocumentXml(
            body, document_xml, result.loss_report, &save_error, images)) {
        result.error = std::move(save_error);
        return result;
    }
    std::string numbering_xml;
    if (!buildNewNumberingXml(
            body, numbering_xml, result.loss_report, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    const bool has_numbering = !numbering_xml.empty();
    const std::string content_types =
        newContentTypes(has_numbering, false, false, images);
    const std::string document_relationships =
        newDocumentRelationships(has_numbering, false, false, images);

    auto temporary = AtomicTempFile::create(target, 0600U, &save_error);
    if (!temporary.has_value()) {
        result.error = std::move(save_error);
        return result;
    }
    if (!temporary->closeDescriptor(&save_error) ||
        !writeNewArchive(
            temporary->path(), content_types, kNewRootRelationships,
            document_relationships,
            buildNewStylesXml(defaults, paragraph_styles),
            buildNewSettingsXml(defaults), numbering_xml, document_xml,
            {}, {}, images, &save_error) ||
        !temporary->syncClosedFile(&save_error) ||
        !validateSavedPackage(
            temporary->path(), nullptr, document_xml, nullptr,
            OpenOptions{}, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    if (!temporary->commit(target, &save_error)) {
        result.error = std::move(save_error);
        return result;
    }
    result.saved = true;
    result.byte_identical_to_opened_file = false;
    result.preserved_member_count = 0;
    return result;
}

}  // namespace docxstudio::ooxml
