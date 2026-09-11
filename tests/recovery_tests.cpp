#include "docxstudio/app/ChatStore.h"
#include "docxstudio/app/RecoveryCodec.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
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

    const auto version = futureVersion.find("\"version\":4");
    check(version != std::string::npos, "encoded recovery version was absent");
    futureVersion.replace(version, std::string("\"version\":4").size(),
                          "\"version\":5");
    check(!RecoveryCodec::decode(futureVersion, error),
          "recovery codec accepted an unsupported future version");
    check(!RecoveryCodec::decode("{not-json", error),
          "recovery codec accepted malformed JSON");
}

void storeCrud() {
    using namespace docxstudio::app;
    const auto databasePath = std::filesystem::temp_directory_path() /
        ("docxstudio-recovery-" + docxstudio::core::NodeId::generate().toString() +
         ".sqlite3");
    ChatStore store;
    std::string error;
    check(store.open(databasePath.string(), error), "could not open recovery test store");
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
    std::error_code ignored;
    std::filesystem::remove(databasePath, ignored);
    std::filesystem::remove(databasePath.string() + "-wal", ignored);
    std::filesystem::remove(databasePath.string() + "-shm", ignored);
}

}  // namespace

int main() {
    codecRoundTrip();
    storeCrud();
    std::cout << "recovery tests passed\n";
    return 0;
}
