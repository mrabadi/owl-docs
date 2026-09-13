#include "docxstudio/ooxml/docx_document.h"

#include <zip.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
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

namespace ooxml = docxstudio::ooxml;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/owl-docs-m0-roundtrip-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
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

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "could not open generated package");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> readMember(
    const std::filesystem::path& path, std::string_view member) {
    int code = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &code);
    check(archive != nullptr, "could not open generated DOCX as ZIP");
    zip_stat_t status;
    zip_stat_init(&status);
    check(zip_stat(archive, std::string(member).c_str(), 0, &status) == 0,
          "expected package member is absent");
    zip_file_t* file = zip_fopen(archive, std::string(member).c_str(), 0);
    check(file != nullptr, "could not open package member");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.size));
    std::size_t consumed = 0;
    while (consumed < bytes.size()) {
        const auto count = zip_fread(
            file, bytes.data() + consumed,
            static_cast<zip_uint64_t>(bytes.size() - consumed));
        check(count > 0, "could not read complete package member");
        consumed += static_cast<std::size_t>(count);
    }
    check(zip_fclose(file) == 0, "could not close package member");
    zip_discard(archive);
    return bytes;
}

std::string readTextMember(
    const std::filesystem::path& path, std::string_view member) {
    const auto bytes = readMember(path, member);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::uint8_t> decodeBase64(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> output;
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    for (const char character : encoded) {
        if (character == '=') break;
        const auto value = alphabet.find(character);
        check(value != std::string_view::npos, "invalid base64 fixture");
        accumulator = (accumulator << 6U) |
                      static_cast<std::uint32_t>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            output.push_back(static_cast<std::uint8_t>(
                (accumulator >> bits) & 0xffU));
        }
    }
    return output;
}

std::vector<std::uint8_t> onePixelPng() {
    return decodeBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
        "+A8AAQUBAScY42YAAAAASUVORK5CYII=");
}

std::vector<std::uint8_t> onePixelJpeg() {
    return decodeBase64(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkS"
        "Ew8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJ"
        "CQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIy"
        "MjIyMjIyMjIyMjIyMjL/wAARCAABAAEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEA"
        "AAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIh"
        "MUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZ"
        "mqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx"
        "8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAV"
        "YnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hp"
        "anN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD3"
        "+iiigD//2Q==");
}

ooxml::NewInlineImage image(
    ooxml::RasterImageFormat format, std::string name,
    std::int64_t width, std::int64_t height,
    std::vector<std::uint8_t> bytes) {
    ooxml::NewInlineImage result;
    result.format = format;
    result.name = std::move(name);
    result.width_emu = width;
    result.height_emu = height;
    result.bytes = std::move(bytes);
    return result;
}

ooxml::NewRun imageRun(ooxml::NewInlineImage payload) {
    ooxml::NewRun run;
    run.inline_image = std::move(payload);
    return run;
}

void testNativeInlinePngAndJpeg(const TemporaryDirectory& temporary) {
    const auto path = temporary.file("inline-images.docx");
    const auto exact_copy = temporary.file("inline-images-copy.docx");
    const auto png = onePixelPng();
    const auto jpeg = onePixelJpeg();

    ooxml::NewParagraph paragraph;
    paragraph.runs.emplace_back("Before ", ooxml::BasicRunFormat{});
    paragraph.runs.push_back(imageRun(image(
        ooxml::RasterImageFormat::png, "Orange owl", 914400, 457200,
        png)));
    paragraph.runs.emplace_back(" between ", ooxml::BasicRunFormat{});
    paragraph.runs.push_back(imageRun(image(
        ooxml::RasterImageFormat::jpeg, "Gray owl", 457200, 914400,
        jpeg)));

    const auto save = ooxml::DocxDocument::writeNew(path, {paragraph});
    check(save.saved,
          save.error ? save.error->message : "inline-image DOCX was not saved");
    check(!save.loss_report.hasBlockers(),
          "valid inline images produced a blocking loss report");

    check(readMember(path, "word/media/image1.png") == png,
          "PNG package bytes changed");
    check(readMember(path, "word/media/image2.jpg") == jpeg,
          "JPEG package bytes changed");
    const auto content_types = readTextMember(path, "[Content_Types].xml");
    check(content_types.find(
              "Extension=\"png\" ContentType=\"image/png\"") !=
              std::string::npos &&
              content_types.find(
                  "Extension=\"jpg\" ContentType=\"image/jpeg\"") !=
                  std::string::npos,
          "raster content types are absent");
    const auto relationships =
        readTextMember(path, "word/_rels/document.xml.rels");
    check(relationships.find(
              "Id=\"rIdImage1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image1.png\"") !=
              std::string::npos &&
              relationships.find(
                  "Id=\"rIdImage2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image2.jpg\"") !=
                  std::string::npos &&
              relationships.find("TargetMode=\"External\"") ==
                  std::string::npos,
          "image relationships are absent or external");
    const auto document_xml = readTextMember(path, "word/document.xml");
    check(std::count(document_xml.begin(), document_xml.end(), '\0') == 0,
          "document XML contains a NUL byte");
    check(document_xml.find("<wp:inline") != std::string::npos &&
              document_xml.find("r:embed=\"rIdImage1\"") !=
                  std::string::npos &&
              document_xml.find("r:embed=\"rIdImage2\"") !=
                  std::string::npos &&
              document_xml.find("name=\"Orange owl\"") !=
                  std::string::npos,
          "native inline DrawingML is incomplete");

    ooxml::Error error;
    auto reopened = ooxml::DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 1,
          "image paragraph did not reopen");
    const auto& runs = reopened->paragraphs()[0].runs;
    check(runs.size() == 4 && runs[1].fragments.size() == 1 &&
              runs[3].fragments.size() == 1,
          "inline run order changed after reopen");
    const auto& imported_png = *runs[1].fragments[0].inline_image;
    const auto& imported_jpeg = *runs[3].fragments[0].inline_image;
    check(imported_png.bytes == png && imported_png.name == "Orange owl" &&
              imported_png.width_emu == 914400 &&
              imported_png.height_emu == 457200 &&
              imported_png.package_member == "word/media/image1.png" &&
              imported_png.content_type == "image/png",
          "PNG semantics changed after reopen");
    check(imported_jpeg.bytes == jpeg && imported_jpeg.name == "Gray owl" &&
              imported_jpeg.width_emu == 457200 &&
              imported_jpeg.height_emu == 914400 &&
              imported_jpeg.package_member == "word/media/image2.jpg" &&
              imported_jpeg.content_type == "image/jpeg",
          "JPEG semantics changed after reopen");
    check(reopened->compatibility().classification ==
              ooxml::CompatibilityClass::basic_body_text_patch,
          "authored inline DrawingML was classified as unsupported");

    const auto original = readFile(path);
    const auto copied = reopened->saveAs(exact_copy);
    check(copied.saved && copied.byte_identical_to_opened_file,
          "unchanged image package was not exactly copied");
    check(readFile(exact_copy) == original,
          "unchanged image Save As changed package bytes");
}

void testNativePictureLayoutsAndAltText(
    const TemporaryDirectory& temporary) {
    const auto path = temporary.file("picture-layouts.docx");
    const auto png = onePixelPng();

    auto inline_image = image(
        ooxml::RasterImageFormat::png, "Inline owl", 914400, 457200,
        png);
    inline_image.accessible_name = "Inline description";
    inline_image.layout.distance_top_emu = 11;
    inline_image.layout.distance_right_emu = 22;
    inline_image.layout.distance_bottom_emu = 33;
    inline_image.layout.distance_left_emu = 44;

    auto square_image = image(
        ooxml::RasterImageFormat::png, "Square owl", 800000, 600000,
        png);
    square_image.accessible_name = "Square & <owl>";
    square_image.layout = {
        ooxml::ImagePlacement::square, 101, 202, 303, 404, false};

    auto top_bottom_image = image(
        ooxml::RasterImageFormat::png, "Top-bottom owl", 700000,
        500000, png);
    top_bottom_image.accessible_name = "Top and bottom description";
    top_bottom_image.layout = {
        ooxml::ImagePlacement::top_and_bottom, 505, 606, 707, 808,
        true};

    ooxml::NewParagraph paragraph;
    paragraph.runs.push_back(imageRun(std::move(inline_image)));
    paragraph.runs.push_back(imageRun(std::move(square_image)));
    paragraph.runs.push_back(imageRun(std::move(top_bottom_image)));
    const auto save = ooxml::DocxDocument::writeNew(path, {paragraph});
    check(save.saved,
          save.error ? save.error->message
                     : "picture-layout DOCX was not saved");

    const std::string xml = readTextMember(path, "word/document.xml");
    check(std::count(xml.begin(), xml.end(), '\0') == 0 &&
              xml.find("<wp:inline distT=\"11\" distB=\"33\" distL=\"44\" distR=\"22\">") !=
                  std::string::npos &&
              xml.find("<wp:wrapSquare wrapText=\"bothSides\"/>") !=
                  std::string::npos &&
              xml.find("<wp:wrapTopAndBottom/>") != std::string::npos &&
              xml.find("<wp:positionH relativeFrom=\"page\"><wp:posOffset>0</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"page\">") !=
                  std::string::npos &&
              xml.find("<wp:positionH relativeFrom=\"character\"><wp:posOffset>0</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"paragraph\">") !=
                  std::string::npos &&
              xml.find("descr=\"Square &amp; &lt;owl&gt;\"") !=
                  std::string::npos,
          "authored picture placement, distances, or alt text is incomplete");

    ooxml::Error error;
    auto reopened = ooxml::DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->paragraphs().size() == 1 &&
              reopened->paragraphs()[0].runs.size() == 3,
          "picture-layout run order changed after reopen");
    const auto& imported_inline =
        *reopened->paragraphs()[0].runs[0].fragments[0].inline_image;
    const auto& imported_square =
        *reopened->paragraphs()[0].runs[1].fragments[0].inline_image;
    const auto& imported_top_bottom =
        *reopened->paragraphs()[0].runs[2].fragments[0].inline_image;
    check(imported_inline.accessible_name == "Inline description" &&
              imported_inline.layout == ooxml::ImageLayout{
                  ooxml::ImagePlacement::inline_with_text, 11, 22, 33,
                  44, true} &&
              imported_square.accessible_name == "Square & <owl>" &&
              imported_square.layout == ooxml::ImageLayout{
                  ooxml::ImagePlacement::square, 101, 202, 303, 404,
                  false} &&
              imported_top_bottom.accessible_name ==
                  "Top and bottom description" &&
              imported_top_bottom.layout == ooxml::ImageLayout{
                  ooxml::ImagePlacement::top_and_bottom, 505, 606, 707,
                  808, true},
          "picture placement, distances, move-with-text, or alt text changed after reopen");
    check(reopened->compatibility().classification ==
              ooxml::CompatibilityClass::basic_body_text_patch,
          "canonical picture anchors were classified as unsupported");
}

void expectRejectedImage(
    const TemporaryDirectory& temporary, std::string_view filename,
    ooxml::NewInlineImage payload) {
    const auto path = temporary.file(filename);
    ooxml::NewDocumentBody body;
    ooxml::NewParagraph paragraph;
    paragraph.runs.push_back(imageRun(std::move(payload)));
    body.blocks.emplace_back(std::move(paragraph));
    const auto save = ooxml::DocxDocument::writeNew(path, body);
    check(!save.saved && save.error.has_value() &&
              save.error->code == ooxml::ErrorCode::unsafe_edit &&
              save.loss_report.hasBlockers(),
          "unsafe authored image was accepted");
    check(!std::filesystem::exists(path),
          "rejected image left a destination package behind");
}

void testUnsafeAuthoredImagesAreRejected(
    const TemporaryDirectory& temporary) {
    expectRejectedImage(
        temporary, "path-name.docx",
        image(ooxml::RasterImageFormat::png, "../outside.png", 100, 100,
              onePixelPng()));
    expectRejectedImage(
        temporary, "zero-extent.docx",
        image(ooxml::RasterImageFormat::png, "Owl", 0, 100,
              onePixelPng()));
    expectRejectedImage(
        temporary, "mismatched-bytes.docx",
        image(ooxml::RasterImageFormat::jpeg, "Owl", 100, 100,
              onePixelPng()));
    expectRejectedImage(
        temporary, "unsupported-kind.docx",
        image(static_cast<ooxml::RasterImageFormat>(99), "Owl", 100, 100,
              onePixelPng()));

    auto negative_wrap = image(
        ooxml::RasterImageFormat::png, "Owl", 100, 100,
        onePixelPng());
    negative_wrap.layout = {
        ooxml::ImagePlacement::square, 0, 0, 0, -1, true};
    expectRejectedImage(
        temporary, "negative-wrap.docx", std::move(negative_wrap));
    auto oversized_wrap = image(
        ooxml::RasterImageFormat::png, "Owl", 100, 100,
        onePixelPng());
    oversized_wrap.layout = {
        ooxml::ImagePlacement::top_and_bottom,
        ooxml::kMaximumImageWrapDistanceEmu + 1, 0, 0, 0, true};
    expectRejectedImage(
        temporary, "oversized-wrap.docx", std::move(oversized_wrap));
    auto fixed_inline = image(
        ooxml::RasterImageFormat::png, "Owl", 100, 100,
        onePixelPng());
    fixed_inline.layout.move_with_text = false;
    expectRejectedImage(
        temporary, "fixed-inline.docx", std::move(fixed_inline));
    auto unknown_placement = image(
        ooxml::RasterImageFormat::png, "Owl", 100, 100,
        onePixelPng());
    unknown_placement.layout.placement =
        static_cast<ooxml::ImagePlacement>(255);
    expectRejectedImage(
        temporary, "unknown-placement.docx",
        std::move(unknown_placement));
    auto oversized_anchor = image(
        ooxml::RasterImageFormat::png, "Owl",
        ooxml::kMaximumImageDimensionEmu + 1, 100, onePixelPng());
    oversized_anchor.layout.placement = ooxml::ImagePlacement::square;
    expectRejectedImage(
        temporary, "oversized-anchor.docx",
        std::move(oversized_anchor));
    auto oversized_alt = image(
        ooxml::RasterImageFormat::png, "Owl", 100, 100,
        onePixelPng());
    oversized_alt.accessible_name.assign(
        ooxml::kMaximumImageAccessibleNameBytes + 1U, 'a');
    expectRejectedImage(
        temporary, "oversized-alt.docx", std::move(oversized_alt));

    auto truncated_png = onePixelPng();
    truncated_png.pop_back();
    expectRejectedImage(
        temporary, "truncated-png.docx",
        image(ooxml::RasterImageFormat::png, "Owl", 100, 100,
              std::move(truncated_png)));
    auto corrupt_png = onePixelPng();
    check(corrupt_png.size() > 29U,
          "PNG fixture is too short for CRC corruption test");
    corrupt_png[29U] ^= 0x01U;
    expectRejectedImage(
        temporary, "corrupt-png.docx",
        image(ooxml::RasterImageFormat::png, "Owl", 100, 100,
              std::move(corrupt_png)));

    auto truncated_jpeg = onePixelJpeg();
    truncated_jpeg.pop_back();
    expectRejectedImage(
        temporary, "truncated-jpeg.docx",
        image(ooxml::RasterImageFormat::jpeg, "Owl", 100, 100,
              std::move(truncated_jpeg)));
    auto corrupt_jpeg = onePixelJpeg();
    check(corrupt_jpeg.size() > 5U && corrupt_jpeg[2U] == 0xffU,
          "JPEG fixture is too short for segment corruption test");
    corrupt_jpeg[4U] = 0U;
    corrupt_jpeg[5U] = 1U;
    expectRejectedImage(
        temporary, "corrupt-jpeg.docx",
        image(ooxml::RasterImageFormat::jpeg, "Owl", 100, 100,
              std::move(corrupt_jpeg)));

    auto oversized = onePixelPng();
    oversized.resize(ooxml::NewInlineImage::maximum_encoded_bytes + 1U);
    expectRejectedImage(
        temporary, "oversized.docx",
        image(ooxml::RasterImageFormat::png, "Owl", 100, 100,
              std::move(oversized)));

    ooxml::NewRun mixed{
        "text", {}, std::nullopt,
        image(ooxml::RasterImageFormat::png, "Owl", 100, 100,
              onePixelPng())};
    const auto mixed_path = temporary.file("mixed-run.docx");
    const auto mixed_save = ooxml::DocxDocument::writeNew(
        mixed_path, {ooxml::NewParagraph{{std::move(mixed)}}});
    check(!mixed_save.saved && mixed_save.loss_report.hasBlockers(),
          "a text-and-image run was accepted");
}

ooxml::NewSection section(
    std::string text, const ooxml::PageSettings& page,
    ooxml::SectionBreakKind break_kind) {
    ooxml::NewSection result;
    result.body.blocks.emplace_back(
        ooxml::NewParagraph{{ooxml::NewRun{std::move(text), {}}}});
    result.page = page;
    result.break_kind = break_kind;
    return result;
}

void testOrderedNativeSections(const TemporaryDirectory& temporary) {
    ooxml::PageSettings first_page;
    first_page.width_twips = 15840;
    first_page.height_twips = 12240;
    first_page.margin_left_twips = 720;
    first_page.margin_right_twips = 1080;
    ooxml::PageSettings second_page;
    second_page.margin_top_twips = 900;
    second_page.margin_right_twips = 1000;
    second_page.margin_bottom_twips = 1100;
    second_page.margin_left_twips = 1200;

    ooxml::NewSectionedDocumentBody body{{
        section("First section", first_page,
                ooxml::SectionBreakKind::continuous),
        section("Second section", second_page,
                ooxml::SectionBreakKind::odd_page),
    }};
    const auto path = temporary.file("two-sections.docx");
    const auto copy = temporary.file("two-sections-copy.docx");
    const auto save = ooxml::DocxDocument::writeNew(path, body);
    check(save.saved,
          save.error ? save.error->message : "sectioned DOCX was not saved");

    const auto xml = readTextMember(path, "word/document.xml");
    const auto first_section = xml.find(
        "<w:pPr><w:sectPr><w:type w:val=\"continuous\"/>");
    const auto second_section = xml.find(
        "<w:sectPr><w:type w:val=\"oddPage\"/>",
        first_section == std::string::npos ? 0U : first_section + 1U);
    check(first_section != std::string::npos &&
              second_section != std::string::npos &&
              first_section < xml.find("</w:p>", first_section) &&
              second_section > xml.rfind("</w:p>"),
          "sectPr nodes are not in native paragraph/body positions");
    const auto first_section_end = xml.find("</w:sectPr>", first_section);
    check(first_section_end != std::string::npos &&
              xml.substr(first_section,
                         first_section_end - first_section)
                      .find(
                          "w:w=\"15840\" w:h=\"12240\" "
                          "w:orient=\"landscape\"") != std::string::npos,
          "non-final section page geometry or landscape orientation is absent");

    ooxml::Error error;
    auto reopened = ooxml::DocxDocument::open(path, &error);
    check(reopened != nullptr, error.message);
    check(reopened->bodyBlocks().size() == 2 &&
              reopened->paragraphs().size() == 2 &&
              reopened->paragraphs()[0].plainText() == "First section" &&
              reopened->paragraphs()[1].plainText() == "Second section",
          "section body order changed after reopen");
    check(reopened->sections().size() == 2,
          "ordered semantic section list was not imported");
    check(reopened->sections()[0] == ooxml::ImportedSection{
              0, 1, first_page, ooxml::SectionBreakKind::continuous} &&
              reopened->sections()[1] == ooxml::ImportedSection{
                  1, 1, second_page, ooxml::SectionBreakKind::odd_page},
          "section ranges, pages, or break kinds changed after reopen");
    check(reopened->paragraphs()[0].format_is_basic,
          "writer-emitted non-final landscape sectPr was not basic on reopen");
    check(reopened->bodyPageSettings().has_value() &&
              *reopened->bodyPageSettings() == second_page,
          "legacy final-section page API changed semantics");
    check(reopened->compatibility().classification ==
              ooxml::CompatibilityClass::basic_body_text_patch,
          "authored basic sections were classified as unsupported");

    const auto original = readFile(path);
    const auto copied = reopened->saveAs(copy);
    check(copied.saved && copied.byte_identical_to_opened_file &&
              readFile(copy) == original,
          "unchanged multi-section Save As was not byte-identical");
}

void testUnsafeSectionShapesAreRejected(
    const TemporaryDirectory& temporary) {
    const auto one_path = temporary.file("one-section.docx");
    ooxml::NewSectionedDocumentBody one{{section(
        "Only", {}, ooxml::SectionBreakKind::next_page)}};
    const auto one_save = ooxml::DocxDocument::writeNew(one_path, one);
    check(!one_save.saved && one_save.loss_report.hasBlockers(),
          "multi-section API accepted only one section");

    ooxml::NewSection first;
    first.body.blocks.emplace_back(ooxml::NewTable{1, 1, false, {"cell"}});
    ooxml::NewSection second = section(
        "Second", {}, ooxml::SectionBreakKind::next_page);
    const auto table_path = temporary.file("table-boundary.docx");
    const auto table_save = ooxml::DocxDocument::writeNew(
        table_path, ooxml::NewSectionedDocumentBody{{first, second}});
    check(!table_save.saved && table_save.loss_report.hasBlockers(),
          "non-final table section boundary was guessed or flattened");
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        testNativeInlinePngAndJpeg(temporary);
        testNativePictureLayoutsAndAltText(temporary);
        testUnsafeAuthoredImagesAreRejected(temporary);
        testOrderedNativeSections(temporary);
        testUnsafeSectionShapesAreRejected(temporary);
        std::cout << "M0 feature round-trip tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "M0 feature round-trip failure: " << exception.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
