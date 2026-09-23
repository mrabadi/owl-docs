#include "docxstudio/app/ChatStore.h"
#include "docxstudio/app/RecoveryCodec.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

constexpr std::string_view kOnePixelPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
    "+A8AAQUBAScY42YAAAAASUVORK5CYII=";

std::vector<std::uint8_t> onePixelPng() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
        0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x08, 0x04, 0x00, 0x00, 0x00, 0xb5,
        0x1c, 0x0c, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01, 0x05,
        0x01, 0x01, 0x27, 0x18, 0xe3, 0x66, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<std::uint8_t> paddedOnePixelJpeg(std::size_t targetBytes) {
    static constexpr std::uint8_t suffix[] = {
        // One-component, one-pixel baseline frame.
        0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00, 0x01,
        0x01, 0x01, 0x11, 0x00,
        // One-component scan, one entropy byte, then EOI.
        0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f,
        0x00, 0x00, 0xff, 0xd9,
    };
    constexpr std::size_t minimumBytes = 2U + std::size(suffix);
    check(targetBytes >= minimumBytes,
          "padded JPEG target is below the structural minimum");

    std::vector<std::uint8_t> bytes;
    bytes.reserve(targetBytes);
    bytes.push_back(0xff);
    bytes.push_back(0xd8);
    std::size_t padding = targetBytes - minimumBytes;
    while (padding != 0U) {
        check(padding >= 4U,
              "padded JPEG cannot encode a sub-four-byte remainder");
        std::size_t segmentBytes = std::min<std::size_t>(padding, 65'537U);
        const std::size_t remainder = padding - segmentBytes;
        if (remainder != 0U && remainder < 4U) {
            segmentBytes -= 4U - remainder;
        }
        const auto encodedLength = static_cast<std::uint16_t>(
            segmentBytes - 2U);
        bytes.push_back(0xff);
        bytes.push_back(0xfe);
        bytes.push_back(static_cast<std::uint8_t>(encodedLength >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(encodedLength & 0xffU));
        bytes.insert(bytes.end(), segmentBytes - 4U, 0U);
        padding -= segmentBytes;
    }
    bytes.insert(bytes.end(), std::begin(suffix), std::end(suffix));
    check(bytes.size() == targetBytes,
          "padded JPEG builder produced the wrong byte count");
    return bytes;
}

docxstudio::core::Document formattedDocument() {
    using namespace docxstudio::core;
    auto first = Paragraph::create(u"Hello \U0001f680 world", NodeId{1, 2});
    auto second = Paragraph::create(u"Second paragraph", NodeId{3, 4});
    check(static_cast<bool>(first) && static_cast<bool>(second),
          "could not create recovery fixture paragraphs");
    std::vector<Paragraph> paragraphs;
    paragraphs.push_back(std::move(first.value()));
    paragraphs.push_back(std::move(second.value()));
    auto document = Document::create(std::move(paragraphs));
    check(static_cast<bool>(document), "could not create recovery fixture document");

    CharacterFormatDelta character;
    character.font_family = PropertyDelta<std::string>::set("Carlito");
    character.font_size_half_points = PropertyDelta<std::int32_t>::set(25);
    character.bold = PropertyDelta<bool>::set(true);
    character.italic = PropertyDelta<bool>::set(false);
    character.underline = PropertyDelta<UnderlineStyle>::set(UnderlineStyle::wavy);
    character.strike = PropertyDelta<bool>::set(false);
    character.foreground_argb = PropertyDelta<std::uint32_t>::set(0xff123456U);
    character.highlight_argb = PropertyDelta<std::uint32_t>::set(0xffffff00U);
    character.baseline = PropertyDelta<BaselinePosition>::set(
        BaselinePosition::superscript);
    character.language = PropertyDelta<std::string>::set("en-US");
    auto characterResult = document.value().applyCharacterFormat(
        {{NodeId{1, 2}, 6}, {NodeId{1, 2}, 8}}, character);
    check(static_cast<bool>(characterResult), "could not format recovery fixture text");

    ParagraphFormatDelta paragraph;
    paragraph.alignment = PropertyDelta<ParagraphAlignment>::set(
        ParagraphAlignment::justified);
    paragraph.left_indent_emu = PropertyDelta<std::int64_t>::set(457200);
    paragraph.right_indent_emu = PropertyDelta<std::int64_t>::set(91440);
    paragraph.first_line_indent_emu = PropertyDelta<std::int64_t>::set(-114300);
    paragraph.space_before_emu = PropertyDelta<std::int64_t>::set(12700);
    paragraph.space_after_emu = PropertyDelta<std::int64_t>::set(101600);
    paragraph.line_spacing_emu = PropertyDelta<std::int64_t>::set(175260);
    paragraph.line_spacing_rule = PropertyDelta<LineSpacingRule>::set(
        LineSpacingRule::at_least);
    paragraph.keep_with_next = PropertyDelta<bool>::set(true);
    paragraph.keep_lines = PropertyDelta<bool>::set(false);
    paragraph.page_break_before = PropertyDelta<bool>::set(true);
    ListLayout listLayout;
    listLayout.levels[6].bullet_indent_spaces = 31;
    listLayout.levels[6].text_indent_spaces = 3;
    paragraph.list_id = PropertyDelta<NodeId>::set(NodeId{21, 22});
    paragraph.list_level = PropertyDelta<std::uint8_t>::set(std::uint8_t{6});
    paragraph.list_layout = PropertyDelta<ListLayout>::set(listLayout);
    auto paragraphResult = document.value().applyParagraphFormat({NodeId{3, 4}}, paragraph);
    check(static_cast<bool>(paragraphResult),
          "could not format recovery fixture paragraph");

    const auto equationResult = document.value().insertEquation(
        {NodeId{1, 2}, 5}, "\\frac{a}{b}", true, NodeId{5, 6});
    check(static_cast<bool>(equationResult),
          "could not insert recovery fixture equation");

    CharacterFormat tableHeaderCharacter;
    tableHeaderCharacter.font_family = "Liberation Sans";
    tableHeaderCharacter.font_size_half_points = 22;
    tableHeaderCharacter.bold = true;
    tableHeaderCharacter.foreground_argb = 0xffffffffU;
    ParagraphFormat tableHeaderParagraph;
    tableHeaderParagraph.alignment = ParagraphAlignment::center;
    tableHeaderParagraph.space_after_emu = 0;
    CharacterFormat tableBodyCharacter;
    tableBodyCharacter.italic = true;
    tableBodyCharacter.foreground_argb = 0xff4f2064U;
    ParagraphFormat tableBodyParagraph;
    tableBodyParagraph.alignment = ParagraphAlignment::right;
    tableBodyParagraph.line_spacing_emu = 152400;
    tableBodyParagraph.line_spacing_rule = LineSpacingRule::automatic;
    auto firstTable = Table::restore(
        2, 2, true, NodeId{7, 8},
        {TableCell{NodeId{9, 10}, u"Name",
                   {{0, 4, tableHeaderCharacter}}, tableHeaderParagraph},
         TableCell{NodeId{11, 12}, u"Value",
                   {{0, 5, tableHeaderCharacter}}, tableHeaderParagraph},
         TableCell{NodeId{13, 14}, u"Alpha",
                   {{1, 4, tableBodyCharacter}}, tableBodyParagraph},
         TableCell{NodeId{15, 16}, u"One\u2028line"}},
        TableStyle::banded_aubergine);
    auto secondTable = Table::restore(
        1, 1, false, NodeId{17, 18},
        {TableCell{NodeId{19, 20}, u"", {}, {}, tableBodyCharacter}},
        std::nullopt);
    check(firstTable && secondTable,
          "could not create recovery fixture tables");
    check(static_cast<bool>(document.value().insertTable(
              NodeId{3, 4}, std::move(firstTable.value()))) &&
              static_cast<bool>(document.value().insertTable(
                  std::nullopt, std::move(secondTable.value()))) &&
              static_cast<bool>(document.value().moveTable(
                  NodeId{17, 18}, NodeId{7, 8})),
          "could not arrange recovery fixture tables");
    check(static_cast<bool>(document.value().setHeaderText(u"Owl Docs")) &&
              static_cast<bool>(document.value().setFooterText(
                  u"Page {PAGE} of {PAGES}")),
          "could not set recovery fixture header and footer");
    return std::move(document.value());
}

void codecRoundTrip() {
    using namespace docxstudio::app;
    const auto original = formattedDocument();
    const RecoveryPageLayout page{595.28, 841.89, 54.0, 63.0, 72.0, 81.0};
    std::string error;
    const auto encoded = RecoveryCodec::encode({original, page}, error);
    check(encoded.has_value(), "recovery codec did not encode a valid document");
    const auto decoded = RecoveryCodec::decode(*encoded, error);
    check(decoded.has_value(), "recovery codec did not decode its own payload");
    check(decoded->document == original,
          "recovery codec did not preserve document text, IDs, or formatting");
    check(decoded->page == page, "recovery codec did not preserve page layout");

    const auto encodedJson = nlohmann::json::parse(*encoded);
    check(encodedJson["tables"][0]["style"] == "banded-aubergine" &&
              encodedJson["tables"][1]["style"].is_null(),
          "encoded recovery omitted an explicit or inherited table style");
    check(encodedJson["tables"][0]["cells"][0]["runs"].size() == 1 &&
              encodedJson["tables"][0]["cells"][0]["format"]["alignment"] ==
                  "center" &&
              encodedJson["tables"][1]["cells"][0]
                         ["default_character_format"]["italic"] == true,
          "encoded recovery omitted table-cell formatting");

    auto legacyJson = encodedJson;
    legacyJson["version"] = 3;
    for (auto& table : legacyJson["tables"]) {
        table.erase("style");
        for (auto& cell : table["cells"]) {
            cell.erase("format");
            cell.erase("default_character_format");
            cell.erase("runs");
        }
    }
    const auto legacyDecoded = RecoveryCodec::decode(legacyJson.dump(), error);
    check(legacyDecoded.has_value(),
          "recovery codec did not decode a version-3 table payload");
    check(legacyDecoded->document.tables().size() == 2 &&
              legacyDecoded->document.tables()[0].style() ==
                  docxstudio::core::TableStyle::grid &&
              legacyDecoded->document.tables()[1].style() ==
                  docxstudio::core::TableStyle::grid &&
              legacyDecoded->document.tables()[0].cells()[0]
                  .character_formats.empty() &&
              legacyDecoded->document.tables()[0].cells()[0]
                  .paragraph_format.empty() &&
              legacyDecoded->document.tables()[1].cells()[0]
                  .default_character_format.empty(),
          "version-3 recovery defaults were not preserved");

    auto missingDefaultFormat = encodedJson;
    missingDefaultFormat["tables"][1]["cells"][0].erase(
        "default_character_format");
    const auto missingDefaultDecoded =
        RecoveryCodec::decode(missingDefaultFormat.dump(), error);
    check(missingDefaultDecoded.has_value() &&
              missingDefaultDecoded->document.tables()[1].cells()[0]
                  .default_character_format.empty(),
          "missing table-cell default format did not decode as empty");

    auto invalidStyle = encodedJson;
    invalidStyle["tables"][0]["style"] = "future-style";
    check(!RecoveryCodec::decode(invalidStyle.dump(), error),
          "recovery codec accepted an unknown table style");

    auto invalidCellRun = encodedJson;
    invalidCellRun["tables"][0]["cells"][0]["runs"][0]["end"] = 400;
    check(!RecoveryCodec::decode(invalidCellRun.dump(), error),
          "recovery codec accepted an out-of-range table-cell format run");

    auto futureVersion = *encoded;
    check(encoded->find("\"list_layout\"") != std::string::npos,
          "encoded recovery omitted semantic list layout");

    auto invalidLevel = *encoded;
    const auto listLevel = invalidLevel.find("\"list_level\":6");
    check(listLevel != std::string::npos, "encoded recovery list level was absent");
    invalidLevel.replace(listLevel, std::string("\"list_level\":6").size(),
                         "\"list_level\":10");
    check(!RecoveryCodec::decode(invalidLevel, error),
          "recovery codec accepted an out-of-range list level");

    auto invalidIndent = *encoded;
    const auto bulletIndent = invalidIndent.find("\"bullet_indent_spaces\":31");
    check(bulletIndent != std::string::npos,
          "encoded recovery custom bullet indentation was absent");
    invalidIndent.replace(
        bulletIndent, std::string("\"bullet_indent_spaces\":31").size(),
        "\"bullet_indent_spaces\":401");
    check(!RecoveryCodec::decode(invalidIndent, error),
          "recovery codec accepted an out-of-range bullet indentation");

    const auto version = futureVersion.find("\"version\":13");
    check(version != std::string::npos, "encoded recovery version was absent");
    futureVersion.replace(version, std::string("\"version\":13").size(),
                          "\"version\":14");
    check(!RecoveryCodec::decode(futureVersion, error),
          "recovery codec accepted an unsupported future version");
    check(!RecoveryCodec::decode("{not-json", error),
          "recovery codec accepted malformed JSON");
}

void frozenVersion4FixtureAndPreflight() {
    using namespace docxstudio::app;

    // This is the minimal shape emitted by the version-4 encoder preserved in
    // repository history. Older v1-v3 encoder implementations are not present
    // in that history, so this test intentionally does not invent their shapes.
    constexpr std::string_view frozenVersion4 = R"json({
        "schema":"docxstudio.recovery",
        "version":4,
        "page":{
            "width_points":612.0,
            "height_points":792.0,
            "margin_top_points":72.0,
            "margin_right_points":72.0,
            "margin_bottom_points":72.0,
            "margin_left_points":72.0
        },
        "paragraphs":[{
            "id":"00000000-0000-0001-0000-000000000002",
            "text_utf16":[65],
            "format":{},
            "runs":[],
            "equations":[]
        }],
        "tables":[],
        "body_blocks":[{
            "kind":"paragraph",
            "id":"00000000-0000-0001-0000-000000000002"
        }]
    })json";

    std::string error;
    const auto restored = RecoveryCodec::decode(frozenVersion4, error);
    check(restored.has_value() &&
              restored->document.paragraphs().size() == 1 &&
              restored->document.paragraphs().front().id() ==
                  docxstudio::core::NodeId{1, 2} &&
              restored->document.paragraphs().front().text() == u"A",
          "frozen version-4 recovery fixture did not decode");

    auto deeplyNested = nlohmann::json::parse(frozenVersion4);
    nlohmann::json nested = 0;
    for (std::size_t depth = 0; depth < 65U; ++depth) {
        nested = nlohmann::json::array({std::move(nested)});
    }
    deeplyNested["ignored_extension"] = std::move(nested);
    check(!RecoveryCodec::decode(deeplyNested.dump(), error) &&
              error.find("nesting") != std::string::npos,
          "recovery preflight accepted excessive JSON nesting");
}

void semanticImageRoundTrip() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    auto paragraph = Paragraph::create(u"AB", NodeId{101, 102});
    check(static_cast<bool>(paragraph),
          "could not create semantic image recovery paragraph");
    auto document = Document::create({std::move(paragraph.value())});
    check(static_cast<bool>(document),
          "could not create semantic image recovery document");

    const auto payload = EncodedImagePayload(onePixelPng());
    const NodeId equationId{103, 104};
    const NodeId firstImageId{105, 106};
    const NodeId secondImageId{107, 108};
    const ImageLayout squareLayout{
        ImagePlacement::square, 12, 34, 56, 78, false};
    const ImageLayout topBottomLayout{
        ImagePlacement::top_and_bottom, 90, 0, 123, 0, true};
    check(static_cast<bool>(document.value().insertEquation(
              {{101, 102}, 1}, "\\frac{x}{y}", false, equationId)) &&
              static_cast<bool>(document.value().insertImage(
                  {{101, 102}, 1}, payload, ImageFormat::png,
                  "First owl", 914400, 457200, firstImageId, std::nullopt,
                  squareLayout)) &&
              static_cast<bool>(document.value().insertImage(
                  {{101, 102}, 3}, payload, ImageFormat::png,
                  "Second owl", 457200, 914400, secondImageId, std::nullopt,
                  topBottomLayout)),
          "could not build mixed image/equation recovery fixture");
    check(document.value().paragraphs().front().text() ==
              u"A\ufffc\ufffc\ufffcB" &&
              document.value().paragraphs().front().imageAt(1)->id ==
                  firstImageId &&
              document.value().paragraphs().front().equationAt(2)->id ==
                  equationId &&
              document.value().paragraphs().front().imageAt(3)->id ==
                  secondImageId,
          "mixed recovery fixture did not have the intended source order");

    CharacterFormatDelta emphasis;
    emphasis.bold = PropertyDelta<bool>::set(true);
    check(static_cast<bool>(document.value().applyCharacterFormat(
              {{{101, 102}, 1}, {{101, 102}, 4}}, emphasis)),
          "could not format mixed recovery fixture");

    std::string error;
    const RecoveryPageLayout page;
    const auto encoded = RecoveryCodec::encode(
        {document.value(), page}, error);
    check(encoded.has_value(),
          "recovery codec rejected semantic inline images");
    const auto encodedJson = nlohmann::json::parse(*encoded);
    check(encodedJson["version"] == RecoveryCodec::currentVersion &&
              !encodedJson.contains("inline_images") &&
              encodedJson["paragraphs"][0]["images"].size() == 2 &&
              encodedJson["paragraphs"][0]["images"][0]["layout"]
                         ["placement"] == "square" &&
              encodedJson["paragraphs"][0]["images"][0]["layout"]
                         ["distance_left_emu"] == 78 &&
              encodedJson["paragraphs"][0]["images"][0]["layout"]
                         ["move_with_text"] == false &&
              encodedJson["paragraphs"][0]["images"][1]["layout"]
                         ["placement"] == "top-and-bottom",
          "recovery codec did not write the semantic image schema");

    const auto decoded = RecoveryCodec::decode(*encoded, error);
    check(decoded && decoded->document == document.value(),
          "semantic image/equation order or payload changed on recovery");
    const auto recoveredBytes =
        decoded->document.findImage(firstImageId)->encoded_payload.bytes();
    check(recoveredBytes.size() == payload.bytes().size() &&
              std::equal(recoveredBytes.begin(), recoveredBytes.end(),
                         payload.bytes().begin()) &&
              decoded->document.findImage(secondImageId)->width_emu ==
                  457200 &&
              decoded->document.findImage(firstImageId)->layout ==
                  squareLayout &&
              decoded->document.findImage(secondImageId)->layout ==
                  topBottomLayout,
          "semantic recovery did not preserve image bytes, geometry, or layout");

    auto version6 = encodedJson;
    version6["version"] = 6;
    for (auto& encodedImage : version6["paragraphs"][0]["images"]) {
        encodedImage.erase("layout");
    }
    const auto migratedVersion6 = RecoveryCodec::decode(version6.dump(), error);
    check(migratedVersion6 &&
              migratedVersion6->document.findImage(firstImageId)->layout ==
                  ImageLayout{} &&
              migratedVersion6->document.findImage(secondImageId)->layout ==
                  ImageLayout{},
          "version-6 semantic images did not migrate to default layout");
}

void paragraphMarkCharacterFormatRoundTrip() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    CharacterFormat emptyMark;
    emptyMark.font_family = "Carlito";
    emptyMark.font_size_half_points = 27;
    emptyMark.bold = false;
    emptyMark.italic = true;
    emptyMark.underline = UnderlineStyle::wavy;
    emptyMark.strike = false;
    emptyMark.foreground_argb = 0xff123456U;
    emptyMark.highlight_argb = 0xfffedcbaU;
    emptyMark.baseline = BaselinePosition::subscript;
    emptyMark.language = "en-US";

    CharacterFormat nonemptyMark;
    nonemptyMark.bold = true;
    nonemptyMark.foreground_argb = 0xff654321U;

    auto empty = Paragraph::create({}, NodeId{601, 602}, emptyMark);
    auto nonempty = Paragraph::create(
        u"Body text", NodeId{603, 604}, nonemptyMark);
    check(empty && nonempty,
          "could not create paragraph-mark recovery fixture");
    auto document = Document::create(
        {std::move(empty.value()), std::move(nonempty.value())});
    check(static_cast<bool>(document),
          "could not create paragraph-mark recovery document");

    std::string error;
    const auto encoded = RecoveryCodec::encode(
        {document.value(), {}}, error);
    check(encoded.has_value(),
          "recovery codec did not encode paragraph-mark formatting");
    const auto encodedJson = nlohmann::json::parse(*encoded);
    check(encodedJson["version"] == RecoveryCodec::currentVersion &&
              encodedJson["paragraphs"][0]
                         ["paragraph_mark_character_format"]
                         ["font_family"] == "Carlito" &&
              encodedJson["paragraphs"][0]
                         ["paragraph_mark_character_format"]
                         ["bold"] == false &&
              encodedJson["paragraphs"][0]
                         ["paragraph_mark_character_format"]
                         ["underline"] == "wavy" &&
              encodedJson["paragraphs"][1]
                         ["paragraph_mark_character_format"]
                         ["foreground_argb"] == 0xff654321U,
          "recovery JSON omitted paragraph-mark character properties");

    const auto decoded = RecoveryCodec::decode(*encoded, error);
    check(decoded && decoded->document == document.value() &&
              decoded->document.paragraphs()[0].characterFormatAt(0) ==
                  emptyMark &&
              decoded->document.paragraphs()[1]
                      .paragraphMarkCharacterFormat() == nonemptyMark,
          "recovery round trip changed paragraph-mark formatting");

    auto version7 = encodedJson;
    version7["version"] = 7;
    for (auto& paragraph : version7["paragraphs"]) {
        paragraph.erase("paragraph_mark_character_format");
    }
    const auto migrated = RecoveryCodec::decode(version7.dump(), error);
    check(migrated &&
              migrated->document.paragraphs()[0]
                  .paragraphMarkCharacterFormat().empty() &&
              migrated->document.paragraphs()[1]
                  .paragraphMarkCharacterFormat().empty(),
          "version-7 paragraphs did not migrate to an empty mark format");

    auto missingCurrent = encodedJson;
    missingCurrent["paragraphs"][0].erase(
        "paragraph_mark_character_format");
    const auto missing = RecoveryCodec::decode(
        missingCurrent.dump(), error);
    check(missing &&
              missing->document.paragraphs()[0]
                  .paragraphMarkCharacterFormat().empty() &&
              missing->document.paragraphs()[1]
                      .paragraphMarkCharacterFormat() == nonemptyMark,
          "missing paragraph-mark recovery data did not default safely");

    auto invalid = encodedJson;
    invalid["paragraphs"][0]["paragraph_mark_character_format"]
           ["font_size_half_points"] = 0;
    check(!RecoveryCodec::decode(invalid.dump(), error),
          "recovery codec accepted an invalid paragraph-mark format");
}

void paragraphStyleIdentityRoundTrip() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    ParagraphStyleProvenance knownProvenance;
    knownProvenance.inherited_character_format.foreground_argb =
        0xff336699U;
    knownProvenance.inherited_paragraph_mark_character_format =
        knownProvenance.inherited_character_format;
    knownProvenance.inherited_paragraph_mark_character_format.italic = true;
    knownProvenance.inherited_paragraph_format.alignment =
        ParagraphAlignment::center;
    CharacterFormatMask directRed;
    directRed.foreground_argb = true;
    knownProvenance.character_overrides.push_back({0, 8, directRed});
    knownProvenance.paragraph_overrides.alignment = true;
    auto known = Paragraph::restore(
        u"Built-in heading", NodeId{701, 702}, {},
        std::string("Heading2"), knownProvenance);
    auto custom = Paragraph::restore(
        u"Imported custom", NodeId{703, 704}, {},
        std::string("Firm.Custom-β"));
    auto unstyled = Paragraph::restore(
        u"No style identity", NodeId{705, 706}, {});
    check(known && custom && unstyled,
          "could not create paragraph-style recovery fixtures");
    auto document = Document::create(
        {std::move(known.value()), std::move(custom.value()),
         std::move(unstyled.value())});
    check(static_cast<bool>(document),
          "could not create paragraph-style recovery document");

    std::string error;
    const auto encoded = RecoveryCodec::encode(
        {document.value(), {}}, error);
    check(encoded.has_value(),
          "recovery codec did not encode paragraph-style identities");
    const auto encodedJson = nlohmann::json::parse(*encoded);
    check(encodedJson["version"] == RecoveryCodec::currentVersion &&
              encodedJson["paragraphs"][0]["style_id"] == "Heading2" &&
              encodedJson["paragraphs"][0]["style_provenance"]
                         ["inherited_character_format"]
                         ["foreground_argb"] == 0xff336699U &&
              encodedJson["paragraphs"][0]["style_provenance"]
                         ["inherited_paragraph_mark_character_format"]
                         ["italic"] == true &&
              encodedJson["paragraphs"][0]["style_provenance"]
                         ["character_overrides"][0]["properties"]
                         ["foreground_argb"] == true &&
              encodedJson["paragraphs"][1]["style_id"] ==
                  "Firm.Custom-β" &&
              !encodedJson["paragraphs"][2].contains("style_id"),
          "recovery JSON omitted or invented a paragraph-style identity");

    const auto decoded = RecoveryCodec::decode(*encoded, error);
    check(decoded && decoded->document == document.value() &&
              decoded->document.paragraphs()[0].styleId() ==
                  std::optional<std::string>("Heading2") &&
              decoded->document.paragraphs()[1].styleId() ==
                  std::optional<std::string>("Firm.Custom-β") &&
              !decoded->document.paragraphs()[2].styleId(),
          "recovery round trip changed paragraph-style identities");

    auto version10 = encodedJson;
    version10["version"] = 10;
    for (auto& paragraph : version10["paragraphs"]) {
        if (paragraph.contains("style_provenance")) {
            paragraph["style_provenance"].erase(
                "inherited_paragraph_mark_character_format");
        }
    }
    const auto migrated10 = RecoveryCodec::decode(version10.dump(), error);
    check(migrated10 &&
              migrated10->document.paragraphs()[0].styleProvenance() &&
              migrated10->document.paragraphs()[0].styleProvenance()
                      ->inherited_paragraph_mark_character_format ==
                  migrated10->document.paragraphs()[0].styleProvenance()
                      ->inherited_character_format,
          "version-10 style provenance did not migrate its paragraph-mark baseline safely");

    auto version9 = encodedJson;
    version9["version"] = 9;
    for (auto& paragraph : version9["paragraphs"]) {
        paragraph.erase("style_provenance");
    }
    const auto migrated9 = RecoveryCodec::decode(version9.dump(), error);
    check(migrated9 &&
              migrated9->document.paragraphs()[0].styleId() ==
                  std::optional<std::string>("Heading2") &&
              !migrated9->document.paragraphs()[0].styleProvenance(),
          "version-9 recovery data did not retain style identity without inventing provenance");

    auto version8 = encodedJson;
    version8["version"] = 8;
    for (auto& paragraph : version8["paragraphs"]) {
        paragraph.erase("style_id");
    }
    const auto migrated = RecoveryCodec::decode(version8.dump(), error);
    check(migrated &&
              !migrated->document.paragraphs()[0].styleId() &&
              !migrated->document.paragraphs()[1].styleId() &&
              !migrated->document.paragraphs()[2].styleId(),
          "version-8 recovery data without style IDs did not migrate safely");

    auto missingCurrent = encodedJson;
    missingCurrent["paragraphs"][0].erase("style_id");
    missingCurrent["paragraphs"][0].erase("style_provenance");
    const auto missing = RecoveryCodec::decode(
        missingCurrent.dump(), error);
    check(missing && !missing->document.paragraphs()[0].styleId() &&
              missing->document.paragraphs()[1].styleId() ==
                  std::optional<std::string>("Firm.Custom-β"),
          "missing current-version style ID did not default safely");

    auto orphanedProvenance = encodedJson;
    orphanedProvenance["paragraphs"][0].erase("style_id");
    check(!RecoveryCodec::decode(orphanedProvenance.dump(), error) &&
              error.find("requires a style ID") != std::string::npos,
          "recovery codec accepted style provenance without a style ID");

    auto missingMarkBaseline = encodedJson;
    missingMarkBaseline["paragraphs"][0]["style_provenance"].erase(
        "inherited_paragraph_mark_character_format");
    check(!RecoveryCodec::decode(missingMarkBaseline.dump(), error),
          "current recovery codec accepted provenance without a paragraph-mark baseline");

    auto invalidMarkBaseline = encodedJson;
    invalidMarkBaseline["paragraphs"][0]["style_provenance"]
                       ["inherited_paragraph_mark_character_format"]
                       ["font_size_half_points"] = 0;
    check(!RecoveryCodec::decode(invalidMarkBaseline.dump(), error),
          "recovery codec accepted an invalid inherited paragraph-mark baseline");

    auto wrongType = encodedJson;
    wrongType["paragraphs"][0]["style_id"] = 7;
    check(!RecoveryCodec::decode(wrongType.dump(), error),
          "recovery codec accepted a non-string paragraph-style ID");

    auto empty = encodedJson;
    empty["paragraphs"][0]["style_id"] = "";
    check(!RecoveryCodec::decode(empty.dump(), error),
          "recovery codec accepted an empty paragraph-style ID");

    auto control = encodedJson;
    control["paragraphs"][0]["style_id"] = "Heading\n2";
    check(!RecoveryCodec::decode(control.dump(), error),
          "recovery codec accepted a control character in a style ID");

    auto exactBoundary = encodedJson;
    exactBoundary["paragraphs"][0]["style_id"] =
        std::string(kMaximumParagraphStyleIdBytes, 'x');
    const auto exactBoundaryDecoded = RecoveryCodec::decode(
        exactBoundary.dump(), error);
    check(exactBoundaryDecoded &&
              exactBoundaryDecoded->document.paragraphs()[0].styleId() &&
              exactBoundaryDecoded->document.paragraphs()[0]
                      .styleId()->size() ==
                  kMaximumParagraphStyleIdBytes,
          "recovery codec rejected the exact paragraph-style ID limit");

    auto oversized = encodedJson;
    oversized["paragraphs"][0]["style_id"] =
        std::string(kMaximumParagraphStyleIdBytes + 1U, 'x');
    check(!RecoveryCodec::decode(oversized.dump(), error) &&
              error.find("size limit") != std::string::npos,
          "recovery codec accepted an oversized paragraph-style ID");
}

void legacyImageMigrationAndBoundaries() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    auto paragraph = Paragraph::create(
        u"A\U0001f600B", NodeId{201, 202});
    check(static_cast<bool>(paragraph),
          "could not create legacy image recovery paragraph");
    auto document = Document::create({std::move(paragraph.value())});
    check(static_cast<bool>(document),
          "could not create legacy image recovery document");
    const NodeId equationId{203, 204};
    check(static_cast<bool>(document.value().insertEquation(
              {{201, 202}, 3}, "x", false, equationId)),
          "could not create legacy mixed-object fixture");
    CharacterFormatDelta bold;
    bold.bold = PropertyDelta<bool>::set(true);
    CharacterFormatDelta italic;
    italic.italic = PropertyDelta<bool>::set(true);
    check(static_cast<bool>(document.value().applyCharacterFormat(
              {{{201, 202}, 1}, {{201, 202}, 3}}, bold)) &&
              static_cast<bool>(document.value().applyCharacterFormat(
                  {{{201, 202}, 3}, {{201, 202}, 4}}, italic)),
          "could not format legacy image-neighbor fixture");

    std::string error;
    const auto encoded = RecoveryCodec::encode(
        {document.value(), {}}, error);
    check(encoded.has_value(),
          "could not encode legacy migration base fixture");
    auto legacy = nlohmann::json::parse(*encoded);
    legacy["version"] = 5;
    for (auto& encodedParagraph : legacy["paragraphs"]) {
        encodedParagraph.erase("images");
    }
    const auto legacyImage = [](NodeId id, std::size_t offset,
                                std::string name) {
        return nlohmann::json{
            {"id", id.toString()},
            {"paragraph_id", NodeId{201, 202}.toString()},
            {"utf16_offset", offset},
            {"width_points", 72.0},
            {"height_points", 36.0},
            {"accessible_name", std::move(name)},
            {"format", "png"},
            {"encoded_base64", kOnePixelPngBase64},
        };
    };
    const NodeId beforeEmoji{205, 206};
    const NodeId beforeEquationFirst{207, 208};
    const NodeId beforeEquationSecond{209, 210};
    legacy["inline_images"] = nlohmann::json::array(
        {legacyImage(beforeEmoji, 1, "Before emoji"),
         legacyImage(beforeEquationFirst, 3, "First at tie"),
         legacyImage(beforeEquationSecond, 3, "Second at tie")});

    const auto migrated = RecoveryCodec::decode(legacy.dump(), error);
    check(migrated.has_value(),
          "recovery codec did not migrate a valid version-5 image journal");
    const auto& restored = migrated->document.paragraphs().front();
    check(restored.text() ==
              u"A\ufffc\U0001f600\ufffc\ufffc\ufffcB" &&
              restored.imageAt(1)->id == beforeEmoji &&
              restored.imageAt(4)->id == beforeEquationFirst &&
              restored.imageAt(5)->id == beforeEquationSecond &&
              restored.equationAt(6)->id == equationId,
          "legacy image anchors were not migrated in visual source order");
    check(restored.imageAt(1)->width_emu == 914400 &&
              restored.imageAt(1)->height_emu == 457200 &&
              restored.characterFormatAt(2).bold == true &&
              restored.characterFormatAt(5).italic == true &&
              restored.characterFormatAt(6).italic == true,
          "legacy image geometry or following-character formatting changed");

    auto splitSurrogate = legacy;
    splitSurrogate["inline_images"][0]["utf16_offset"] = 2;
    check(!RecoveryCodec::decode(splitSurrogate.dump(), error),
          "recovery codec accepted a legacy image anchor inside a surrogate pair");

    auto missingParagraph = legacy;
    missingParagraph["inline_images"][0]["paragraph_id"] =
        NodeId{999, 1000}.toString();
    check(!RecoveryCodec::decode(missingParagraph.dump(), error),
          "recovery codec accepted a legacy image with no paragraph");
}

void semanticImageAdversarialLimits() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    check(RecoveryCodec::currentVersion == 13,
          "recovery schema version was not bumped for header/footer pictures");
    check(kMaximumInlineImagesPerDocument == 512 &&
              kMaximumEncodedImageBytes == 16U * 1024U * 1024U &&
              kMaximumDocumentEncodedImageBytes == 32U * 1024U * 1024U &&
              kMaximumImageAccessibleNameBytes == 4U * 1024U,
          "recovery tests and the semantic core disagree on image budgets");

    auto paragraph = Paragraph::create(u"XY", NodeId{301, 302});
    check(static_cast<bool>(paragraph),
          "could not create image-limit recovery paragraph");
    auto document = Document::create({std::move(paragraph.value())});
    check(static_cast<bool>(document),
          "could not create image-limit recovery document");
    check(static_cast<bool>(document.value().insertEquation(
              {{301, 302}, 1}, "z", false, NodeId{303, 304})) &&
              static_cast<bool>(document.value().insertImage(
                  {{301, 302}, 2}, EncodedImagePayload(onePixelPng()),
                  ImageFormat::png, "image", 1000, 1000,
                  NodeId{305, 306})),
          "could not build image-limit recovery fixture");

    std::string error;
    const auto encoded = RecoveryCodec::encode(
        {document.value(), {}}, error);
    check(encoded.has_value(),
          "could not encode image-limit recovery fixture");
    const auto original = nlohmann::json::parse(*encoded);

    auto exactLayout = original;
    exactLayout["paragraphs"][0]["images"][0]["layout"] = {
        {"placement", "square"},
        {"distance_top_emu", kMaximumImageWrapDistanceEmu},
        {"distance_right_emu", 2},
        {"distance_bottom_emu", 3},
        {"distance_left_emu", 4},
        {"move_with_text", false}};
    const auto exactLayoutDecoded =
        RecoveryCodec::decode(exactLayout.dump(), error);
    check(exactLayoutDecoded &&
              exactLayoutDecoded->document.findImage(NodeId{305, 306})
                      ->layout.distance_top_emu ==
                  kMaximumImageWrapDistanceEmu,
          "recovery codec rejected the exact image-wrap-distance limit");

    auto partialLayout = original;
    partialLayout["paragraphs"][0]["images"][0]["layout"] = {
        {"placement", "top-and-bottom"}};
    const auto partialLayoutDecoded =
        RecoveryCodec::decode(partialLayout.dump(), error);
    check(partialLayoutDecoded &&
              partialLayoutDecoded->document.findImage(NodeId{305, 306})
                      ->layout ==
                  ImageLayout{ImagePlacement::top_and_bottom, 0, 0, 0, 0,
                              true},
          "missing recovery image-layout fields did not use safe defaults");

    auto unknownPlacement = original;
    unknownPlacement["paragraphs"][0]["images"][0]["layout"]
                    ["placement"] = "behind-text";
    check(!RecoveryCodec::decode(unknownPlacement.dump(), error),
          "recovery codec accepted an unknown image placement");

    auto negativeDistance = original;
    negativeDistance["paragraphs"][0]["images"][0]["layout"]
                    ["distance_left_emu"] = -1;
    check(!RecoveryCodec::decode(negativeDistance.dump(), error),
          "recovery codec accepted a negative image wrap distance");

    auto oversizedDistance = original;
    oversizedDistance["paragraphs"][0]["images"][0]["layout"]
                     ["distance_right_emu"] =
        kMaximumImageWrapDistanceEmu + 1;
    check(!RecoveryCodec::decode(oversizedDistance.dump(), error),
          "recovery codec accepted an oversized image wrap distance");

    auto fixedInline = original;
    fixedInline["paragraphs"][0]["images"][0]["layout"]
               ["move_with_text"] = false;
    check(!RecoveryCodec::decode(fixedInline.dump(), error),
          "recovery codec accepted a fixed-position inline image");

    auto malformedMove = original;
    malformedMove["paragraphs"][0]["images"][0]["layout"]
                 ["move_with_text"] = "true";
    check(!RecoveryCodec::decode(malformedMove.dump(), error),
          "recovery codec accepted a non-Boolean move-with-text value");

    auto exactName = original;
    exactName["paragraphs"][0]["images"][0]["accessible_name"] =
        std::string(kMaximumImageAccessibleNameBytes, 'a');
    check(RecoveryCodec::decode(exactName.dump(), error).has_value(),
          "recovery codec rejected the exact accessible-name limit");

    auto oversizedName = original;
    oversizedName["paragraphs"][0]["images"][0]["accessible_name"] =
        std::string(kMaximumImageAccessibleNameBytes + 1U, 'a');
    check(!RecoveryCodec::decode(oversizedName.dump(), error),
          "recovery codec accepted an oversized accessible name");

    auto collidingObjects = original;
    collidingObjects["paragraphs"][0]["images"][0]["offset"] = 1;
    check(!RecoveryCodec::decode(collidingObjects.dump(), error),
          "recovery codec accepted image and equation metadata for one placeholder");

    auto tooManyImages = original;
    const auto imageTemplate = tooManyImages["paragraphs"][0]["images"][0];
    tooManyImages["paragraphs"][0]["images"] = nlohmann::json::array();
    for (std::size_t index = 0;
         index <= kMaximumInlineImagesPerDocument; ++index) {
        tooManyImages["paragraphs"][0]["images"].push_back(imageTemplate);
    }
    check(!RecoveryCodec::decode(tooManyImages.dump(), error),
          "recovery codec accepted more than 512 semantic images");

    auto oversizedPayload = original;
    const std::size_t oversizedDecodedBytes =
        kMaximumEncodedImageBytes + 1U;
    const std::size_t oversizedBase64Bytes =
        ((oversizedDecodedBytes + 2U) / 3U) * 4U;
    oversizedPayload["paragraphs"][0]["images"][0]["encoded_base64"] =
        std::string(oversizedBase64Bytes, 'A');
    check(!RecoveryCodec::decode(oversizedPayload.dump(), error),
          "recovery codec accepted a payload above the per-image core limit");

    auto mismatchedFormat = original;
    mismatchedFormat["paragraphs"][0]["images"][0]["format"] = "jpeg";
    check(!RecoveryCodec::decode(mismatchedFormat.dump(), error),
          "recovery codec accepted PNG bytes labeled as JPEG");

    auto malformedBase64 = original;
    malformedBase64["paragraphs"][0]["images"][0]["encoded_base64"] =
        "AAAA=AAA";
    check(!RecoveryCodec::decode(malformedBase64.dump(), error),
          "recovery codec accepted interior base64 padding");

    auto noncanonicalBase64 = original;
    auto noncanonical = noncanonicalBase64["paragraphs"][0]["images"][0]
                            ["encoded_base64"]
                                .get<std::string>();
    check(noncanonical.size() >= 2U && noncanonical.back() == '=' &&
              noncanonical[noncanonical.size() - 2U] == 'I',
          "base64 fixture does not end in the expected padded quartet");
    // J differs from I only in unused pad bits. A permissive decoder would
    // produce the same bytes, while a canonical decoder must reject it.
    noncanonical[noncanonical.size() - 2U] = 'J';
    noncanonicalBase64["paragraphs"][0]["images"][0]["encoded_base64"] =
        std::move(noncanonical);
    check(!RecoveryCodec::decode(noncanonicalBase64.dump(), error),
          "recovery codec accepted noncanonical base64 pad bits");

    auto crossKindDuplicate = original;
    crossKindDuplicate["paragraphs"][0]["images"][0]["id"] =
        crossKindDuplicate["paragraphs"][0]["equations"][0]["id"];
    check(!RecoveryCodec::decode(crossKindDuplicate.dump(), error),
          "recovery codec accepted an image/equation NodeId collision");

    auto exactDimension = original;
    exactDimension["paragraphs"][0]["images"][0]["width_emu"] =
        kMaximumInlineImageDimensionEmu;
    check(RecoveryCodec::decode(exactDimension.dump(), error).has_value(),
          "recovery codec rejected the exact image-dimension limit");
    auto oversizedDimension = exactDimension;
    oversizedDimension["paragraphs"][0]["images"][0]["width_emu"] =
        kMaximumInlineImageDimensionEmu + 1;
    check(!RecoveryCodec::decode(oversizedDimension.dump(), error),
          "recovery codec accepted an image beyond the dimension limit");

    auto aggregateParagraph = Paragraph::create({}, NodeId{401, 402});
    check(static_cast<bool>(aggregateParagraph),
          "could not create aggregate-limit recovery paragraph");
    auto aggregateDocument = Document::create(
        {std::move(aggregateParagraph.value())});
    check(static_cast<bool>(aggregateDocument),
          "could not create aggregate-limit recovery document");
    const EncodedImagePayload maximumPayload(
        paddedOnePixelJpeg(kMaximumEncodedImageBytes));
    check(static_cast<bool>(aggregateDocument.value().insertImage(
              {{401, 402}, 0}, maximumPayload, ImageFormat::jpeg,
              "First maximum image", 1000, 1000, NodeId{403, 404})) &&
              static_cast<bool>(aggregateDocument.value().insertImage(
                  {{401, 402}, 1}, maximumPayload, ImageFormat::jpeg,
                  "Second maximum image", 1000, 1000,
                  NodeId{405, 406})),
          "core rejected the exact 32 MiB aggregate recovery boundary");
    auto aggregateEncoded = RecoveryCodec::encode(
        {aggregateDocument.value(), {}}, error);
    check(aggregateEncoded.has_value(),
          "recovery codec rejected the exact aggregate byte limit");
    check(RecoveryCodec::decode(*aggregateEncoded, error).has_value(),
          "recovery codec could not restore the exact aggregate byte limit");

    auto aggregateOverflow = nlohmann::json::parse(*aggregateEncoded);
    aggregateEncoded.reset();
    aggregateOverflow["paragraphs"][0]["text_utf16"].push_back(
        static_cast<std::uint16_t>(kInlineObjectReplacementCharacter));
    aggregateOverflow["paragraphs"][0]["images"].push_back(
        {{"id", NodeId{407, 408}.toString()},
         {"offset", 2},
         {"width_emu", 1000},
         {"height_emu", 1000},
         {"accessible_name", "Aggregate overflow"},
         {"format", "png"},
         {"encoded_base64", kOnePixelPngBase64}});
    check(!RecoveryCodec::decode(aggregateOverflow.dump(), error),
          "recovery codec accepted more than 32 MiB of image payloads");
}

void semanticImageCountBoundary() {
    using namespace docxstudio::app;
    using namespace docxstudio::core;

    auto paragraph = Paragraph::create({}, NodeId{501, 502});
    check(static_cast<bool>(paragraph),
          "could not create image-count recovery paragraph");
    auto document = Document::create({std::move(paragraph.value())});
    check(static_cast<bool>(document),
          "could not create image-count recovery document");
    const EncodedImagePayload payload(onePixelPng());
    for (std::size_t index = 0;
         index < kMaximumInlineImagesPerDocument; ++index) {
        const auto inserted = document.value().insertImage(
            {{501, 502}, index}, payload, ImageFormat::png, "Picture", 1, 1,
            NodeId{1'000, static_cast<std::uint64_t>(index + 1U)});
        check(static_cast<bool>(inserted),
              "core rejected an image at the exact count boundary");
    }

    std::string error;
    const auto encoded = RecoveryCodec::encode({document.value(), {}}, error);
    check(encoded.has_value(),
          "recovery codec rejected exactly 512 inline images");
    const auto restored = RecoveryCodec::decode(*encoded, error);
    check(restored.has_value() &&
              restored->document.paragraphs().front().images().size() ==
                  kMaximumInlineImagesPerDocument,
          "recovery codec did not restore exactly 512 inline images");

    auto oversized = nlohmann::json::parse(*encoded);
    oversized["paragraphs"][0]["text_utf16"].push_back(
        static_cast<std::uint16_t>(kInlineObjectReplacementCharacter));
    auto extraImage = oversized["paragraphs"][0]["images"].front();
    extraImage["id"] = NodeId{1'000, 10'000}.toString();
    extraImage["offset"] = kMaximumInlineImagesPerDocument;
    oversized["paragraphs"][0]["images"].push_back(std::move(extraImage));
    check(!RecoveryCodec::decode(oversized.dump(), error),
          "recovery codec accepted 513 inline images");
}

void storeCrud() {
    using namespace docxstudio::app;
    const auto databasePath = std::filesystem::temp_directory_path() /
        ("docxstudio-recovery-" + docxstudio::core::NodeId::generate().toString() +
         ".sqlite3");
    ChatStore store;
    std::string error;
    check(store.open(databasePath.string(), error), "could not open recovery test store");
    check(store.bindThread("document-1", "thread-old-catalog", error),
          "could not bind legacy Codex thread");
    check(store.appendMessage("document-1", "user", "retained history", error),
          "could not store chat history for catalog migration");
    const RecoveryRecord created{"journal-1", "/original/report.docx", "report.docx",
                                 std::string("one\0two", 7), 0};
    check(store.upsertRecovery(created, error), "could not create recovery record");
    const auto read = store.recoveryRecord("journal-1");
    check(read.has_value(), "could not read recovery record");
    check(read->sourcePath == created.sourcePath &&
              read->displayName == created.displayName &&
              read->payload == created.payload,
          "recovery record fields did not round-trip");
    check(store.recoveryRecords().size() == 1,
          "recovery record was absent from list operation");

    auto updated = created;
    updated.displayName = "revised.docx";
    updated.payload = "replacement";
    check(store.upsertRecovery(updated, error), "could not update recovery record");
    const auto reread = store.recoveryRecord("journal-1");
    check(reread && reread->displayName == "revised.docx" &&
              reread->payload == "replacement",
          "recovery record update was not persisted");
    check(store.deleteRecovery("journal-1", error), "could not delete recovery record");
    check(!store.recoveryRecord("journal-1") && store.recoveryRecords().empty(),
          "deleted recovery record remained in the store");
    store.close();

    sqlite3* rawDatabase = nullptr;
    check(sqlite3_open(databasePath.c_str(), &rawDatabase) == SQLITE_OK,
          "could not reopen raw store for catalog migration fixture");
    check(sqlite3_exec(
              rawDatabase,
              "UPDATE app_meta SET value='legacy-catalog' "
              "WHERE key='editor_tool_catalog_version';",
              nullptr, nullptr, nullptr) == SQLITE_OK,
          "could not mark the catalog fixture stale");
    sqlite3_close(rawDatabase);

    check(store.open(databasePath.string(), error),
          "could not migrate stale editor tool catalog");
    check(!store.threadForDocument("document-1"),
          "stale app-server thread mapping survived a tool catalog upgrade");
    const auto retainedMessages = store.messages("document-1");
    check(retainedMessages.size() == 1 &&
              retainedMessages.front().text == "retained history",
          "tool catalog upgrade discarded local chat history");
    check(store.bindThread("document-1", "thread-current-catalog", error),
          "could not bind current-catalog Codex thread");
    store.close();
    check(store.open(databasePath.string(), error),
          "could not reopen current editor tool catalog");
    check(store.threadForDocument("document-1") ==
              std::optional<std::string>("thread-current-catalog"),
          "current-catalog thread mapping was invalidated unnecessarily");
    store.close();
    std::error_code ignored;
    std::filesystem::remove(databasePath, ignored);
    std::filesystem::remove(databasePath.string() + "-wal", ignored);
    std::filesystem::remove(databasePath.string() + "-shm", ignored);
}

}  // namespace

int main() {
    codecRoundTrip();
    frozenVersion4FixtureAndPreflight();
    semanticImageRoundTrip();
    paragraphMarkCharacterFormatRoundTrip();
    paragraphStyleIdentityRoundTrip();
    legacyImageMigrationAndBoundaries();
    semanticImageAdversarialLimits();
    semanticImageCountBoundary();
    storeCrud();
    std::cout << "recovery tests passed\n";
    return 0;
}
