#include "docxstudio/ooxml/docx_document.h"
#include "docxstudio/math/ast.h"
#include "docxstudio/math/latex_parser.h"

#include <zip.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using docxstudio::ooxml::BasicParagraphAlignment;
using docxstudio::ooxml::BasicBaseline;
using docxstudio::ooxml::BasicLineSpacingRule;
using docxstudio::ooxml::BasicRunFormat;
using docxstudio::ooxml::BasicTableStyle;
using docxstudio::ooxml::CompatibilityClass;
using docxstudio::ooxml::DocxDocument;
using docxstudio::ooxml::DocumentDefaults;
using docxstudio::ooxml::Error;
using docxstudio::ooxml::EquationPayload;
using docxstudio::ooxml::FragmentKind;
using docxstudio::ooxml::ImportedParagraphBlock;
using docxstudio::ooxml::ImportedTableBlock;
using docxstudio::ooxml::ImportedUnsupportedBodyBlock;
using docxstudio::ooxml::IssueCode;
using docxstudio::ooxml::NewDocumentBody;
using docxstudio::ooxml::NewParagraph;
using docxstudio::ooxml::NewRun;
using docxstudio::ooxml::NewTable;
using docxstudio::ooxml::OpenOptions;

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/docxstudio-ooxml-tests-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
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

std::vector<unsigned char> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "could not open test file");
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void addMember(zip_t* archive, const std::string& name, const std::string& contents) {
    void* owned_contents = nullptr;
    if (!contents.empty()) {
        owned_contents = std::malloc(contents.size());
        check(owned_contents != nullptr, "malloc failed for ZIP fixture");
        std::memcpy(owned_contents, contents.data(), contents.size());
    }
    zip_source_t* source = zip_source_buffer(
        archive,
        owned_contents,
        static_cast<zip_uint64_t>(contents.size()),
        1);
    if (source == nullptr) {
        std::free(owned_contents);
    }
    check(source != nullptr, "zip_source_buffer failed");
    const zip_int64_t index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        throw std::runtime_error("zip_file_add failed");
    }
    check(
        zip_set_file_compression(
            archive, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 6) == 0,
        "zip_set_file_compression failed");
}

void createPackage(
    const std::filesystem::path& path,
    const std::string& document_xml,
    bool signed_package = false,
    std::string_view main_content_type =
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml") {
    const std::string content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"" + std::string(main_content_type) + "\"/>"
        "</Types>";
    static const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "</Relationships>";

    int error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    check(archive != nullptr, "zip_open failed while creating fixture");
    addMember(archive, "[Content_Types].xml", content_types);
    addMember(archive, "_rels/.rels", relationships);
    addMember(archive, "word/document.xml", document_xml);
    addMember(archive, "customXml/item1.bin", std::string("opaque\0payload", 14));
    if (signed_package) {
        addMember(archive, "_xmlsignatures/sig1.xml", "<Signature>test-only</Signature>");
    }
    check(zip_close(archive) == 0, "zip_close failed while creating fixture");
}

void appendMembers(
    const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& members) {
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), 0, &error);
    check(archive != nullptr, "zip_open failed while extending fixture");
    for (const auto& [name, contents] : members) {
        addMember(archive, name, contents);
    }
    check(zip_close(archive) == 0, "zip_close failed while extending fixture");
}

void replaceMember(
    const std::filesystem::path& path, const std::string& name,
    const std::string& contents) {
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), 0, &error);
    check(archive != nullptr, "zip_open failed while replacing fixture member");
    void* owned_contents = nullptr;
    if (!contents.empty()) {
        owned_contents = std::malloc(contents.size());
        check(owned_contents != nullptr,
              "malloc failed for replacement ZIP fixture");
        std::memcpy(owned_contents, contents.data(), contents.size());
    }
    zip_source_t* source = zip_source_buffer(
        archive, owned_contents,
        static_cast<zip_uint64_t>(contents.size()), 1);
    if (source == nullptr) std::free(owned_contents);
    check(source != nullptr, "zip_source_buffer failed for replacement member");
    const zip_int64_t index = zip_file_add(
        archive, name.c_str(), source,
        ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        zip_discard(archive);
        throw std::runtime_error("zip_file_add replacement failed");
    }
    check(zip_set_file_compression(
              archive, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE,
              6) == 0,
          "zip_set_file_compression failed for replacement member");
    check(zip_close(archive) == 0,
          "zip_close failed while replacing fixture member");
}

std::string readMember(const std::filesystem::path& path, const char* name) {
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
    check(archive != nullptr, "could not reopen test DOCX");
    zip_stat_t status;
    zip_stat_init(&status);
    check(zip_stat(archive, name, ZIP_FL_UNCHANGED, &status) == 0, "missing test package member");
    zip_file_t* member = zip_fopen(archive, name, ZIP_FL_UNCHANGED);
    check(member != nullptr, "could not open test package member");
    std::string contents(static_cast<std::size_t>(status.size), '\0');
    std::size_t consumed = 0;
    while (consumed < contents.size()) {
        const zip_int64_t count = zip_fread(member, contents.data() + consumed, contents.size() - consumed);
        check(count > 0, "could not read complete test package member");
        consumed += static_cast<std::size_t>(count);
    }
    check(zip_fclose(member) == 0, "could not close test package member");
    zip_discard(archive);
    return contents;
}

constexpr std::string_view kWordNamespace =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";

std::string documentWithBody(std::string_view body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<w:document xmlns:w=\"" +
           std::string(kWordNamespace) + "\"><w:body>" + std::string(body) +
           "<w:sectPr/></w:body></w:document>";
}

std::size_t countOccurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string extractElement(
    std::string_view xml,
    std::string_view opening_tag,
    std::string_view closing_tag) {
    const std::size_t begin = xml.find(opening_tag);
    check(begin != std::string_view::npos, "fixture element opening tag is missing");
    const std::size_t closing_begin = xml.find(closing_tag, begin);
    check(closing_begin != std::string_view::npos,
          "fixture element closing tag is missing");
    return std::string(xml.substr(
        begin, closing_begin + closing_tag.size() - begin));
}

void checkAuthoredNumberingLevelOrder(
    std::string_view xml, std::size_t expected_levels) {
    std::size_t cursor = 0;
    std::size_t levels = 0;
    while ((cursor = xml.find("<w:lvl ", cursor)) != std::string_view::npos) {
        const auto end = xml.find("</w:lvl>", cursor);
        check(end != std::string_view::npos,
              "authored numbering level is not closed");
        const auto level = xml.substr(cursor, end - cursor);
        const auto start = level.find("<w:start ");
        const auto format = level.find("<w:numFmt ");
        const auto suffix = level.find("<w:suff ");
        const auto text = level.find("<w:lvlText ");
        const auto justification = level.find("<w:lvlJc ");
        const auto properties = level.find("<w:pPr>");
        check(start != std::string_view::npos &&
                  format != std::string_view::npos &&
                  suffix != std::string_view::npos &&
                  text != std::string_view::npos &&
                  justification != std::string_view::npos &&
                  properties != std::string_view::npos &&
                  start < format && format < suffix && suffix < text &&
                  text < justification && justification < properties,
              "authored CT_Lvl children are not in schema sequence order");
        ++levels;
        cursor = end + std::string_view("</w:lvl>").size();
    }
    check(levels == expected_levels,
          "authored numbering.xml has an unexpected level count");
    check(xml.find("w:ilvl=\"9\"") == std::string_view::npos &&
              xml.find("%10") == std::string_view::npos,
          "authored numbering.xml exceeds the nine-level OOXML limit");
}

void testInlineImagePageBreakAndTablePresentation(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("presentation.docx");
    const auto output = temporary.file("presentation-edited.docx");
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:drawing>"
        "<wp:inline><wp:extent cx=\"914400\" cy=\"457200\"/>"
        "<wp:docPr id=\"1\" name=\"Diagram\"/>"
        "<a:graphic><a:graphicData><a:blip r:embed=\"rIdImage\"/>"
        "</a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p>"
        "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"
        "<w:tbl><w:tblPr><w:jc w:val=\"center\"/>"
        "<w:tblLook w:firstRow=\"1\"/></w:tblPr>"
        "<w:tblGrid><w:gridCol w:w=\"2400\"/></w:tblGrid><w:tr><w:tc>"
        "<w:tcPr><w:tcW w:type=\"dxa\" w:w=\"2300\"/>"
        "<w:vAlign w:val=\"center\"/>"
        "<w:tcBorders><w:top w:val=\"single\" w:sz=\"8\" w:color=\"D7DCD7\"/>"
        "<w:left w:val=\"single\" w:sz=\"8\" w:color=\"D7DCD7\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"8\" w:color=\"D7DCD7\"/>"
        "<w:right w:val=\"single\" w:sz=\"8\" w:color=\"D7DCD7\"/>"
        "</w:tcBorders><w:tcMar>"
        "<w:top w:type=\"dxa\" w:w=\"90\"/>"
        "<w:start w:type=\"dxa\" w:w=\"110\"/>"
        "<w:bottom w:type=\"dxa\" w:w=\"90\"/>"
        "<w:end w:type=\"dxa\" w:w=\"110\"/>"
        "</w:tcMar><w:shd w:fill=\"173B32\"/></w:tcPr>"
        "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr>"
        "<w:r><w:rPr><w:rFonts w:ascii=\"Ubuntu\" w:hAnsi=\"Ubuntu\"/>"
        "<w:b/><w:color w:val=\"FFFFFF\"/><w:sz w:val=\"18\"/></w:rPr>"
        "<w:t>Header</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:p><w:r><w:t>After break</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(path, document_xml);
    static const std::string document_relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdImage\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
        "Target=\"media/test.png\"/>"
        "</Relationships>";
    const std::string image_bytes("\x89PNG\r\n\x1a\nfixture", 15);
    appendMembers(path, {
        {"word/_rels/document.xml.rels", document_relationships},
        {"word/media/test.png", image_bytes}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->paragraphs().size() == 4,
          "presentation fixture paragraph count differs");
    const auto& image_fragment = document->paragraphs()[0].runs[0].fragments[0];
    check(image_fragment.kind == FragmentKind::inline_image &&
              image_fragment.inline_image.has_value(),
          "inline DrawingML image was not parsed");
    check(image_fragment.inline_image->width_emu == 914400 &&
              image_fragment.inline_image->height_emu == 457200,
          "inline image extent was not parsed");
    check(image_fragment.inline_image->package_member == "word/media/test.png" &&
              image_fragment.inline_image->content_type == "image/png" &&
              image_fragment.inline_image->bytes.size() == 15,
          "internal image relationship was not resolved to bounded package bytes");
    check(document->paragraphs()[1].hard_page_break_after &&
              document->paragraphs()[1].runs[0].fragments[0].kind ==
                  FragmentKind::page_break,
          "hard page break was flattened into a soft line break");

    const auto* table = std::get_if<ImportedTableBlock>(
        &document->bodyBlocks()[2]);
    check(table != nullptr && table->rows == 1 && table->columns == 1,
          "styled table was not imported as a semantic table");
    check(table->alignment == BasicParagraphAlignment::center &&
              table->column_widths_twips == std::vector<std::uint32_t>{2400},
          "table alignment or authoritative grid width was not parsed");
    const auto& cell = table->cells.front();
    check(cell.width_twips == 2300 && cell.margin_top_twips == 90 &&
              cell.margin_right_twips == 110 &&
              cell.margin_bottom_twips == 90 &&
              cell.margin_left_twips == 110,
          "table cell width or padding was not parsed");
    check(cell.fill_rgb == 0x173B32U &&
              cell.vertical_alignment ==
                  docxstudio::ooxml::BasicVerticalAlignment::center,
          "table cell fill or vertical alignment was not parsed");
    check(cell.border_top && cell.border_top->rgb == 0xD7DCD7U &&
              cell.border_top->width_eighth_points == 8 &&
              cell.border_right && cell.border_bottom && cell.border_left,
          "table cell borders were not parsed");

    const auto image_before = *image_fragment.inline_image;
    const auto table_before = *table;
    const std::string table_xml_before = extractElement(
        readMember(path, "word/document.xml"), "<w:tbl>", "</w:tbl>");
    const auto& editable_fragment =
        document->paragraphs()[3].runs[0].fragments[0];
    check(editable_fragment.kind == FragmentKind::text &&
              editable_fragment.text_span_id.has_value(),
          "text after the presentation fixture was not editable");
    const auto edit = document->replaceText(
        *editable_fragment.text_span_id, "After supported edit");
    check(edit.accepted,
          edit.error ? edit.error->message : "supported presentation edit was refused");
    const auto save = document->saveAs(output);
    check(save.saved,
          save.error ? save.error->message : "presentation fixture save failed");

    check(readMember(output, "word/media/test.png") == image_bytes,
          "supported text edit changed the image package member bytes");
    check(readMember(output, "word/_rels/document.xml.rels") ==
              document_relationships,
          "supported text edit changed the image relationship part");
    const std::string saved_document_xml =
        readMember(output, "word/document.xml");
    check(extractElement(saved_document_xml, "<w:tbl>", "</w:tbl>") ==
              table_xml_before,
          "supported text edit changed unrelated table presentation XML");
    check(saved_document_xml.find("<w:t>After supported edit</w:t>") !=
              std::string::npos,
          "supported text edit was absent from the saved document XML");

    error = {};
    auto reopened = DocxDocument::open(output, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 4 &&
              reopened->paragraphs()[3].plainText() == "After supported edit",
          "edited presentation text did not survive save and reopen");
    const auto& reopened_image =
        reopened->paragraphs()[0].runs[0].fragments[0];
    check(reopened_image.kind == FragmentKind::inline_image &&
              reopened_image.inline_image.has_value() &&
              *reopened_image.inline_image == image_before,
          "inline image data changed across supported edit/save/reopen");
    const auto* reopened_table = std::get_if<ImportedTableBlock>(
        &reopened->bodyBlocks()[2]);
    check(reopened_table != nullptr && *reopened_table == table_before,
          "table presentation changed across supported edit/save/reopen");
}

void testUnsupportedAnchorIsOpaqueAndPreserved(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("unsupported-anchor.docx");
    const auto output = temporary.file("unsupported-anchor-edited.docx");
    const std::string anchor =
        "<wp:anchor distT=\"10\" distB=\"20\" distL=\"30\" distR=\"40\" "
        "simplePos=\"0\" relativeHeight=\"0\" behindDoc=\"0\" locked=\"0\" "
        "layoutInCell=\"1\" allowOverlap=\"0\">"
        "<wp:simplePos x=\"0\" y=\"0\"/>"
        "<wp:positionH relativeFrom=\"character\"><wp:posOffset>0</wp:posOffset>"
        "</wp:positionH><wp:positionV relativeFrom=\"paragraph\">"
        "<wp:posOffset>0</wp:posOffset></wp:positionV>"
        "<wp:extent cx=\"914400\" cy=\"457200\"/>"
        "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>"
        // Tight wrapping requires a polygon that Picture Layout v1 cannot
        // represent. It must therefore stay raw and preservation-only.
        "<wp:wrapTight wrapText=\"bothSides\"><wp:wrapPolygon edited=\"0\">"
        "<wp:start x=\"0\" y=\"0\"/><wp:lineTo x=\"1\" y=\"1\"/>"
        "</wp:wrapPolygon></wp:wrapTight>"
        "<wp:docPr id=\"1\" name=\"Opaque owl\" descr=\"Do not flatten\"/>"
        "<a:graphic><a:graphicData "
        "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
        "<pic:pic><pic:blipFill><a:blip r:embed=\"rIdOpaque\"/>"
        "</pic:blipFill></pic:pic></a:graphicData></a:graphic>"
        "</wp:anchor>";
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
        "<w:body><w:p><w:r><w:drawing>" + anchor +
        "</w:drawing></w:r><w:r><w:t>Editable tail</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(path, document_xml);
    const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdOpaque\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
        "Target=\"media/opaque.png\"/></Relationships>";
    const std::string image_bytes("\x89PNG\r\n\x1a\nopaque-anchor", 21);
    appendMembers(
        path,
        {{"word/_rels/document.xml.rels", relationships},
         {"word/media/opaque.png", image_bytes}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->compatibility().classification ==
              CompatibilityClass::safe_text_patch,
          "unsupported anchor was exposed as a fully editable picture");
    std::optional<docxstudio::ooxml::TextSpanId> editable_span;
    bool exposed_picture = false;
    for (const auto& run : document->paragraphs().front().runs) {
        for (const auto& fragment : run.fragments) {
            exposed_picture = exposed_picture ||
                fragment.kind == FragmentKind::inline_image;
            if (fragment.kind == FragmentKind::text &&
                fragment.text == "Editable tail") {
                editable_span = fragment.text_span_id;
            }
        }
    }
    check(!exposed_picture && editable_span.has_value(),
          "unsupported anchor was flattened or blocked adjacent text editing");
    const auto edit = document->replaceText(*editable_span, "Changed tail");
    check(edit.accepted,
          edit.error ? edit.error->message
                     : "safe edit beside opaque anchor was refused");
    const auto save = document->saveAs(output);
    check(save.saved,
          save.error ? save.error->message
                     : "opaque-anchor preservation save failed");
    const std::string saved_xml = readMember(output, "word/document.xml");
    check(extractElement(saved_xml, "<wp:anchor", "</wp:anchor>") ==
              anchor &&
              saved_xml.find("<w:t>Changed tail</w:t>") !=
                  std::string::npos,
          "unsupported anchor changed or adjacent text edit was lost");
    check(readMember(output, "word/media/opaque.png") == image_bytes &&
              readMember(output, "word/_rels/document.xml.rels") ==
                  relationships,
          "opaque anchor media or relationship bytes changed");
}

void testInlineImageTableCellIsPreservedViewOnly(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("image-table-cell.docx");
    const auto output = temporary.file("image-table-cell-edited.docx");
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<w:body><w:tbl><w:tblGrid><w:gridCol w:w=\"2400\"/>"
        "</w:tblGrid><w:tr><w:tc><w:p><w:r><w:drawing><wp:inline>"
        "<wp:extent cx=\"914400\" cy=\"457200\"/>"
        "<wp:docPr id=\"1\" name=\"Cell image\"/>"
        "<a:graphic><a:graphicData><a:blip r:embed=\"rIdCellImage\"/>"
        "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>"
        "</w:p></w:tc></w:tr></w:tbl>"
        "<w:p><w:r><w:t>Editable tail</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(path, document_xml);
    const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdCellImage\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
        "Target=\"media/cell.png\"/>"
        "</Relationships>";
    const std::string image_bytes("\x89PNG\r\n\x1a\ncell-image", 18);
    appendMembers(
        path,
        {{"word/_rels/document.xml.rels", relationships},
         {"word/media/cell.png", image_bytes}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->paragraphs().size() == 2 &&
              document->bodyBlocks().size() == 2,
          "image-table fixture structure changed on import");
    const auto* unsupported = std::get_if<ImportedUnsupportedBodyBlock>(
        &document->bodyBlocks()[0]);
    check(unsupported != nullptr && unsupported->element_name == "w:tbl" &&
              unsupported->reason.find("inline images inside table cells") !=
                  std::string::npos &&
              unsupported->fallback_paragraph_indices ==
                  std::vector<std::size_t>{0},
          "table with an inline image cell was exposed as semantically editable");
    check(document->compatibility().classification ==
              CompatibilityClass::safe_text_patch &&
              std::any_of(
                  document->compatibility().issues.begin(),
                  document->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.code == IssueCode::unsupported_body_content &&
                             issue.detail.find(
                                 "inline images inside table cells") !=
                                 std::string::npos;
                  }),
          "image-bearing table was not reported as preserved view-only");

    const auto& image_fragment =
        document->paragraphs()[0].runs[0].fragments[0];
    check(image_fragment.kind == FragmentKind::inline_image &&
              image_fragment.inline_image &&
              image_fragment.inline_image->bytes.size() == image_bytes.size(),
          "view-only cell image payload was not retained");
    const auto& tail_fragment =
        document->paragraphs()[1].runs[0].fragments[0];
    check(tail_fragment.text_span_id.has_value(),
          "text outside the view-only table is not patchable");
    const auto original_table = extractElement(
        readMember(path, "word/document.xml"), "<w:tbl>", "</w:tbl>");
    const auto edit = document->replaceText(
        *tail_fragment.text_span_id, "Edited outside table");
    check(edit.accepted,
          edit.error ? edit.error->message
                     : "unrelated text edit was refused");
    const auto save = document->saveAs(output);
    check(save.saved,
          save.error ? save.error->message
                     : "image-table preservation save failed");
    check(extractElement(
              readMember(output, "word/document.xml"),
              "<w:tbl>", "</w:tbl>") == original_table &&
              readMember(output, "word/_rels/document.xml.rels") ==
                  relationships &&
              readMember(output, "word/media/cell.png") == image_bytes,
          "unrelated text edit changed the view-only image table or media");

    error = {};
    auto reopened = DocxDocument::open(output, &error);
    check(reopened != nullptr, error.message);
    check(std::holds_alternative<ImportedUnsupportedBodyBlock>(
              reopened->bodyBlocks()[0]) &&
              reopened->paragraphs()[1].plainText() ==
                  "Edited outside table",
          "image table lost view-only status or unrelated edit on reopen");
}

void testRepeatedImageReferencesShareBoundedStorage(
    const TemporaryDirectory& temporary) {
    constexpr std::size_t reference_count = 4'100U;
    const auto path = temporary.file("repeated-image-references.docx");
    std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<w:body>";
    constexpr std::string_view drawing =
        "<w:p><w:r><w:drawing><wp:inline>"
        "<wp:extent cx=\"914400\" cy=\"914400\"/>"
        "<wp:docPr id=\"1\" name=\"Shared\"/>"
        "<a:graphic><a:graphicData><a:blip r:embed=\"rIdShared\"/>"
        "</a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p>";
    document_xml.reserve(document_xml.size() +
                         drawing.size() * reference_count + 64U);
    for (std::size_t index = 0; index < reference_count; ++index) {
        document_xml.append(drawing);
    }
    document_xml.append("<w:sectPr/></w:body></w:document>");
    createPackage(path, document_xml);
    static const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rIdShared\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
        "Target=\"media/shared.png\"/>"
        "</Relationships>";
    const std::string image_bytes("\x89PNG\r\n\x1a\nshared", 14);
    appendMembers(
        path,
        {{"word/_rels/document.xml.rels", relationships},
         {"word/media/shared.png", image_bytes}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->paragraphs().size() == reference_count,
          "repeated-image fixture paragraph count differs");
    const auto image_at = [&document](std::size_t index)
        -> const docxstudio::ooxml::InlineImagePayload& {
        const auto& fragment =
            document->paragraphs()[index].runs[0].fragments[0];
        check(fragment.inline_image.has_value(),
              "repeated DrawingML image was not parsed");
        return *fragment.inline_image;
    };
    const auto& first = image_at(0U);
    const auto& second = image_at(1U);
    const auto& last_resolved = image_at(4'095U);
    const auto& first_beyond_limit = image_at(4'096U);
    check(first.bytes.size() == image_bytes.size() &&
              first.bytes.sharesStorageWith(second.bytes) &&
              first.bytes.sharesStorageWith(last_resolved.bytes),
          "repeated image references copied their package-member bytes");
    check(first_beyond_limit.bytes.empty() &&
              first_beyond_limit.package_member.empty(),
          "OOXML image-reference resolution limit was not enforced");
}

void testStylesThemesAndNativeNumberingImport(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("styles-numbering.docx");
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"HeadingSample\"/>"
        "<w:spacing w:after=\"40\"/><w:rPr><w:i w:val=\"0\"/>"
        "</w:rPr></w:pPr>"
        "<w:r><w:rPr><w:rStyle w:val=\"EmphasisSample\"/></w:rPr>"
        "<w:t>Styled</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"5\"/></w:numPr></w:pPr>"
        "<w:r><w:t>First</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"5\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Second</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/>"
        "<w:numId w:val=\"5\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Nested</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(path, document_xml);

    const std::string theme_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<a:themeElements><a:clrScheme name=\"Synthetic\">"
        "<a:dk1><a:srgbClr val=\"000000\"/></a:dk1>"
        "<a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1>"
        "<a:accent1><a:srgbClr val=\"336699\"/></a:accent1>"
        "</a:clrScheme><a:fontScheme name=\"Synthetic\">"
        "<a:majorFont><a:latin typeface=\"Theme Serif\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Theme Sans\"/></a:minorFont>"
        "</a:fontScheme></a:themeElements></a:theme>";
    const std::string styles_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:asciiTheme=\"minorHAnsi\" w:hAnsiTheme=\"minorHAnsi\"/>"
        "<w:sz w:val=\"20\"/></w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:after=\"100\"/>"
        "</w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:rPr><w:b w:val=\"0\"/></w:rPr>"
        "</w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"HeadingSample\">"
        "<w:name w:val=\"Heading Sample\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:jc w:val=\"center\"/><w:keepNext/>"
        "<w:rPr><w:i/></w:rPr></w:pPr>"
        "<w:rPr><w:rFonts w:ascii=\"Fallback Serif\" "
        "w:hAnsi=\"Fallback Serif\" w:asciiTheme=\"majorHAnsi\" "
        "w:hAnsiTheme=\"majorHAnsi\"/><w:b/>"
        "<w:color w:val=\"112233\" w:themeColor=\"accent1\"/>"
        "</w:rPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"EmphasisSample\">"
        "<w:name w:val=\"Emphasis Sample\"/><w:rPr><w:i/></w:rPr></w:style>"
        "</w:styles>";
    const std::string numbering_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"7\">"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/>"
        "<w:numFmt w:val=\"decimal\"/><w:suff w:val=\"tab\"/>"
        "<w:lvlText w:val=\"%1.\"/><w:pPr><w:tabs>"
        "<w:tab w:val=\"num\" w:pos=\"720\"/></w:tabs>"
        "<w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/>"
        "<w:numFmt w:val=\"lowerLetter\"/>"
        "<w:lvlText w:val=\"%1.%2)\"/><w:pPr><w:tabs>"
        "<w:tab w:val=\"num\" w:pos=\"1080\"/></w:tabs>"
        "<w:ind w:left=\"1080\" w:hanging=\"360\"/></w:pPr></w:lvl>"
        "</w:abstractNum><w:num w:numId=\"5\">"
        "<w:abstractNumId w:val=\"7\"/></w:num></w:numbering>";
    const std::string document_relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rStyles\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
        "Target=\"styles-main.xml\"/>"
        "<Relationship Id=\"rNumbering\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" "
        "Target=\"lists/main.xml\"/>"
        "<Relationship Id=\"rTheme\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" "
        "Target=\"themes/main.xml\"/>"
        "</Relationships>";
    appendMembers(
        path,
        {{"word/_rels/document.xml.rels", document_relationships},
         {"word/themes/main.xml", theme_xml},
         {"word/styles-main.xml", styles_xml},
         {"word/lists/main.xml", numbering_xml}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr,
          error.message.empty() ? "style/numbering fixture did not open"
                                : error.message);
    check(document->paragraphs().size() == 4,
          "style/numbering fixture paragraph count changed");

    const auto& styled = document->paragraphs()[0];
    check(styled.style_id == "HeadingSample" &&
              styled.alignment == BasicParagraphAlignment::center &&
              styled.keep_with_next == true &&
              styled.space_after_twips == 40,
          "paragraph style cascade or direct formatting precedence failed");
    check(styled.runs.size() == 1,
          "styled paragraph run count changed");
    check(styled.runs[0].style_id == "EmphasisSample",
          "character style identity was not retained");
    check(styled.runs[0].format.font_family == "Theme Serif",
          "theme major font was not resolved through paragraph style");
    check(styled.runs[0].format.font_size_half_points == 20,
          "docDefaults font size did not cascade into styled run");
    check(styled.runs[0].format.bold == true,
          "paragraph style bold did not cascade into styled run");
    check(styled.runs[0].format.italic == true,
          "character style italic did not cascade into styled run");
    check(styled.runs[0].format.foreground_rgb == 0x00336699U,
          "theme color did not override its fallback value");
    check(styled.style_provenance &&
              styled.style_provenance->inherited_character_format
                      .foreground_rgb == 0x00336699U &&
              styled.style_provenance->inherited_character_format.italic ==
                  std::nullopt &&
              styled.style_provenance->inherited_paragraph_mark_format
                      .italic == true &&
              styled.style_provenance->inherited_alignment ==
                  BasicParagraphAlignment::center &&
              styled.style_provenance->direct_space_after_twips == 40 &&
              styled.style_provenance->direct_paragraph_mark_format.italic ==
                  false &&
              styled.paragraph_mark_format &&
              styled.paragraph_mark_format->italic == false &&
              styled.runs[0].paragraph_style_overrides.italic == true,
          "style import did not separate the resolved baseline from direct/character-style overrides");

    const auto& first = document->paragraphs()[1];
    const auto& second = document->paragraphs()[2];
    const auto& nested = document->paragraphs()[3];
    check(first.numbering && second.numbering && nested.numbering,
          "native numPr references were not resolved");
    check(first.numbering->marker_text == "3." &&
              second.numbering->marker_text == "4." &&
              nested.numbering->marker_text == "4.a)",
          "native numbering counters or level template expansion failed");
    check(first.left_indent_twips == 720 &&
              first.first_line_indent_twips == -360 &&
              first.left_tab_stops_twips == std::vector<std::uint32_t>{720} &&
              nested.left_indent_twips == 1080,
          "numbering-level indentation was not applied");
    check(first.runs[0].format.font_family == "Theme Sans" &&
              first.runs[0].format.font_size_half_points == 20 &&
              first.runs[0].format.bold == false,
          "document defaults and explicit false style values were not inherited");

    const auto cyclic_path = temporary.file("cyclic-style.docx");
    const std::string cyclic_document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body><w:p><w:pPr><w:pStyle w:val=\"CycleA\"/></w:pPr>"
        "<w:r><w:t>Cycle</w:t></w:r></w:p><w:sectPr/></w:body></w:document>";
    createPackage(cyclic_path, cyclic_document);
    const std::string cyclic_styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:style w:type=\"paragraph\" w:styleId=\"CycleA\">"
        "<w:basedOn w:val=\"CycleB\"/><w:rPr><w:color w:val=\"336699\"/>"
        "</w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"CycleB\">"
        "<w:basedOn w:val=\"CycleA\"/></w:style></w:styles>";
    appendMembers(
        cyclic_path,
        {{"word/_rels/document.xml.rels", document_relationships},
         {"word/styles-main.xml", cyclic_styles}});
    auto cyclic = DocxDocument::open(cyclic_path, &error);
    check(cyclic && cyclic->paragraphs().size() == 1 &&
              cyclic->paragraphs()[0].style_id == "CycleA" &&
              !cyclic->paragraphs()[0].style_provenance &&
              !cyclic->paragraphs()[0].format_is_basic,
          "cyclic paragraph style produced unsafe partial provenance");

    const auto deep_path = temporary.file("deep-style-chain.docx");
    const std::string deep_document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"P69\"/></w:pPr>"
        "<w:r><w:t>Deep paragraph</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>"
        "<w:r><w:rPr><w:rStyle w:val=\"C69\"/></w:rPr>"
        "<w:t>Deep character</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"P62\"/></w:pPr>"
        "<w:r><w:t>Bounded paragraph</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>"
        "<w:r><w:rPr><w:rStyle w:val=\"C63\"/></w:rPr>"
        "<w:t>Bounded character</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"MissingP\"/></w:pPr>"
        "<w:r><w:t>Missing paragraph</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>"
        "<w:r><w:rPr><w:rStyle w:val=\"MissingC\"/></w:rPr>"
        "<w:t>Missing character</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>"
        "<w:r><w:rPr><w:rStyle w:val=\"CycleC1\"/></w:rPr>"
        "<w:t>Cyclic character</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(deep_path, deep_document);

    std::string deep_styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:rPr><w:sz w:val=\"22\"/></w:rPr></w:style>";
    for (std::size_t index = 0; index < 70; ++index) {
        deep_styles +=
            "<w:style w:type=\"paragraph\" w:styleId=\"P" +
            std::to_string(index) + "\"><w:basedOn w:val=\"" +
            (index == 0 ? std::string{"Normal"}
                        : "P" + std::to_string(index - 1)) +
            "\"/><w:rPr><w:b/></w:rPr></w:style>";
    }
    for (std::size_t index = 0; index < 70; ++index) {
        deep_styles +=
            "<w:style w:type=\"character\" w:styleId=\"C" +
            std::to_string(index) + "\">" +
            (index == 0
                 ? std::string{}
                 : "<w:basedOn w:val=\"C" +
                       std::to_string(index - 1) + "\"/>") +
            "<w:rPr><w:i/></w:rPr></w:style>";
    }
    deep_styles +=
        "<w:style w:type=\"character\" w:styleId=\"CycleC1\">"
        "<w:basedOn w:val=\"CycleC2\"/></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"CycleC2\">"
        "<w:basedOn w:val=\"CycleC1\"/></w:style></w:styles>";
    appendMembers(
        deep_path,
        {{"word/_rels/document.xml.rels", document_relationships},
         {"word/styles-main.xml", deep_styles}});

    auto deep = DocxDocument::open(deep_path, &error);
    check(deep && deep->paragraphs().size() == 7,
          "bounded style-chain fixture did not open");
    check(deep->paragraphs()[0].style_id == "P69" &&
              !deep->paragraphs()[0].style_provenance &&
              !deep->paragraphs()[0].format_is_basic,
          "over-depth paragraph basedOn chain produced supported provenance");
    check(deep->paragraphs()[1].runs.size() == 1 &&
              deep->paragraphs()[1].runs[0].style_id == "C69" &&
              !deep->paragraphs()[1].runs[0].format_is_basic,
          "over-depth character basedOn chain was treated as supported");
    check(deep->paragraphs()[2].style_id == "P62" &&
              deep->paragraphs()[2].style_provenance &&
              deep->paragraphs()[2].format_is_basic &&
              deep->paragraphs()[3].runs[0].style_id == "C63" &&
              deep->paragraphs()[3].runs[0].format_is_basic,
          "a style chain within the explicit depth bound was rejected");
    check(deep->paragraphs()[4].style_id == "MissingP" &&
              !deep->paragraphs()[4].style_provenance &&
              !deep->paragraphs()[4].format_is_basic &&
              deep->paragraphs()[5].runs[0].style_id == "MissingC" &&
              !deep->paragraphs()[5].runs[0].format_is_basic &&
              deep->paragraphs()[6].runs[0].style_id == "CycleC1" &&
              !deep->paragraphs()[6].runs[0].format_is_basic,
          "missing or cyclic basedOn targets were treated as supported");
    check(deep->compatibility().classification ==
              CompatibilityClass::safe_text_patch,
          "unsupported style chains did not conservatively block structural rewrite");
    check(std::any_of(
              deep->compatibility().issues.begin(),
              deep->compatibility().issues.end(),
              [](const auto& issue) {
                  return issue.code == IssueCode::unsupported_formatting;
              }),
          "bounded/missing/cyclic style failures lacked a compatibility warning");
}

void testNumberingLevelJustificationIsConservative(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("numbering-level-justification.docx");
    const auto copy = temporary.file("numbering-level-justification-copy.docx");
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"5\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Left marker</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"6\"/></w:numPr></w:pPr>"
        "<w:r><w:t>Right marker</w:t></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";
    createPackage(path, document_xml);
    const std::string numbering_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"7\"><w:lvl w:ilvl=\"0\">"
        "<w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
        "<w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"left\"/>"
        "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
        "</w:lvl></w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"8\"><w:lvl w:ilvl=\"0\">"
        "<w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
        "<w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"right\"/>"
        "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
        "</w:lvl></w:abstractNum>"
        "<w:num w:numId=\"5\"><w:abstractNumId w:val=\"7\"/></w:num>"
        "<w:num w:numId=\"6\"><w:abstractNumId w:val=\"8\"/></w:num>"
        "</w:numbering>";
    const std::string relationships =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rNumbering\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" "
        "Target=\"numbering.xml\"/>"
        "</Relationships>";
    appendMembers(
        path,
        {{"word/_rels/document.xml.rels", relationships},
         {"word/numbering.xml", numbering_xml}});

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->paragraphs().size() == 2,
          "numbering-justification fixture paragraph count changed");
    const auto& left = document->paragraphs()[0];
    const auto& right = document->paragraphs()[1];
    check(left.numbering && left.numbering->marker_text == "1." &&
              left.format_is_basic,
          "supported left-aligned list marker was not parsed");
    check(!right.numbering && right.numbering_id == 6 &&
              !right.format_is_basic,
          "right-aligned list marker was silently treated as left-aligned");
    check(document->compatibility().classification ==
              CompatibilityClass::safe_text_patch &&
              std::any_of(
                  document->compatibility().issues.begin(),
                  document->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.code == IssueCode::unsupported_formatting &&
                             issue.paragraph_index == 1;
                  }),
          "unsupported list-level justification was not reported conservatively");

    const auto original = readBytes(path);
    const auto save = document->saveAs(copy);
    check(save.saved && save.byte_identical_to_opened_file &&
              readBytes(copy) == original,
          "unsupported list-level justification was not preserved exactly");
}

void testNativeNumberingWriting(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("native-numbering.docx");

    docxstudio::ooxml::NewNumbering topLevel;
    topLevel.num_id = 11;
    topLevel.level = 0;
    topLevel.format = docxstudio::ooxml::BasicNumberFormat::decimal;
    topLevel.start = 3;
    topLevel.level_text = "%1)";
    topLevel.suffix = docxstudio::ooxml::BasicNumberSuffix::tab;
    topLevel.text_indent_twips = 720;
    topLevel.hanging_indent_twips = 360;
    topLevel.tab_stop_twips = 720;

    NewParagraph first{{NewRun{"First body", {}}}};
    first.numbering = topLevel;
    NewParagraph second{{NewRun{"Second body", {}}}};
    second.numbering = topLevel;

    auto nestedLevel = topLevel;
    nestedLevel.level = 1;
    nestedLevel.format =
        docxstudio::ooxml::BasicNumberFormat::lower_letter;
    nestedLevel.start = 1;
    nestedLevel.level_text = "%1.%2)";
    nestedLevel.text_indent_twips = 1080;
    nestedLevel.tab_stop_twips = 1080;
    NewParagraph nested{{NewRun{"Nested body", {}}}};
    nested.numbering = nestedLevel;

    const auto save = DocxDocument::writeNew(
        path, NewDocumentBody{{first, second, nested}});
    check(save.saved,
          save.error ? save.error->message
                     : "native-numbering DOCX was not saved");

    const std::string contentTypes = readMember(path, "[Content_Types].xml");
    const std::string relationships =
        readMember(path, "word/_rels/document.xml.rels");
    const std::string documentXml = readMember(path, "word/document.xml");
    const std::string numberingXml = readMember(path, "word/numbering.xml");
    checkAuthoredNumberingLevelOrder(numberingXml, 2);
    check(contentTypes.find("/word/numbering.xml") != std::string::npos &&
              relationships.find(
                  "relationships/numbering\" Target=\"numbering.xml\"") !=
                  std::string::npos,
          "native numbering package metadata was not emitted");
    check(countOccurrences(documentXml, "<w:numPr>") == 3 &&
              countOccurrences(documentXml, "<w:numId w:val=\"11\"/>") == 3 &&
              documentXml.find("<w:tab/>") == std::string::npos &&
              documentXml.find(">3)</w:t>") == std::string::npos &&
              documentXml.find(">First body</w:t>") != std::string::npos,
          "document.xml duplicated native markers or omitted numPr");
    check(countOccurrences(numberingXml, "<w:lvl ") == 2 &&
              numberingXml.find("<w:start w:val=\"3\"/>") !=
                  std::string::npos &&
              numberingXml.find("<w:numFmt w:val=\"decimal\"/>") !=
                  std::string::npos &&
              numberingXml.find("<w:numFmt w:val=\"lowerLetter\"/>") !=
                  std::string::npos &&
              numberingXml.find("<w:lvlText w:val=\"%1.%2)\"/>") !=
                  std::string::npos &&
              numberingXml.find("<w:tab w:val=\"num\" w:pos=\"1080\"/>") !=
                  std::string::npos,
          "numbering.xml lost formats, templates, starts, or geometry");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 3 &&
              reopened->paragraphs()[0].plainText() == "First body" &&
              reopened->paragraphs()[1].plainText() == "Second body" &&
              reopened->paragraphs()[2].plainText() == "Nested body",
          "native-numbering body text changed on reopen");
    check(reopened->paragraphs()[0].numbering &&
              reopened->paragraphs()[1].numbering &&
              reopened->paragraphs()[2].numbering &&
              reopened->paragraphs()[0].numbering->marker_text == "3)" &&
              reopened->paragraphs()[1].numbering->marker_text == "4)" &&
              reopened->paragraphs()[2].numbering->marker_text == "4.a)" &&
              reopened->paragraphs()[2].numbering->level == 1 &&
              reopened->paragraphs()[2].left_indent_twips == 1080 &&
              reopened->paragraphs()[2].first_line_indent_twips == -360,
          "native numbering semantics did not survive write/reopen");

    NewParagraph conflict{{NewRun{"Conflict", {}}}};
    auto conflictingDefinition = topLevel;
    conflictingDefinition.start = 9;
    conflict.numbering = conflictingDefinition;
    const auto rejected = DocxDocument::writeNew(
        temporary.file("conflicting-native-numbering.docx"),
        NewDocumentBody{{first, conflict}});
    check(!rejected.saved && rejected.loss_report.hasBlockers(),
          "conflicting definitions for one native list level were accepted");

    NewParagraph invalidTenth{{NewRun{"Invalid native level", {}}}};
    auto tenthLevel = topLevel;
    tenthLevel.level = 9;
    tenthLevel.level_text = "%10.";
    invalidTenth.numbering = tenthLevel;
    const auto invalidTenthSave = DocxDocument::writeNew(
        temporary.file("invalid-tenth-native-level.docx"),
        NewDocumentBody{{invalidTenth}});
    check(!invalidTenthSave.saved &&
              invalidTenthSave.loss_report.hasBlockers(),
          "an out-of-schema tenth native numbering level was accepted");
}

void testUnsafeImageRelationshipsAreNotLoaded(
    const TemporaryDirectory& temporary) {
    const std::string document_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
        "<w:body><w:p><w:r><w:drawing><wp:inline>"
        "<wp:extent cx=\"914400\" cy=\"457200\"/>"
        "<a:graphic><a:graphicData><a:blip r:embed=\"rIdImage\"/>"
        "</a:graphicData></a:graphic>"
        "</wp:inline></w:drawing></w:r></w:p>"
        "<w:sectPr/></w:body></w:document>";

    struct UnsafeRelationshipCase {
        std::string file_name;
        std::string target;
        std::string target_mode_attribute;
        std::string package_member;
        std::string bytes;
    };
    const std::vector<UnsafeRelationshipCase> cases{
        {"external-image.docx",
         "media/external.png",
         " TargetMode=\"External\"",
         "word/media/external.png",
         "external bytes must stay unread"},
        {"traversal-image.docx",
         "../media/traversal.png",
         "",
         "media/traversal.png",
         "traversal bytes must stay unread"}};

    for (const auto& test_case : cases) {
        const auto path = temporary.file(test_case.file_name);
        createPackage(path, document_xml);
        const std::string relationships =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rIdImage\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
            "Target=\"" + test_case.target + "\"" +
            test_case.target_mode_attribute + "/>"
            "</Relationships>";
        appendMembers(path, {
            {"word/_rels/document.xml.rels", relationships},
            {test_case.package_member, test_case.bytes}});

        Error error;
        auto document = DocxDocument::open(path, &error);
        check(document != nullptr, error.message);
        check(document->paragraphs().size() == 1 &&
                  document->paragraphs()[0].runs.size() == 1 &&
                  document->paragraphs()[0].runs[0].fragments.size() == 1,
              "unsafe image relationship changed the document structure");
        const auto& fragment =
            document->paragraphs()[0].runs[0].fragments[0];
        check(fragment.kind == FragmentKind::inline_image &&
                  fragment.inline_image.has_value(),
              "unsafe relationship removed the inert inline image placeholder");
        const auto& image = *fragment.inline_image;
        check(image.relationship_id == "rIdImage" &&
                  image.package_member.empty() &&
                  image.content_type.empty() && image.bytes.empty() &&
                  !image.renderable(),
              "external or traversal image relationship loaded package bytes");
    }
}

void testNewDocumentCreation(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("new.docx");
    NewRun title{"Hello & <world>", {}};
    title.format.bold = true;
    title.format.font_family = "Carlito";
    title.format.font_size_half_points = 28;
    title.format.foreground_rgb = 0x13579bU;
    title.format.highlight_rgb = 0xffee88U;
    title.format.baseline = docxstudio::ooxml::BasicBaseline::superscript;
    NewRun spaced{" second ", {}};

    docxstudio::ooxml::PageSettings page;
    page.width_twips = 15840;
    page.height_twips = 12240;
    page.margin_top_twips = 720;
    page.margin_right_twips = 900;
    page.margin_bottom_twips = 720;
    page.margin_left_twips = 900;
    NewParagraph title_paragraph{{title}, BasicParagraphAlignment::center};
    title_paragraph.left_indent_twips = 720;
    title_paragraph.right_indent_twips = 360;
    title_paragraph.first_line_indent_twips = -360;
    title_paragraph.space_before_twips = 120;
    title_paragraph.space_after_twips = 240;
    title_paragraph.line_spacing = 360;
    title_paragraph.line_spacing_rule =
        docxstudio::ooxml::BasicLineSpacingRule::automatic;
    title_paragraph.keep_with_next = true;
    title_paragraph.keep_lines = false;
    title_paragraph.page_break_before = true;
    title_paragraph.left_tab_stops_twips = {960, 1440};
    const auto save = DocxDocument::writeNew(
        path,
        {title_paragraph, NewParagraph{{spaced}, std::nullopt}},
        page);
    check(save.saved, save.error.has_value() ? save.error->message : "new DOCX was not saved");

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(document->packageMembers().size() == 6, "minimal DOCX should contain six parts");
    check(document->paragraphs().size() == 2, "new DOCX paragraph count differs");
    check(document->paragraphs()[0].plainText() == "Hello & <world>", "new DOCX text differs");
    check(document->paragraphs()[1].plainText() == " second ", "xml:space text differs");
    check(
        document->paragraphs()[0].alignment == BasicParagraphAlignment::center,
        "paragraph alignment was not parsed");
    const auto& parsed_paragraph = document->paragraphs()[0];
    check(parsed_paragraph.left_indent_twips == 720, "left indent was not parsed");
    check(parsed_paragraph.right_indent_twips == 360, "right indent was not parsed");
    check(parsed_paragraph.first_line_indent_twips == -360,
          "hanging indent was not parsed");
    check(parsed_paragraph.space_before_twips == 120,
          "space-before was not parsed");
    check(parsed_paragraph.space_after_twips == 240,
          "space-after was not parsed");
    check(parsed_paragraph.line_spacing == 360,
          "line spacing was not parsed");
    check(parsed_paragraph.line_spacing_rule ==
              docxstudio::ooxml::BasicLineSpacingRule::automatic,
          "line spacing rule was not parsed");
    check(parsed_paragraph.keep_with_next == true, "keep-next was not parsed");
    check(parsed_paragraph.keep_lines == false, "explicit keep-lines false was not parsed");
    check(parsed_paragraph.page_break_before == true,
          "page-break-before was not parsed");
    check(parsed_paragraph.left_tab_stops_twips ==
              std::vector<std::uint32_t>{960, 1440},
          "left paragraph tab stops were not parsed");
    const auto& parsed_format = document->paragraphs()[0].runs[0].format;
    check(parsed_format.bold.value_or(false),
          "bold run formatting was not parsed");
    check(parsed_format.font_family == "Carlito", "font family was not parsed");
    check(parsed_format.font_size_half_points == 28, "font size was not parsed");
    check(parsed_format.foreground_rgb == 0x13579bU, "foreground color was not parsed");
    check(parsed_format.highlight_rgb == 0xffee88U, "highlight color was not parsed");
    check(parsed_format.baseline == docxstudio::ooxml::BasicBaseline::superscript,
          "superscript was not parsed");
    check(document->bodyPageSettings() == page, "body page settings were not parsed");
    const std::string xml = readMember(path, "word/document.xml");
    check(xml.find(
              "<w:pgSz w:w=\"15840\" w:h=\"12240\" w:orient=\"landscape\"/>") !=
              std::string::npos,
          "page size or landscape orientation was not serialized");
    check(xml.find("w:top=\"720\" w:right=\"900\" w:bottom=\"720\" w:left=\"900\"") !=
              std::string::npos,
          "page margins were not serialized");
    check(xml.find("<w:ind w:left=\"720\" w:right=\"360\" w:hanging=\"360\"/>") !=
              std::string::npos,
          "paragraph indents were not serialized");
    check(xml.find("<w:spacing w:before=\"120\" w:after=\"240\" w:line=\"360\" "
                   "w:lineRule=\"auto\"/>") != std::string::npos,
          "paragraph spacing was not serialized");
    check(xml.find("<w:tabs><w:tab w:val=\"left\" w:pos=\"960\"/>"
                   "<w:tab w:val=\"left\" w:pos=\"1440\"/></w:tabs>") !=
              std::string::npos,
          "paragraph tab stops were not serialized");
    check(
        document->compatibility().classification == CompatibilityClass::basic_body_text_patch,
        "minimal DOCX should be classified as basic body text patch safe");
}

void testNewDocumentDefaultStyle(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("default-style.docx");
    const auto save = DocxDocument::writeNew(
        path, {NewParagraph{{NewRun{"Plain text", {}}}}});
    check(save.saved,
          save.error.has_value() ? save.error->message : "plain DOCX was not saved");

    const std::string content_types = readMember(path, "[Content_Types].xml");
    check(
        content_types.find(
            "<Override PartName=\"/word/styles.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.styles+xml\"/>") != std::string::npos,
        "styles.xml content-type override is missing or incorrect");
    check(
        content_types.find(
            "<Override PartName=\"/word/settings.xml\" "
            "ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.settings+xml\"/>") != std::string::npos,
        "settings.xml content-type override is missing or incorrect");

    const std::string relationships =
        readMember(path, "word/_rels/document.xml.rels");
    check(
        relationships.find(
            "<Relationship Id=\"rId1\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
            "Target=\"styles.xml\"/>") != std::string::npos,
        "document-to-styles relationship is missing or incorrect");
    check(
        relationships.find(
            "<Relationship Id=\"rId2\" "
            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" "
            "Target=\"settings.xml\"/>") != std::string::npos,
        "document-to-settings relationship is missing or incorrect");

    const std::string styles = readMember(path, "word/styles.xml");
    const std::string legacy_styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Carlito\" w:hAnsi=\"Carlito\" w:cs=\"Carlito\"/>"
        "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
        "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr>"
        "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
        "</w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:qFormat/><w:pPr>"
        "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
        "</w:pPr><w:rPr>"
        "<w:rFonts w:ascii=\"Carlito\" w:hAnsi=\"Carlito\" w:cs=\"Carlito\"/>"
        "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
        "</w:rPr></w:style></w:styles>";
    check(styles == legacy_styles,
          "unstyled authoring changed the legacy Normal-only styles.xml bytes");
    const std::string run_defaults =
        "<w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Carlito\" w:hAnsi=\"Carlito\" w:cs=\"Carlito\"/>"
        "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
        "</w:rPr></w:rPrDefault>";
    const std::string paragraph_defaults =
        "<w:pPrDefault><w:pPr>"
        "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>"
        "</w:pPr></w:pPrDefault>";
    check(styles.find(run_defaults) != std::string::npos,
          "Carlito 11 document run defaults are missing");
    check(styles.find(paragraph_defaults) != std::string::npos,
          "single-spaced document paragraph defaults are missing");
    check(
        styles.find(
            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
            "<w:name w:val=\"Normal\"/><w:qFormat/>") != std::string::npos,
        "default Normal paragraph style is missing");
    check(countOccurrences(styles, "w:ascii=\"Carlito\"") == 2 &&
              countOccurrences(styles, "<w:sz w:val=\"22\"/>") == 2 &&
              countOccurrences(styles, "w:after=\"0\" w:line=\"240\"") == 2,
          "Normal style does not repeat the declared document defaults");
    const std::string settings = readMember(path, "word/settings.xml");
    check(settings.find("<w:defaultTabStop w:val=\"240\"/>") !=
              std::string::npos,
          "four-space default tab stop is missing from settings.xml");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 1 &&
              reopened->paragraphs()[0].plainText() == "Plain text",
          "plain DOCX did not reopen with its body text intact");
    check(reopened->packageMembers().size() == 6,
          "reopened plain DOCX did not retain its style/settings parts");
}

void testNativeParagraphStyleWriting(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("native-paragraph-styles.docx");
    NewParagraph title{{NewRun{"A styled title", {}}}};
    title.style_id = "Title";
    NewParagraph heading{{NewRun{"A second-level heading", {}}}};
    heading.style_id = "Heading2";
    heading.space_after_twips = 77;
    NewParagraph deep_heading{{NewRun{"A deep heading", {}}}};
    deep_heading.style_id = "Heading9";
    NewParagraph no_spacing{{NewRun{"Compact body", {}}}};
    no_spacing.style_id = "NoSpacing";
    NewParagraph explicit_normal{{NewRun{"Normal body", {}}}};
    explicit_normal.style_id = "Normal";

    const auto save = DocxDocument::writeNew(
        path, {title, heading, deep_heading, no_spacing, explicit_normal});
    check(save.saved,
          save.error ? save.error->message
                     : "native paragraph-style DOCX was not saved");

    const std::string document_xml =
        readMember(path, "word/document.xml");
    check(document_xml.find(
              "<w:pPr><w:pStyle w:val=\"Heading2\"/>"
              "<w:spacing w:after=\"77\"/>") != std::string::npos,
          "w:pStyle was not emitted first in CT_PPr schema order");
    check(countOccurrences(document_xml, "<w:pStyle ") == 5,
          "authored paragraph style references were omitted or duplicated");

    const std::string styles_xml = readMember(path, "word/styles.xml");
    check(countOccurrences(styles_xml, "<w:style ") == 5,
          "styles.xml did not contain exactly Normal plus the used styles");
    check(styles_xml.find(
              "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
              "<w:name w:val=\"Heading 2\"/>"
              "<w:basedOn w:val=\"Normal\"/>"
              "<w:next w:val=\"Normal\"/>"
              "<w:uiPriority w:val=\"9\"/><w:qFormat/><w:pPr>"
              "<w:keepNext/><w:keepLines/>"
              "<w:spacing w:before=\"200\" w:after=\"80\" "
              "w:line=\"240\" w:lineRule=\"auto\"/>"
              "<w:jc w:val=\"left\"/>"
              "<w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr>"
              "<w:b/><w:color w:val=\"77216F\"/>"
              "<w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/>"
              "</w:rPr></w:style>") != std::string::npos,
          "Heading2 definition lost metadata, outline, or formatting");
    check(styles_xml.find("<w:outlineLvl w:val=\"8\"/>") !=
              std::string::npos,
          "Heading9 definition did not carry its outline level");
    check(styles_xml.find("w:styleId=\"Title\"") != std::string::npos &&
              styles_xml.find("w:styleId=\"NoSpacing\"") !=
                  std::string::npos &&
              styles_xml.find("w:styleId=\"Subtitle\"") ==
                  std::string::npos &&
              styles_xml.find("w:styleId=\"Quote\"") ==
                  std::string::npos &&
              styles_xml.find("w:styleId=\"Heading1\"") ==
                  std::string::npos &&
              styles_xml.find("w:styleId=\"Heading3\"") ==
                  std::string::npos,
          "styles.xml omitted a used style or fabricated an unused one");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 5 &&
              reopened->paragraphs()[0].style_id == "Title" &&
              reopened->paragraphs()[1].style_id == "Heading2" &&
              reopened->paragraphs()[2].style_id == "Heading9" &&
              reopened->paragraphs()[3].style_id == "NoSpacing" &&
              reopened->paragraphs()[4].style_id == "Normal",
          "native paragraph style IDs did not survive write/reopen");
    check(reopened->paragraphs()[0].alignment ==
                  BasicParagraphAlignment::left &&
              reopened->paragraphs()[0].keep_with_next == true &&
              reopened->paragraphs()[0].runs[0].format.bold == true &&
              reopened->paragraphs()[0].runs[0].format.font_size_half_points ==
                  56 &&
              reopened->paragraphs()[1].space_after_twips == 77 &&
              reopened->paragraphs()[1].runs[0].format.foreground_rgb ==
                  0x0077216fU,
          "built-in style formatting or direct-format precedence changed on reopen");
    check(reopened->compatibility().classification ==
                  CompatibilityClass::basic_body_text_patch &&
              reopened->compatibility().issues.empty() &&
              reopened->isCanonicalRegeneratableSimplePackage(
                  DocumentDefaults{}),
          "styled current-writer package was not safely regeneratable");

    const auto reordered_path =
        temporary.file("native-paragraph-style-reordered.docx");
    check(std::filesystem::copy_file(path, reordered_path),
          "could not copy styled canonical fixture");
    std::string reordered_xml =
        readMember(reordered_path, "word/document.xml");
    const std::string canonical_properties =
        "<w:pStyle w:val=\"Heading2\"/>"
        "<w:spacing w:after=\"77\"/>";
    const auto property_position = reordered_xml.find(canonical_properties);
    check(property_position != std::string::npos,
          "styled canonical fixture properties are missing");
    reordered_xml.replace(
        property_position, canonical_properties.size(),
        "<w:spacing w:after=\"77\"/>"
        "<w:pStyle w:val=\"Heading2\"/>");
    replaceMember(reordered_path, "word/document.xml", reordered_xml);
    error = {};
    auto reordered = DocxDocument::open(reordered_path, &error);
    check(reordered != nullptr, error.message);
    check(!reordered->isCanonicalRegeneratableSimplePackage(
              DocumentDefaults{}),
          "out-of-order pStyle grammar was declared canonical");

    const auto custom_path = temporary.file("imported-custom-style.docx");
    check(std::filesystem::copy_file(path, custom_path),
          "could not copy custom paragraph-style fixture");
    std::string custom_document_xml =
        readMember(custom_path, "word/document.xml");
    const auto title_reference = custom_document_xml.find(
        "w:pStyle w:val=\"Title\"");
    check(title_reference != std::string::npos,
          "custom style fixture Title reference is missing");
    custom_document_xml.replace(
        title_reference, std::string_view("w:pStyle w:val=\"Title\"").size(),
        "w:pStyle w:val=\"FirmLegalBody\"");
    replaceMember(custom_path, "word/document.xml", custom_document_xml);
    std::string custom_styles_xml =
        readMember(custom_path, "word/styles.xml");
    const auto title_definition = custom_styles_xml.find(
        "w:styleId=\"Title\"");
    check(title_definition != std::string::npos,
          "custom style fixture Title definition is missing");
    custom_styles_xml.replace(
        title_definition, std::string_view("w:styleId=\"Title\"").size(),
        "w:styleId=\"FirmLegalBody\"");
    replaceMember(custom_path, "word/styles.xml", custom_styles_xml);
    error = {};
    auto custom = DocxDocument::open(custom_path, &error);
    check(custom != nullptr &&
              custom->paragraphs()[0].style_id == "FirmLegalBody",
          error.message.empty()
              ? "imported custom paragraph style ID was not retained"
              : error.message);
    check(!custom->isCanonicalRegeneratableSimplePackage(
              DocumentDefaults{}),
          "imported custom paragraph style was declared canonical");

    NewParagraph regenerate_custom{{NewRun{"Do not fabricate", {}}}};
    regenerate_custom.style_id = custom->paragraphs()[0].style_id;
    const auto custom_save = DocxDocument::writeNew(
        temporary.file("regenerated-custom-style.docx"),
        {regenerate_custom});
    check(!custom_save.saved && custom_save.error &&
              custom_save.error->code ==
                  docxstudio::ooxml::ErrorCode::unsafe_edit &&
              custom_save.loss_report.hasBlockers() &&
              custom_save.loss_report.issues.front().code ==
                  IssueCode::unsupported_formatting,
          "unknown custom paragraph style was fabricated during regeneration");

    const std::vector<std::string> built_in_ids{
        "Normal", "NoSpacing", "Title", "Subtitle", "Quote",
        "Heading1", "Heading2", "Heading3", "Heading4", "Heading5",
        "Heading6", "Heading7", "Heading8", "Heading9"};
    std::vector<NewParagraph> every_built_in;
    every_built_in.reserve(built_in_ids.size());
    for (const auto& id : built_in_ids) {
        NewParagraph paragraph{{NewRun{id, {}}}};
        paragraph.style_id = id;
        every_built_in.push_back(std::move(paragraph));
    }
    const auto every_style_path =
        temporary.file("every-built-in-paragraph-style.docx");
    const auto every_style_save = DocxDocument::writeNew(
        every_style_path, every_built_in);
    error = {};
    auto every_style = every_style_save.saved
        ? DocxDocument::open(every_style_path, &error)
        : nullptr;
    check(every_style_save.saved && every_style != nullptr &&
              every_style->paragraphs().size() == built_in_ids.size() &&
              countOccurrences(
                  readMember(every_style_path, "word/styles.xml"),
                  "<w:style ") == built_in_ids.size() &&
              every_style->isCanonicalRegeneratableSimplePackage(
                  DocumentDefaults{}),
          every_style_save.error
              ? every_style_save.error->message
              : (error.message.empty()
                     ? "full built-in paragraph style catalog did not round-trip"
                     : error.message));

    const std::vector<std::string> invalid_ids{
        "", "Heading 1", "1Heading", std::string(254, 'A'),
        std::string("\xc3\x28", 2)};
    for (std::size_t index = 0; index < invalid_ids.size(); ++index) {
        NewParagraph invalid{{NewRun{"Rejected style", {}}}};
        invalid.style_id = invalid_ids[index];
        const auto invalid_path = temporary.file(
            "invalid-paragraph-style-" + std::to_string(index) + ".docx");
        const auto invalid_save = DocxDocument::writeNew(
            invalid_path, {invalid});
        check(!invalid_save.saved && invalid_save.error &&
                  invalid_save.error->code ==
                      docxstudio::ooxml::ErrorCode::unsafe_edit &&
                  invalid_save.loss_report.hasBlockers() &&
                  !std::filesystem::exists(invalid_path),
              "invalid paragraph style ID was accepted or left an output file");
    }

    NewTable table;
    table.rows = 1;
    table.columns = 1;
    NewParagraph quote{{NewRun{"Styled cell", {}}}};
    quote.style_id = "Quote";
    table.cell_paragraphs = {quote};
    const auto table_path = temporary.file("table-cell-paragraph-style.docx");
    const auto table_save = DocxDocument::writeNew(
        table_path, NewDocumentBody{{table}});
    check(table_save.saved &&
              readMember(table_path, "word/styles.xml")
                      .find("w:styleId=\"Quote\"") != std::string::npos,
          table_save.error ? table_save.error->message
                           : "table-cell paragraph style was not catalogued");

    docxstudio::ooxml::NewSection first_section;
    NewParagraph subtitle{{NewRun{"Section one", {}}}};
    subtitle.style_id = "Subtitle";
    first_section.body.blocks = {subtitle};
    docxstudio::ooxml::NewSection second_section;
    NewParagraph heading_three{{NewRun{"Section two", {}}}};
    heading_three.style_id = "Heading3";
    second_section.body.blocks = {heading_three};
    docxstudio::ooxml::NewSectionedDocumentBody sections{
        {first_section, second_section}};
    const auto sectioned_path =
        temporary.file("sectioned-paragraph-styles.docx");
    const auto sectioned_save = DocxDocument::writeNew(
        sectioned_path, sections);
    const std::string sectioned_styles = sectioned_save.saved
        ? readMember(sectioned_path, "word/styles.xml")
        : std::string{};
    check(sectioned_save.saved &&
              sectioned_styles.find("w:styleId=\"Subtitle\"") !=
                  std::string::npos &&
              sectioned_styles.find("w:styleId=\"Heading3\"") !=
                  std::string::npos,
          sectioned_save.error
              ? sectioned_save.error->message
              : "sectioned paragraph styles were not catalogued");
}

void testCustomDocumentDefaults(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("custom-defaults.docx");
    DocumentDefaults defaults;
    defaults.font_family = "Noto Sans & Serif";
    defaults.font_size_half_points = 27;
    defaults.default_tab_stop_twips = 333;
    const auto save = DocxDocument::writeNew(
        path, {NewParagraph{{NewRun{"Custom defaults", {}}}}}, {}, defaults);
    check(save.saved,
          save.error ? save.error->message : "custom-default DOCX was not saved");

    const std::string styles = readMember(path, "word/styles.xml");
    check(countOccurrences(styles, "w:ascii=\"Noto Sans &amp; Serif\"") == 2 &&
              countOccurrences(styles, "<w:sz w:val=\"27\"/>") == 2,
          "custom font family or half-point size was not written to document defaults");
    const std::string settings = readMember(path, "word/settings.xml");
    check(settings.find("<w:defaultTabStop w:val=\"333\"/>") !=
              std::string::npos,
          "custom default tab stop was not written to settings.xml");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 1 &&
              reopened->paragraphs()[0].plainText() == "Custom defaults",
          "custom-default DOCX did not reopen with its content intact");

    defaults.font_size_half_points = 0;
    const auto invalid = DocxDocument::writeNew(
        temporary.file("invalid-defaults.docx"),
        {NewParagraph{{NewRun{"Rejected", {}}}}}, {}, defaults);
    check(!invalid.saved && invalid.error.has_value(),
          "invalid document defaults were accepted");

    NewParagraph invalidTabs{{NewRun{"Rejected tabs", {}}}};
    invalidTabs.left_tab_stops_twips = {720, 360};
    const auto invalidTabSave = DocxDocument::writeNew(
        temporary.file("invalid-tabs.docx"), {invalidTabs});
    check(!invalidTabSave.saved && invalidTabSave.error.has_value(),
          "out-of-order paragraph tab stops were accepted");
}

void testSemanticTableWriting(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("semantic-table.docx");
    NewTable table;
    table.rows = 2;
    table.columns = 3;
    table.header_row = true;
    table.cells = {
        "Name", "Amount", "Notes",
        "Owls & foxes", "42", "first\nsecond",
    };
    NewDocumentBody body{{
        NewParagraph{{NewRun{"Before", {}}}},
        table,
        NewParagraph{{NewRun{"After", {}}}},
    }};
    const auto save = DocxDocument::writeNew(path, body);
    check(save.saved,
          save.error.has_value() ? save.error->message
                                 : "semantic table DOCX was not saved");

    const std::string xml = readMember(path, "word/document.xml");
    const auto before = xml.find(">Before</w:t>");
    const auto table_begin = xml.find("<w:tbl>");
    const auto after = xml.find(">After</w:t>");
    check(before != std::string::npos && table_begin != std::string::npos &&
              after != std::string::npos && before < table_begin && table_begin < after,
          "semantic table did not retain its body-block order");
    check(countOccurrences(xml, "<w:tr>") == 2,
          "semantic table emitted the wrong row count");
    check(countOccurrences(xml, "<w:tc>") == 6,
          "semantic table emitted the wrong cell count");
    check(countOccurrences(xml, "<w:gridCol ") == 3,
          "semantic table emitted the wrong column grid");
    check(countOccurrences(xml, "<w:tblHeader/>") == 1 &&
              xml.find("w:firstRow=\"1\"") != std::string::npos,
          "semantic table did not encode its header row");
    check(xml.find("w:fill=\"FFFFFF\"") != std::string::npos &&
              countOccurrences(xml, "<w:b/>") >= 3,
          "header cells were not visibly distinguished");
    check(xml.find("Owls &amp; foxes") != std::string::npos &&
              xml.find("first</w:t><w:br/><w:t>second") != std::string::npos,
          "table cell text was not safely serialized");
    check(xml.find("<w:tblBorders>") != std::string::npos &&
              xml.find("<w:tblLayout w:type=\"fixed\"/>") != std::string::npos,
          "semantic table lacks visible stable grid geometry");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 8,
          "saved table did not reopen with every cell paragraph intact");
    const std::vector<std::string> expected{
        "Before", "Name", "Amount", "Notes", "Owls & foxes", "42",
        "first\nsecond", "After"};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        check(reopened->paragraphs()[index].plainText() == expected[index],
              "saved table cell reading order changed after reopen");
    }
    check(reopened->paragraphs().front().direct_body_child &&
              !reopened->paragraphs()[1].direct_body_child &&
              reopened->paragraphs().back().direct_body_child,
          "reopened table cell was confused with a top-level paragraph");
    check(reopened->bodyBlocks().size() == 3 &&
              std::holds_alternative<ImportedParagraphBlock>(
                  reopened->bodyBlocks()[0]) &&
              std::holds_alternative<ImportedTableBlock>(
                  reopened->bodyBlocks()[1]) &&
              std::holds_alternative<ImportedParagraphBlock>(
                  reopened->bodyBlocks()[2]),
          "writer reopen did not expose paragraph/table/paragraph body order");
    const auto& imported_table =
        std::get<ImportedTableBlock>(reopened->bodyBlocks()[1]);
    check(imported_table.rows == 2 && imported_table.columns == 3 &&
              imported_table.header_row && imported_table.cells.size() == 6,
          "writer reopen lost imported table dimensions or header semantics");
    check(imported_table.style == BasicTableStyle::grid &&
              imported_table.source_style_id == "TableGrid",
          "writer reopen lost the default semantic table style");
    for (std::size_t index = 0; index < imported_table.cells.size(); ++index) {
        const std::size_t source_index =
            imported_table.cells[index].source_paragraph_index;
        check(source_index == index + 1 &&
                  reopened->paragraphs()[source_index].plainText() ==
                      expected[index + 1],
              "writer reopen did not retain row-major table cell references");
    }
    check(std::get<ImportedParagraphBlock>(reopened->bodyBlocks()[0])
                  .source_paragraph_index == 0 &&
              std::get<ImportedParagraphBlock>(reopened->bodyBlocks()[2])
                  .source_paragraph_index == 7,
          "writer reopen mapped a direct body paragraph to the wrong source index");
    check(reopened->compatibility().classification ==
              CompatibilityClass::basic_body_text_patch,
          "writer-produced simple table was incorrectly classified as view-only");

    const auto invalid_path = temporary.file("invalid-table.docx");
    NewDocumentBody malformed{{NewTable{2, 2, false, {"only one"}}}};
    const auto invalid_save = DocxDocument::writeNew(invalid_path, malformed);
    check(!invalid_save.saved && invalid_save.loss_report.hasBlockers() &&
              invalid_save.error.has_value() &&
              invalid_save.error->code == docxstudio::ooxml::ErrorCode::unsafe_edit,
          "non-rectangular semantic table was not rejected cleanly");
}

void testFormattedTableCellsAndStyles(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("formatted-table-cells.docx");

    docxstudio::ooxml::BasicRunFormat heading_format;
    heading_format.font_family = "Carlito";
    heading_format.font_size_half_points = 28;
    heading_format.bold = true;
    heading_format.italic = true;
    heading_format.underline = true;
    heading_format.strike = true;
    heading_format.foreground_rgb = 0x112233U;
    heading_format.highlight_rgb = 0xFFE699U;

    docxstudio::ooxml::BasicRunFormat emphasized_format;
    emphasized_format.bold = false;
    emphasized_format.italic = false;
    emphasized_format.underline = false;
    emphasized_format.strike = false;
    emphasized_format.baseline = docxstudio::ooxml::BasicBaseline::superscript;

    docxstudio::ooxml::BasicRunFormat empty_cell_format;
    empty_cell_format.font_family = "Carlito";
    empty_cell_format.font_size_half_points = 26;
    empty_cell_format.bold = false;
    empty_cell_format.italic = false;
    empty_cell_format.underline = false;
    empty_cell_format.strike = false;
    empty_cell_format.foreground_rgb = 0x445566U;
    empty_cell_format.highlight_rgb = 0xDDEEFFU;
    empty_cell_format.baseline =
        docxstudio::ooxml::BasicBaseline::subscript;

    NewParagraph heading{{
        NewRun{"Styled ", heading_format},
        NewRun{"header", emphasized_format},
    }};
    heading.alignment = BasicParagraphAlignment::center;
    heading.space_after_twips = 120;
    heading.line_spacing = 240;
    heading.line_spacing_rule =
        docxstudio::ooxml::BasicLineSpacingRule::automatic;

    NewParagraph justified{{NewRun{"A justified body cell", {}}}};
    justified.alignment = BasicParagraphAlignment::justified;
    NewParagraph empty_header;
    empty_header.paragraph_mark_format.emplace();
    empty_header.paragraph_mark_format->baseline =
        docxstudio::ooxml::BasicBaseline::normal;
    NewParagraph empty_cell;
    empty_cell.paragraph_mark_format = empty_cell_format;

    NewTable table;
    table.rows = 2;
    table.columns = 2;
    table.header_row = true;
    table.style = BasicTableStyle::banded_blue;
    table.cell_paragraphs = {
        heading,
        empty_header,
        justified,
        empty_cell,
    };

    const auto save = DocxDocument::writeNew(
        path, NewDocumentBody{{table}});
    check(save.saved,
          save.error ? save.error->message
                     : "formatted table-cell DOCX was not saved");

    const std::string xml = readMember(path, "word/document.xml");
    check(xml.find("<w:tblStyle w:val=\"ColorfulList-Accent1\"/>") !=
              std::string::npos,
          "semantic table style was not written as a Word table style ID");
    check(xml.find("<w:jc w:val=\"center\"/>") != std::string::npos &&
              xml.find("<w:jc w:val=\"both\"/>") != std::string::npos,
          "table-cell paragraph alignment was not serialized");
    check(xml.find("<w:color w:val=\"112233\"/>") != std::string::npos &&
              xml.find("<w:shd w:val=\"clear\" w:color=\"auto\" "
                       "w:fill=\"FFE699\"/>") != std::string::npos &&
              xml.find("<w:vertAlign w:val=\"superscript\"/>") !=
                  std::string::npos,
          "table-cell character formatting was not serialized");
    check(xml.find("<w:b w:val=\"0\"/>") != std::string::npos &&
              xml.find("<w:i w:val=\"0\"/>") != std::string::npos &&
              xml.find("<w:u w:val=\"none\"/>") != std::string::npos &&
              xml.find("<w:strike w:val=\"0\"/>") != std::string::npos,
          "explicit character-format off values were not serialized");
    check(xml.find("<w:vertAlign w:val=\"baseline\"/>") !=
              std::string::npos,
          "explicit normal baseline override was not serialized");
    const auto paragraph_mark_start = xml.find(
        "<w:pPr><w:rPr><w:rFonts w:ascii=\"Carlito\"");
    const auto paragraph_mark_end = xml.find(
        "</w:rPr>", paragraph_mark_start);
    check(paragraph_mark_start != std::string::npos &&
              paragraph_mark_end != std::string::npos,
          "empty-cell insertion formatting was not written on the paragraph mark");
    const std::string paragraph_mark_xml = xml.substr(
        paragraph_mark_start,
        paragraph_mark_end + std::string_view("</w:rPr>").size() -
            paragraph_mark_start);
    const std::array<std::string_view, 10> ordered_properties{
        "<w:rFonts", "<w:b ", "<w:i ", "<w:strike ", "<w:color ",
        "<w:sz ", "<w:szCs ", "<w:u ", "<w:shd ", "<w:vertAlign "};
    std::size_t previous_property = 0;
    bool schema_ordered = true;
    for (const auto property : ordered_properties) {
        const auto position = paragraph_mark_xml.find(
            property, previous_property);
        if (position == std::string::npos) {
            schema_ordered = false;
            break;
        }
        previous_property = position + property.size();
    }
    check(schema_ordered,
          "run properties were not emitted in CT_ParaRPr schema order");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->compatibility().classification ==
              CompatibilityClass::basic_body_text_patch &&
              std::none_of(
                  reopened->compatibility().issues.begin(),
                  reopened->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.detail.find(
                                 "Direct body paragraph-mark formatting") !=
                             std::string::npos;
                  }),
          "supported table-cell paragraph-mark formatting was misclassified");
    check(reopened->bodyBlocks().size() == 1 &&
              std::holds_alternative<ImportedTableBlock>(
                  reopened->bodyBlocks().front()),
          "formatted table did not reopen as a semantic table");
    const auto& imported =
        std::get<ImportedTableBlock>(reopened->bodyBlocks().front());
    check(imported.style == BasicTableStyle::banded_blue &&
              imported.source_style_id == "ColorfulList-Accent1",
          "formatted table style did not survive save/reopen");
    check(imported.cells.size() == 4,
          "formatted table cell mapping is incomplete");

    const auto& reopened_heading = reopened->paragraphs().at(
        imported.cells[0].source_paragraph_index);
    check(reopened_heading.plainText() == "Styled header" &&
              reopened_heading.alignment == BasicParagraphAlignment::center &&
              reopened_heading.space_after_twips == 120 &&
              reopened_heading.line_spacing == 240,
          "table-cell paragraph formatting changed on reopen");
    check(reopened_heading.runs.size() == 2 &&
              reopened_heading.runs[0].format == heading_format &&
              reopened_heading.runs[1].format.bold == false &&
              reopened_heading.runs[1].format.italic == false &&
              reopened_heading.runs[1].format.underline == false &&
              reopened_heading.runs[1].format.strike == false &&
              reopened_heading.runs[1].format.baseline ==
                  docxstudio::ooxml::BasicBaseline::superscript,
          "table-cell run formatting changed on reopen");
    const auto& reopened_justified = reopened->paragraphs().at(
        imported.cells[2].source_paragraph_index);
    check(reopened_justified.alignment ==
              BasicParagraphAlignment::justified,
          "body table-cell justification changed on reopen");
    const auto& reopened_empty_header = reopened->paragraphs().at(
        imported.cells[1].source_paragraph_index);
    check(reopened_empty_header.plainText().empty() &&
              reopened_empty_header.runs.empty() &&
              reopened_empty_header.paragraph_mark_format.has_value() &&
              reopened_empty_header.paragraph_mark_format->baseline ==
                  docxstudio::ooxml::BasicBaseline::normal &&
              reopened_empty_header.paragraph_mark_format->foreground_rgb ==
                  0xFFFFFFU,
          "empty styled header lost its insertion baseline or fallback text color");
    const auto& reopened_empty = reopened->paragraphs().at(
        imported.cells[3].source_paragraph_index);
    check(reopened_empty.plainText().empty() &&
              reopened_empty.runs.empty() &&
              reopened_empty.paragraph_mark_format == empty_cell_format,
          "empty-cell paragraph-mark insertion formatting changed on reopen");

    NewTable legacy_empty_header{1, 1, true, {""}};
    legacy_empty_header.style = BasicTableStyle::banded_blue;
    const auto legacy_empty_header_path =
        temporary.file("legacy-empty-header.docx");
    const auto legacy_empty_header_save = DocxDocument::writeNew(
        legacy_empty_header_path,
        NewDocumentBody{{legacy_empty_header}});
    check(legacy_empty_header_save.saved,
          legacy_empty_header_save.error
              ? legacy_empty_header_save.error->message
              : "legacy empty-header table was not saved");
    const std::string legacy_empty_header_xml = readMember(
        legacy_empty_header_path, "word/document.xml");
    check(legacy_empty_header_xml.find(
              "<w:p><w:pPr><w:rPr><w:b/><w:color w:val=\"FFFFFF\"/>") !=
              std::string::npos,
          "legacy empty header omitted its paragraph-mark formatting");
    check(legacy_empty_header_xml.find("<w:p><w:r>") ==
              std::string::npos,
          "legacy empty header retained a dummy run");
    auto reopened_legacy_empty_header = DocxDocument::open(
        legacy_empty_header_path, &error);
    check(reopened_legacy_empty_header != nullptr &&
              reopened_legacy_empty_header->paragraphs().size() == 1 &&
              reopened_legacy_empty_header->paragraphs()[0].runs.empty() &&
              reopened_legacy_empty_header->paragraphs()[0]
                      .paragraph_mark_format.has_value() &&
              reopened_legacy_empty_header->paragraphs()[0]
                      .paragraph_mark_format->bold == true &&
              reopened_legacy_empty_header->paragraphs()[0]
                      .paragraph_mark_format->foreground_rgb == 0xFFFFFFU,
          "legacy empty-header paragraph-mark formatting changed on reopen");

    NewTable malformed = table;
    malformed.cell_paragraphs.pop_back();
    const auto malformed_save = DocxDocument::writeNew(
        temporary.file("malformed-formatted-table.docx"),
        NewDocumentBody{{malformed}});
    check(!malformed_save.saved && malformed_save.loss_report.hasBlockers(),
          "non-rectangular formatted table payload was accepted");

    NewTable equation_table;
    equation_table.rows = 1;
    equation_table.columns = 1;
    const NewRun equation_run{
        "", {}, EquationPayload{"x^{2}", false}};
    equation_table.cell_paragraphs = {
        NewParagraph{{equation_run}}};
    const auto equation_save = DocxDocument::writeNew(
        temporary.file("table-cell-equation.docx"),
        NewDocumentBody{{equation_table}});
    check(!equation_save.saved && equation_save.loss_report.hasBlockers(),
          "equation inside the basic editable table subset was accepted");
}

void testBodyParagraphMarkFormattingRoundTrip(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("body-paragraph-mark.docx");
    docxstudio::ooxml::BasicRunFormat markFormat;
    markFormat.font_family = "Carlito";
    markFormat.font_size_half_points = 27;
    markFormat.bold = false;
    markFormat.italic = true;
    markFormat.underline = true;
    markFormat.strike = false;
    markFormat.foreground_rgb = 0x123456U;
    markFormat.highlight_rgb = 0xfedcbaU;
    markFormat.baseline = docxstudio::ooxml::BasicBaseline::subscript;
    NewParagraph authored;
    authored.paragraph_mark_format = markFormat;
    const auto save = DocxDocument::writeNew(path, {authored});
    check(save.saved,
          save.error ? save.error->message
                     : "body paragraph-mark DOCX was not saved");
    const std::string xml = readMember(path, "word/document.xml");
    check(xml.find("<w:p><w:pPr><w:rPr>") != std::string::npos &&
              xml.find("<w:color w:val=\"123456\"/>") !=
                  std::string::npos &&
              xml.find("<w:sz w:val=\"27\"/>") != std::string::npos &&
              xml.find("<w:vertAlign w:val=\"subscript\"/>") !=
                  std::string::npos &&
              xml.find("<w:r>") == std::string::npos,
          "body insertion formatting was not written on the paragraph mark");

    Error error;
    auto opened = DocxDocument::open(path, &error);
    check(opened != nullptr, error.message);
    check(opened->paragraphs().size() == 1 &&
              opened->paragraphs()[0].plainText().empty() &&
              opened->paragraphs()[0].runs.empty() &&
              opened->paragraphs()[0].paragraph_mark_format == markFormat,
          "body paragraph-mark formatting changed after save and reopen");
    check(opened->compatibility().classification ==
              CompatibilityClass::basic_body_text_patch &&
              std::none_of(
                  opened->compatibility().issues.begin(),
                  opened->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.detail.find("paragraph-mark formatting") !=
                          std::string::npos;
                  }),
          "supported body paragraph-mark formatting was misclassified");

    const auto importedPath = temporary.file("imported-paragraph-mark.docx");
    createPackage(
        importedPath,
        documentWithBody(
            "<w:p><w:pPr><w:rPr><w:b w:val=\"0\"/>"
            "</w:rPr></w:pPr><w:r><w:t>Body text</w:t></w:r></w:p>"));
    auto imported = DocxDocument::open(importedPath, &error);
    check(imported != nullptr && imported->paragraphs().size() == 1 &&
              imported->paragraphs()[0].paragraph_mark_format.has_value() &&
              imported->paragraphs()[0].paragraph_mark_format->bold == false &&
              imported->compatibility().classification ==
                  CompatibilityClass::basic_body_text_patch,
          "imported body paragraph-mark formatting was not editable");
}

void testUnknownImportedTableStyleIsRetained(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("custom-table-style.docx");
    const auto copy = temporary.file("custom-table-style-copy.docx");
    createPackage(
        path,
        documentWithBody(
            "<w:tbl><w:tblPr><w:tblStyle w:val=\"FirmLegalTable42\"/>"
            "<w:tblLook w:firstRow=\"0\"/></w:tblPr>"
            "<w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid>"
            "<w:tr><w:tc><w:p><w:r><w:t>Preserved</w:t></w:r></w:p>"
            "</w:tc></w:tr></w:tbl>"));

    Error error;
    auto opened = DocxDocument::open(path, &error);
    check(opened != nullptr, error.message);
    check(opened->bodyBlocks().size() == 1 &&
              std::holds_alternative<ImportedTableBlock>(
                  opened->bodyBlocks().front()),
          "custom-style basic table was needlessly made view-only");
    const auto& table =
        std::get<ImportedTableBlock>(opened->bodyBlocks().front());
    check(!table.style && table.source_style_id == "FirmLegalTable42",
          "unknown table style ID was discarded or misclassified");

    const auto original_bytes = readBytes(path);
    const auto save = opened->saveAs(copy);
    check(save.saved && save.byte_identical_to_opened_file &&
              readBytes(copy) == original_bytes,
          "unchanged table with an unknown style was not copied byte-identically");

    NewTable regenerated;
    regenerated.rows = 1;
    regenerated.columns = 1;
    regenerated.cells = {"Regenerated safely"};
    regenerated.style = std::nullopt;
    regenerated.source_style_id = "FirmLegalTable42";
    const auto regenerated_path =
        temporary.file("custom-table-style-regenerated.docx");
    const auto regenerated_save = DocxDocument::writeNew(
        regenerated_path, NewDocumentBody{{regenerated}});
    check(regenerated_save.saved &&
              std::any_of(
                  regenerated_save.loss_report.issues.begin(),
                  regenerated_save.loss_report.issues.end(),
                  [](const auto& issue) {
                      return issue.severity ==
                                 docxstudio::ooxml::IssueSeverity::warning &&
                             issue.code ==
                                 docxstudio::ooxml::IssueCode::unsupported_formatting;
                  }),
          regenerated_save.error ? regenerated_save.error->message
                                 : "omitted custom table style was not reported");
    check(readMember(regenerated_path, "word/document.xml")
                  .find("FirmLegalTable42") == std::string::npos,
          "generated DOCX retained a dangling custom table style reference");
    auto reopened = DocxDocument::open(regenerated_path, &error);
    check(reopened != nullptr && reopened->bodyBlocks().size() == 1,
          error.message);
    const auto& reopened_table =
        std::get<ImportedTableBlock>(reopened->bodyBlocks().front());
    check(!reopened_table.style && !reopened_table.source_style_id,
          "generated DOCX unexpectedly acquired a table style reference");
}

void testExternalSimpleTableAndAdvancedFallback(
    const TemporaryDirectory& temporary) {
    const auto simple_path = temporary.file("external-simple-table.docx");
    createPackage(
        simple_path,
        documentWithBody(
            "<w:p><w:r><w:t>Intro</w:t></w:r></w:p>"
            "<w:tbl>"
            "<w:tblPr><w:tblStyle w:val=\"TableGrid\"/>"
            "<w:tblW w:w=\"0\" w:type=\"auto\"/>"
            "<w:tblLook w:firstRow=\"1\" w:lastRow=\"0\"/></w:tblPr>"
            "<w:tblGrid><w:gridCol w:w=\"3000\"/>"
            "<w:gridCol w:w=\"3000\"/></w:tblGrid>"
            "<w:tr><w:trPr><w:tblHeader/></w:trPr>"
            "<w:tc><w:tcPr><w:tcW w:w=\"3000\" w:type=\"dxa\"/></w:tcPr>"
            "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Name</w:t></w:r></w:p></w:tc>"
            "<w:tc><w:p><w:r><w:t>Amount</w:t></w:r></w:p></w:tc></w:tr>"
            "<w:tr><w:tc><w:p><w:r><w:t>Owls</w:t></w:r></w:p></w:tc>"
            "<w:tc><w:p><w:r><w:t>42</w:t></w:r></w:p></w:tc></w:tr>"
            "</w:tbl>"
            "<w:p><w:r><w:t>Outro</w:t></w:r></w:p>"));

    Error error;
    auto simple = DocxDocument::open(simple_path, &error);
    check(simple != nullptr, error.message);
    check(simple->bodyBlocks().size() == 3 &&
              std::holds_alternative<ImportedParagraphBlock>(
                  simple->bodyBlocks()[0]) &&
              std::holds_alternative<ImportedTableBlock>(
                  simple->bodyBlocks()[1]) &&
              std::holds_alternative<ImportedParagraphBlock>(
                  simple->bodyBlocks()[2]),
          "external-like simple table was not exposed in direct body order");
    const auto& table = std::get<ImportedTableBlock>(simple->bodyBlocks()[1]);
    check(table.rows == 2 && table.columns == 2 && table.header_row &&
              table.cells.size() == 4,
          "external-like table dimensions or header were not recognized");
    check(table.style == BasicTableStyle::grid &&
              table.source_style_id == "TableGrid",
          "external built-in table style was not recognized");
    const std::vector<std::string> expected_cells{
        "Name", "Amount", "Owls", "42"};
    for (std::size_t index = 0; index < table.cells.size(); ++index) {
        const auto source = table.cells[index].source_paragraph_index;
        check(source < simple->paragraphs().size() &&
                  simple->paragraphs()[source].plainText() ==
                      expected_cells[index],
              "external-like table cell references are not row-major");
    }
    check(simple->compatibility().classification ==
              CompatibilityClass::basic_body_text_patch,
          "external-like simple table was unnecessarily downgraded");

    const auto advanced_path = temporary.file("advanced-table.docx");
    createPackage(
        advanced_path,
        documentWithBody(
            "<w:p><w:r><w:t>Editable outside</w:t></w:r></w:p>"
            "<w:tbl><w:tblGrid><w:gridCol/><w:gridCol/></w:tblGrid>"
            "<w:tr><w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc>"
            "<w:tc><w:p><w:r><w:t>B</w:t></w:r></w:p></w:tc></w:tr>"
            "<w:tr><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr>"
            "<w:p><w:r><w:t>Merged</w:t></w:r></w:p></w:tc></w:tr>"
            "</w:tbl>"));
    error = {};
    auto advanced = DocxDocument::open(advanced_path, &error);
    check(advanced != nullptr, error.message);
    check(advanced->bodyBlocks().size() == 2 &&
              std::holds_alternative<ImportedUnsupportedBodyBlock>(
                  advanced->bodyBlocks()[1]),
          "merged/non-rectangular table was exposed as an editable table");
    const auto& fallback = std::get<ImportedUnsupportedBodyBlock>(
        advanced->bodyBlocks()[1]);
    check(fallback.element_name == "w:tbl" &&
              fallback.fallback_paragraph_indices.size() == 3 &&
              !fallback.reason.empty(),
          "advanced table did not retain a useful ordered paragraph fallback");
    check(advanced->compatibility().classification ==
              CompatibilityClass::safe_text_patch &&
              std::any_of(
                  advanced->compatibility().issues.begin(),
                  advanced->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.code == IssueCode::unsupported_body_content &&
                             issue.detail.find("view-only fallback") !=
                                 std::string::npos;
                  }),
          "advanced table was not conservatively reported as preserved view-only");
}

std::string canonicalLatex(std::string_view source) {
    const auto parsed = docxstudio::math::parseLatex(source);
    check(parsed.hasValue(), "equation test fixture is not in the safe LaTeX subset");
    return docxstudio::math::toCanonicalLatex(parsed.value());
}

void testNativeOfficeMathRoundTrip(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("native-office-math.docx");
    const std::string composite = canonicalLatex(
        R"({x}+z_{i}^{2}+\frac{a_1}{\sqrt[3]{b}}+\sum_{i=1}^{n}+\int_{0}^{1}+\left\langle \alpha+\partial x \right\rangle)");
    const std::vector<std::string> matrices{
        canonicalLatex(R"(\begin{matrix}a & b \\ c & d\end{matrix})"),
        canonicalLatex(R"(\begin{pmatrix}a & b \\ c & d\end{pmatrix})"),
        canonicalLatex(R"(\begin{bmatrix}a & b \\ c & d\end{bmatrix})"),
        canonicalLatex(R"(\begin{Bmatrix}a & b \\ c & d\end{Bmatrix})"),
        canonicalLatex(R"(\begin{vmatrix}a & b \\ c & d\end{vmatrix})"),
        canonicalLatex(R"(\begin{Vmatrix}a & b \\ c & d\end{Vmatrix})"),
    };
    const std::string displayed = canonicalLatex(R"(\frac{1}{2})");
    docxstudio::ooxml::BasicRunFormat equation_format;
    equation_format.font_size_half_points = 30;
    equation_format.bold = false;
    equation_format.foreground_rgb = 0x00cc0000U;

    NewParagraph inline_paragraph;
    inline_paragraph.runs.emplace_back("Before ", docxstudio::ooxml::BasicRunFormat{});
    inline_paragraph.runs.emplace_back(
        "", equation_format, EquationPayload{composite, false});
    inline_paragraph.runs.emplace_back(" between ", docxstudio::ooxml::BasicRunFormat{});
    for (const auto& matrix : matrices) {
        inline_paragraph.runs.emplace_back(
            "", docxstudio::ooxml::BasicRunFormat{}, EquationPayload{matrix, false});
    }
    inline_paragraph.runs.emplace_back(" after", docxstudio::ooxml::BasicRunFormat{});

    NewParagraph display_paragraph{{NewRun{
        "", docxstudio::ooxml::BasicRunFormat{}, EquationPayload{displayed, true}}}};
    const auto save = DocxDocument::writeNew(path, {inline_paragraph, display_paragraph});
    check(save.saved,
          save.error ? save.error->message : "native Office Math DOCX was not saved");
    check(!save.loss_report.hasBlockers(),
          "native Office Math save unexpectedly reported compatibility loss");

    const std::string xml = readMember(path, "word/document.xml");
    check(xml.find("xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"") !=
              std::string::npos,
          "Office Math namespace is absent");
    check(xml.find("<m:oMath>") != std::string::npos &&
              xml.find("<m:oMathPara>") != std::string::npos,
          "inline or display Office Math wrapper is absent");
    check(xml.find("<m:f>") != std::string::npos &&
              xml.find("<m:rad>") != std::string::npos &&
              xml.find("<m:sSubSup>") != std::string::npos &&
              xml.find("<m:nary>") != std::string::npos,
          "fraction, radical, script, or large-operator OMML is absent");
    check(countOccurrences(xml, "<m:m>") == matrices.size(),
          "not every safe matrix environment emitted a native OMML matrix");
    check(xml.find("\\frac") == std::string::npos &&
              xml.find("\\sqrt") == std::string::npos &&
              xml.find("\\begin") == std::string::npos &&
              xml.find("〖") == std::string::npos,
          "literal LaTeX or a legacy bracket marker leaked into document.xml");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 2,
          "native Office Math document did not reopen with both paragraphs");
    const auto& inline_runs = reopened->paragraphs()[0].runs;
    check(inline_runs.size() == matrices.size() + 4,
          "inline text/equation order changed after reopen");
    check(inline_runs[0].plainText() == "Before " &&
              inline_runs[2].plainText() == " between " &&
              inline_runs.back().plainText() == " after",
          "surrounding text moved around inline equations");
    check(inline_runs[1].fragments.size() == 1 &&
              inline_runs[1].fragments[0].kind == FragmentKind::equation &&
              inline_runs[1].fragments[0].equation.has_value() &&
              inline_runs[1].fragments[0].equation->canonical_latex == composite &&
              !inline_runs[1].fragments[0].equation->display,
          "composite inline equation did not round-trip as a structured fragment");
    check(inline_runs[1].format.font_size_half_points == 30 &&
              inline_runs[1].format.bold == false &&
              inline_runs[1].format.foreground_rgb == 0x00cc0000U &&
              inline_runs[1].paragraph_style_overrides == equation_format,
          "uniform nested Office Math w:rPr formatting did not round-trip as direct formatting");
    for (std::size_t index = 0; index < matrices.size(); ++index) {
        const auto& fragment = inline_runs[index + 3].fragments.at(0);
        check(fragment.kind == FragmentKind::equation && fragment.equation.has_value() &&
                  fragment.equation->canonical_latex == matrices[index] &&
                  !fragment.equation->display,
              "matrix equation did not round-trip through its native environment");
    }
    const auto& display_fragment = reopened->paragraphs()[1].runs.at(0).fragments.at(0);
    check(display_fragment.kind == FragmentKind::equation &&
              display_fragment.equation.has_value() &&
              display_fragment.equation->canonical_latex == displayed &&
              display_fragment.equation->display,
          "display equation did not retain its display semantics");
    check(reopened->paragraphs()[0].plainText().find("\xef\xbf\xbc") !=
              std::string::npos,
          "equations do not occupy an atomic inline position in plain text");
    check(reopened->compatibility().classification ==
              CompatibilityClass::basic_body_text_patch,
          "our native Office Math subset was classified as unsupported content");
}

void testUnsupportedOfficeMathIsPreserved(const TemporaryDirectory& temporary) {
    const auto source = temporary.file("unsupported-office-math.docx");
    const auto output = temporary.file("unsupported-office-math-edited.docx");
    const std::string unsupported =
        "<m:oMath><m:func><m:fName><m:r><m:t>f</m:t></m:r></m:fName>"
        "<m:e><m:r><m:t>x</m:t></m:r></m:e></m:func></m:oMath>";
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"" + std::string(kWordNamespace) +
        "\" xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\">"
        "<w:body><w:p><w:r><w:t>Before</w:t></w:r>" + unsupported +
        "<w:r><w:t>After</w:t></w:r></w:p><w:sectPr/></w:body></w:document>";
    createPackage(source, xml);

    Error error;
    auto document = DocxDocument::open(source, &error);
    check(document != nullptr, error.message);
    check(document->compatibility().classification == CompatibilityClass::safe_text_patch,
          "unsupported Office Math did not conservatively downgrade compatibility");
    check(std::any_of(
              document->compatibility().issues.begin(),
              document->compatibility().issues.end(),
              [](const auto& issue) {
                  return issue.code == IssueCode::unsupported_body_content &&
                         issue.detail.find("Office Math") != std::string::npos;
              }),
          "unsupported Office Math did not produce a compatibility warning");

    const auto& before = document->paragraphs()[0].runs[0].fragments[0];
    check(before.text_span_id.has_value(), "text next to unsupported Office Math is not editable");
    const auto edit = document->replaceText(*before.text_span_id, "Changed");
    check(edit.accepted, edit.error ? edit.error->message : "safe adjacent edit was refused");
    const auto save = document->saveAs(output);
    check(save.saved,
          save.error ? save.error->message : "unsupported Office Math preservation save failed");
    const std::string saved_xml = readMember(output, "word/document.xml");
    check(saved_xml.find(unsupported) != std::string::npos,
          "unsupported Office Math subtree was modified or flattened");
    check(saved_xml.find("<w:t>Changed</w:t>") != std::string::npos,
          "safe text edit next to preserved Office Math was lost");

    const auto mixed_source = temporary.file(
        "mixed-office-math-formatting.docx");
    const auto mixed_output = temporary.file(
        "mixed-office-math-formatting-edited.docx");
    const std::string mixed =
        "<m:oMath><m:f><m:num><m:r><w:rPr><w:color w:val=\"CC0000\"/>"
        "</w:rPr><m:t>x</m:t></m:r></m:num><m:den><m:r><w:rPr>"
        "<w:color w:val=\"0000CC\"/></w:rPr><m:t>y</m:t></m:r>"
        "</m:den></m:f></m:oMath>";
    const std::string mixed_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<w:document xmlns:w=\"" + std::string(kWordNamespace) +
        "\" xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\">"
        "<w:body><w:p><w:r><w:t>Before</w:t></w:r>" + mixed +
        "<w:r><w:t>After</w:t></w:r></w:p><w:sectPr/></w:body></w:document>";
    createPackage(mixed_source, mixed_xml);

    auto mixed_document = DocxDocument::open(mixed_source, &error);
    check(mixed_document &&
              mixed_document->compatibility().classification ==
                  CompatibilityClass::safe_text_patch &&
              std::any_of(
                  mixed_document->compatibility().issues.begin(),
                  mixed_document->compatibility().issues.end(),
                  [](const auto& issue) {
                      return issue.code ==
                                 IssueCode::unsupported_body_content &&
                             issue.detail.find("Office Math") !=
                                 std::string::npos;
                  }) &&
              std::none_of(
                  mixed_document->paragraphs()[0].runs.begin(),
                  mixed_document->paragraphs()[0].runs.end(),
                  [](const auto& run) {
                      return std::any_of(
                          run.fragments.begin(), run.fragments.end(),
                          [](const auto& fragment) {
                              return fragment.kind == FragmentKind::equation;
                          });
                  }),
          "mixed Office Math w:rPr was silently flattened into one editable equation atom");
    const auto& mixed_before =
        mixed_document->paragraphs()[0].runs[0].fragments[0];
    check(mixed_before.text_span_id.has_value(),
          "text next to mixed-format Office Math is not editable");
    const auto mixed_edit = mixed_document->replaceText(
        *mixed_before.text_span_id, "Retained");
    check(mixed_edit.accepted,
          mixed_edit.error ? mixed_edit.error->message
                           : "safe mixed-equation adjacent edit was refused");
    const auto mixed_save = mixed_document->saveAs(mixed_output);
    check(mixed_save.saved,
          mixed_save.error ? mixed_save.error->message
                           : "mixed equation preservation save failed");
    const std::string mixed_saved_xml = readMember(
        mixed_output, "word/document.xml");
    check(mixed_saved_xml.find(mixed) != std::string::npos &&
              mixed_saved_xml.find("<w:t>Retained</w:t>") !=
                  std::string::npos,
          "mixed Office Math formatting was flattened or blocked an adjacent safe text patch");
}

void testExactUnchangedSave(const TemporaryDirectory& temporary) {
    const auto source = temporary.file("source.docx");
    const auto copy = temporary.file("copy.docx");
    createPackage(source, documentWithBody("<w:p><w:r><w:t>Hello</w:t></w:r></w:p>"));
    const auto original = readBytes(source);

    Error error;
    auto document = DocxDocument::open(source, &error);
    check(document != nullptr, error.message);
    check(std::filesystem::remove(source), "could not remove source after open");
    const auto save = document->saveAs(copy);
    check(save.saved, save.error.has_value() ? save.error->message : "unchanged Save As failed");
    check(save.byte_identical_to_opened_file, "unchanged save did not report exact copy");
    check(readBytes(copy) == original, "unchanged Save As bytes differ");
    check(!document->dirty(), "successful unchanged save should remain clean");
}

void testNewRunTabsAndBreaks(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("controls.docx");
    NewRun run{
        std::string(" lead & \t middle\r\nnext\nline\rlast \xe2\x80\xa8 final < "),
        {}};
    run.format.italic = true;
    const auto save = DocxDocument::writeNew(path, {NewParagraph{{run}}});
    check(save.saved, save.error ? save.error->message : "control DOCX was not saved");

    const std::string xml = readMember(path, "word/document.xml");
    check(countOccurrences(xml, "<w:tab/>") == 1, "tab was not serialized as w:tab");
    check(countOccurrences(xml, "<w:br/>") == 4,
          "CR/LF/CRLF/U+2028 were not serialized as four w:br elements");
    check(xml.find("<w:t xml:space=\"preserve\"> lead &amp; </w:t><w:tab/>"
                   "<w:t xml:space=\"preserve\"> middle</w:t><w:br/>") !=
              std::string::npos,
          "text around a tab/break was not segmented or space-preserved");
    check(xml.find("<w:t xml:space=\"preserve\"> final &lt; </w:t>") !=
              std::string::npos,
          "final control-delimited text was not escaped or space-preserved");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 1, "control DOCX paragraph count differs");
    check(reopened->paragraphs()[0].plainText() ==
              " lead & \t middle\nnext\nline\nlast \n final < ",
          "serialized control elements did not reopen as normalized text");
    check(reopened->paragraphs()[0].runs.size() == 1,
          "control serialization unnecessarily split the formatted run");
    check(reopened->paragraphs()[0].runs[0].format.italic.value_or(false),
          "run formatting did not cover serialized tabs and breaks");
}

void testHeaderFooterRoundTrip(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("header-footer.docx");
    NewDocumentBody body{{NewParagraph{{NewRun{"Body text", {}}}}}};
    body.header_text = "Owl Docs";
    body.footer_text = "Page {PAGE} of {PAGES}";
    const auto save = DocxDocument::writeNew(path, body);
    check(save.saved,
          save.error ? save.error->message : "header/footer DOCX was not saved");

    const std::string document_xml = readMember(path, "word/document.xml");
    const std::string header_xml = readMember(path, "word/header1.xml");
    const std::string footer_xml = readMember(path, "word/footer1.xml");
    check(document_xml.find(
              "<w:headerReference w:type=\"default\" r:id=\"rIdHeader1\"/>") !=
              std::string::npos &&
              document_xml.find(
              "<w:footerReference w:type=\"default\" r:id=\"rIdFooter1\"/>") !=
              std::string::npos,
          "document section omitted its header/footer references");
    check(header_xml.find("<w:t>Owl Docs</w:t>") != std::string::npos,
          "header text was not serialized");
    check(footer_xml.find("w:instr=\" PAGE \"") != std::string::npos &&
              footer_xml.find("w:instr=\" NUMPAGES \"") != std::string::npos,
          "footer page fields were not serialized as Word fields");

    Error error;
    auto reopened = DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->headerText() == std::optional<std::string>{"Owl Docs"} &&
              reopened->footerText() ==
                  std::optional<std::string>{"Page {PAGE} of {PAGES}"},
          "header/footer text and page-field tokens did not reopen");
    check(reopened->isCanonicalRegeneratableSimplePackage(
              DocumentDefaults{}),
          "an Owl Docs header/footer package was not safely regeneratable");
}

void testUnsupportedFormattingIsReported(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("unsupported-formatting.docx");
    createPackage(
        path,
        documentWithBody(
            "<w:p><w:pPr><w:widowControl/></w:pPr><w:r><w:rPr>"
            "<w:smallCaps/></w:rPr><w:t>Preserve me</w:t></w:r></w:p>"));

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document != nullptr, error.message);
    check(!document->paragraphs()[0].format_is_basic,
          "unsupported w:pPr flag was treated as basic formatting");
    check(!document->paragraphs()[0].runs[0].format_is_basic,
          "unsupported w:rPr flag was treated as basic formatting");
    check(document->compatibility().classification == CompatibilityClass::safe_text_patch,
          "unsupported formatting did not downgrade basic-body classification");
    const auto& issues = document->compatibility().issues;
    check(std::count_if(issues.begin(), issues.end(), [](const auto& issue) {
              return issue.code == IssueCode::unsupported_formatting;
          }) == 2,
          "unsupported paragraph and run formatting warnings were not reported");
}

void testXmlComplexityLimits(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("deep.docx");
    std::string body;
    for (int depth = 0; depth < 300; ++depth) body += "<w:sdt>";
    body += "<w:p><w:r><w:t>Too deep</w:t></w:r></w:p>";
    for (int depth = 0; depth < 300; ++depth) body += "</w:sdt>";
    createPackage(path, documentWithBody(body));

    Error error;
    auto document = DocxDocument::open(path, &error);
    check(document == nullptr, "deeply nested document bypassed the XML depth limit");
    check(error.code == docxstudio::ooxml::ErrorCode::xml_complexity_exceeded,
          "deep XML rejection used the wrong error code");
    check(error.message.find("depth limit") != std::string::npos,
          "deep XML rejection did not identify the exceeded limit");

    const auto wide_path = temporary.file("node-count.docx");
    createPackage(
        wide_path,
        documentWithBody("<w:p><w:r><w:t>Count nodes</w:t></w:r></w:p>"));
    OpenOptions options;
    options.max_xml_nodes = 7;
    error = {};
    document = DocxDocument::open(wide_path, &error, options);
    check(document == nullptr, "document bypassed the XML node-count limit");
    check(error.code == docxstudio::ooxml::ErrorCode::xml_complexity_exceeded,
          "node-count rejection used the wrong error code");
    check(error.message.find("node-count limit") != std::string::npos,
          "node-count rejection did not identify the exceeded limit");
}

void testCanonicalSimplePackageRegenerationEligibility(
    const TemporaryDirectory& temporary) {
    BasicRunFormat run_format;
    run_format.font_family = "Liberation Serif";
    run_format.font_size_half_points = 25;
    run_format.bold = true;
    run_format.italic = false;
    run_format.underline = true;
    run_format.strike = false;
    run_format.foreground_rgb = 0x00336699U;
    run_format.highlight_rgb = 0x00ffee88U;
    run_format.baseline = BasicBaseline::superscript;
    NewParagraph paragraph;
    paragraph.runs.push_back(
        {"A simple\tOwl Docs document\nwith a second line", run_format});
    paragraph.alignment = BasicParagraphAlignment::justified;
    paragraph.left_indent_twips = 360;
    paragraph.right_indent_twips = 180;
    paragraph.first_line_indent_twips = -120;
    paragraph.space_before_twips = 60;
    paragraph.space_after_twips = 120;
    paragraph.line_spacing = 280;
    paragraph.line_spacing_rule = BasicLineSpacingRule::exact;
    paragraph.keep_with_next = true;
    paragraph.keep_lines = false;
    paragraph.page_break_before = false;
    paragraph.left_tab_stops_twips = {360, 720};
    BasicRunFormat paragraph_mark_format;
    paragraph_mark_format.bold = false;
    paragraph.paragraph_mark_format = paragraph_mark_format;
    DocumentDefaults defaults;
    defaults.font_family = "Liberation Serif";
    defaults.font_size_half_points = 25;
    defaults.default_tab_stop_twips = 333;

    const auto canonical_path = temporary.file("canonical-simple.docx");
    const auto canonical_save = DocxDocument::writeNew(
        canonical_path, {paragraph}, {}, defaults);
    check(canonical_save.saved,
          canonical_save.error
              ? canonical_save.error->message
              : "canonical simple package was not created");
    Error error;
    auto canonical = DocxDocument::open(canonical_path, &error);
    check(canonical != nullptr, error.message);
    check(canonical->compatibility().classification ==
                  CompatibilityClass::basic_body_text_patch &&
              canonical->compatibility().issues.empty() &&
              canonical->isCanonicalRegeneratableSimplePackage(defaults),
          "current-writer text-only package was not recognized as fully regeneratable");
    auto mismatched_defaults = defaults;
    mismatched_defaults.font_size_half_points += 1;
    check(!canonical->isCanonicalRegeneratableSimplePackage(
              mismatched_defaults),
          "canonical package accepted regeneration with different document defaults");

    const auto expect_document_variant_rejected =
        [&](const char* file_name, std::string_view needle,
            std::string_view replacement, const char* failure) {
            const auto variant_path = temporary.file(file_name);
            check(std::filesystem::copy_file(canonical_path, variant_path),
                  "could not copy canonical body-grammar fixture");
            std::string document_xml = readMember(
                variant_path, "word/document.xml");
            const auto position = document_xml.find(needle);
            check(position != std::string::npos,
                  "canonical body-grammar fixture marker is missing");
            document_xml.replace(position, needle.size(), replacement);
            replaceMember(variant_path, "word/document.xml", document_xml);
            auto variant = DocxDocument::open(variant_path, &error);
            check(variant != nullptr, error.message);
            check(!variant->isCanonicalRegeneratableSimplePackage(defaults),
                  failure);
        };

    expect_document_variant_rejected(
        "canonical-run-complex-script-font.docx",
        "<w:rFonts w:ascii=\"Liberation Serif\" "
        "w:hAnsi=\"Liberation Serif\"/>",
        "<w:rFonts w:ascii=\"Liberation Serif\" "
        "w:hAnsi=\"Liberation Serif\" w:cs=\"Noto Sans Arabic\"/>",
        "an unpreserved complex-script run font was declared regeneratable");
    expect_document_variant_rejected(
        "canonical-break-clear.docx", "<w:br/>",
        "<w:br w:clear=\"all\"/>",
        "an attributed line break was declared regeneratable");
    expect_document_variant_rejected(
        "canonical-tab-attribute.docx", "<w:tab/>",
        "<w:tab w:future=\"1\"/>",
        "a tab with an unknown attribute was declared regeneratable");
    expect_document_variant_rejected(
        "canonical-carriage-return.docx", "<w:br/>", "<w:cr/>",
        "a non-writer carriage-return element was declared regeneratable");
    expect_document_variant_rejected(
        "canonical-text-attribute.docx", "<w:t>",
        "<w:t w:future=\"1\">",
        "a text node with an unknown attribute was declared regeneratable");
    expect_document_variant_rejected(
        "canonical-run-namespace.docx", "<w:r>",
        "<w:r xmlns:future=\"urn:owl-docs:future\" "
        "future:opaque=\"1\">",
        "a run with an unknown namespace attribute was declared regeneratable");

    const auto custom_path = temporary.file("canonical-plus-custom.docx");
    check(std::filesystem::copy_file(canonical_path, custom_path),
          "could not copy canonical custom-part fixture");
    appendMembers(custom_path,
                  {{"customXml/item1.bin",
                    std::string("opaque\0payload", 14)}});
    auto custom = DocxDocument::open(custom_path, &error);
    check(custom != nullptr, error.message);
    check(custom->compatibility().classification ==
                  CompatibilityClass::basic_body_text_patch &&
              custom->compatibility().issues.empty() &&
              !custom->isCanonicalRegeneratableSimplePackage(defaults),
          "an opaque custom package member was incorrectly declared regeneratable");

    const auto external_path = temporary.file("canonical-plus-external.docx");
    check(std::filesystem::copy_file(canonical_path, external_path),
          "could not copy canonical external-relationship fixture");
    std::string external_relationships = readMember(
        external_path, "word/_rels/document.xml.rels");
    const auto relationship_end = external_relationships.find(
        "</Relationships>");
    check(relationship_end != std::string::npos,
          "canonical relationships closing element is missing");
    external_relationships.insert(
        relationship_end,
        "<Relationship Id=\"rIdExternal\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" "
        "Target=\"https://example.invalid/\" TargetMode=\"External\"/>");
    replaceMember(external_path, "word/_rels/document.xml.rels",
                  external_relationships);
    auto external = DocxDocument::open(external_path, &error);
    check(external != nullptr, error.message);
    check(!external->isCanonicalRegeneratableSimplePackage(defaults),
          "an external relationship was incorrectly declared regeneratable");

    const auto section_path = temporary.file("canonical-plus-columns.docx");
    check(std::filesystem::copy_file(canonical_path, section_path),
          "could not copy canonical section-property fixture");
    std::string section_xml = readMember(section_path, "word/document.xml");
    const auto section_end = section_xml.find("</w:sectPr>");
    check(section_end != std::string::npos,
          "canonical final section closing element is missing");
    section_xml.insert(section_end, "<w:cols w:num=\"2\"/>");
    replaceMember(section_path, "word/document.xml", section_xml);
    auto section = DocxDocument::open(section_path, &error);
    check(section != nullptr, error.message);
    check(section->compatibility().classification ==
                  CompatibilityClass::basic_body_text_patch &&
              section->compatibility().issues.empty() &&
              !section->isCanonicalRegeneratableSimplePackage(defaults),
          "an unsupported final-section column setting was incorrectly declared regeneratable");
}

void testTextPatchPreservesOpaqueMembers(const TemporaryDirectory& temporary) {
    const auto source = temporary.file("complex.docx");
    const auto output = temporary.file("complex-edited.docx");
    createPackage(
        source,
        documentWithBody(
            "<w:p><w:r><w:t>Hello &amp; original</w:t></w:r></w:p>"
            "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Table text</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"));
    const std::string opaque_before = readMember(source, "customXml/item1.bin");

    Error error;
    auto document = DocxDocument::open(source, &error);
    check(document != nullptr, error.message);
    check(document->paragraphs().size() == 2, "table paragraph should be parsed for display");
    check(
        document->compatibility().classification ==
            CompatibilityClass::basic_body_text_patch,
        "simple table package was not recognized as basic editable body content (actual " +
            std::to_string(static_cast<int>(document->compatibility().classification)) + ")");
    const auto& fragment = document->paragraphs()[0].runs[0].fragments[0];
    check(fragment.kind == FragmentKind::text && fragment.text_span_id.has_value(), "missing text span id");
    const auto edit = document->replaceText(*fragment.text_span_id, "Changed & retained");
    check(edit.accepted, edit.error.has_value() ? edit.error->message : "text patch refused");
    check(document->dirty(), "accepted text patch should mark document dirty");
    const auto save = document->saveAs(output);
    check(save.saved, save.error.has_value() ? save.error->message : "modified save failed");
    check(!save.byte_identical_to_opened_file, "modified save reported byte-identical");
    check(!document->dirty(), "successful modified save should establish a clean baseline");
    check(readMember(output, "customXml/item1.bin") == opaque_before, "opaque package part changed");
    const std::string output_xml = readMember(output, "word/document.xml");
    check(output_xml.find("Changed &amp; retained") != std::string::npos, "escaped text patch missing");
    check(output_xml.find("<w:tbl>") != std::string::npos,
          "unmodified simple table was not preserved");
    std::string expected_xml = readMember(source, "word/document.xml");
    const std::string original_text = "Hello &amp; original";
    const std::size_t original_position = expected_xml.find(original_text);
    check(original_position != std::string::npos, "fixture text missing");
    expected_xml.replace(original_position, original_text.size(), "Changed &amp; retained");
    check(output_xml == expected_xml, "modified save changed XML outside the selected w:t content");
}

void testUnsafeEditsAreRefused(const TemporaryDirectory& temporary) {
    const auto unsigned_path = temporary.file("unsigned.docx");
    createPackage(unsigned_path, documentWithBody("<w:p><w:r><w:t>Hello</w:t></w:r></w:p>"));
    Error error;
    auto document = DocxDocument::open(unsigned_path, &error);
    check(document != nullptr, error.message);
    const auto id = *document->paragraphs()[0].runs[0].fragments[0].text_span_id;
    const auto whitespace = document->replaceText(id, " leading space");
    check(!whitespace.accepted, "unsafe whitespace patch should be refused");
    check(
        !whitespace.loss_report.issues.empty() &&
            whitespace.loss_report.issues[0].code == IssueCode::whitespace_preservation_required,
        "whitespace refusal should include a loss reason");

    const auto signed_path = temporary.file("signed.docx");
    createPackage(
        signed_path,
        documentWithBody("<w:p><w:r><w:t>Signed text</w:t></w:r></w:p>"),
        true);
    auto signed_document = DocxDocument::open(signed_path, &error);
    check(signed_document != nullptr, error.message);
    check(
        signed_document->compatibility().classification == CompatibilityClass::exact_round_trip_only,
        "signed package should be exact-round-trip-only");
    const auto signed_id = *signed_document->paragraphs()[0].runs[0].fragments[0].text_span_id;
    const auto signed_edit = signed_document->replaceText(signed_id, "Invalidates signature");
    check(!signed_edit.accepted, "signed package edit should be refused");
    check(signed_edit.loss_report.hasBlockers(), "signed package refusal needs a blocker report");
    const auto signed_copy = temporary.file("signed-copy.docx");
    const auto signed_bytes = readBytes(signed_path);
    const auto signed_save = signed_document->saveAs(signed_copy);
    check(signed_save.saved, "unchanged signed package Save As should be allowed");
    check(readBytes(signed_copy) == signed_bytes, "signed package exact Save As changed bytes");
}

void testExcludedWordMainTypesAreRejected(const TemporaryDirectory& temporary) {
    const std::vector<std::pair<std::string_view, std::string_view>> excluded_types{
        {"macro-enabled-renamed.docx",
         "application/vnd.ms-word.document.macroEnabled.main+xml"},
        {"template-renamed.docx",
         "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml"},
        {"macro-template-renamed.docx",
         "application/vnd.ms-word.template.macroEnabledTemplate.main+xml"}};

    for (const auto& [name, content_type] : excluded_types) {
        const auto path = temporary.file(name);
        createPackage(
            path,
            documentWithBody("<w:p><w:r><w:t>Excluded format</w:t></w:r></w:p>"),
            false,
            content_type);
        Error error;
        auto document = DocxDocument::open(path, &error);
        check(document == nullptr,
              "excluded macro-enabled document or template was accepted");
        check(error.code == docxstudio::ooxml::ErrorCode::invalid_opc_metadata,
              "excluded Word main type used the wrong error code");
        check(error.message.find("Only macro-free DOCX documents are supported") !=
                  std::string::npos,
              "excluded Word main type did not produce a clear format error");
    }
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        testInlineImagePageBreakAndTablePresentation(temporary);
        testUnsupportedAnchorIsOpaqueAndPreserved(temporary);
        testInlineImageTableCellIsPreservedViewOnly(temporary);
        testRepeatedImageReferencesShareBoundedStorage(temporary);
        testStylesThemesAndNativeNumberingImport(temporary);
        testNumberingLevelJustificationIsConservative(temporary);
        testNativeNumberingWriting(temporary);
        testUnsafeImageRelationshipsAreNotLoaded(temporary);
        testNewDocumentCreation(temporary);
        testNativeParagraphStyleWriting(temporary);
        testSemanticTableWriting(temporary);
        testFormattedTableCellsAndStyles(temporary);
        testBodyParagraphMarkFormattingRoundTrip(temporary);
        testUnknownImportedTableStyleIsRetained(temporary);
        testExternalSimpleTableAndAdvancedFallback(temporary);
        testNativeOfficeMathRoundTrip(temporary);
        testUnsupportedOfficeMathIsPreserved(temporary);
        testNewDocumentDefaultStyle(temporary);
        testCustomDocumentDefaults(temporary);
        testNewRunTabsAndBreaks(temporary);
        testHeaderFooterRoundTrip(temporary);
        testExactUnchangedSave(temporary);
        testTextPatchPreservesOpaqueMembers(temporary);
        testUnsupportedFormattingIsReported(temporary);
        testXmlComplexityLimits(temporary);
        testCanonicalSimplePackageRegenerationEligibility(temporary);
        testUnsafeEditsAreRefused(temporary);
        testExcludedWordMainTypesAreRejected(temporary);
        std::cout << "OOXML tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "OOXML test failure: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
