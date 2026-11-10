#include "docxstudio/ooxml/docx_document.h"

#include <pugixml.hpp>
#include <zip.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <unistd.h>

using namespace docxstudio::ooxml;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string member(const std::filesystem::path& path, const char* name) {
    int error = 0;
    auto* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
    check(archive != nullptr, "open native table package");
    zip_stat_t status{};
    check(zip_stat(archive, name, 0, &status) == 0, "locate native table part");
    auto* input = zip_fopen(archive, name, 0);
    std::string text(static_cast<std::size_t>(status.size), '\0');
    check(input && zip_fread(input, text.data(), text.size()) ==
                       static_cast<zip_int64_t>(text.size()),
          "read native table part");
    zip_fclose(input);
    zip_discard(archive);
    return text;
}

void replace(const std::filesystem::path& path, const char* name,
             const std::string& bytes) {
    int error = 0;
    auto* archive = zip_open(path.c_str(), 0, &error);
    check(archive != nullptr, "open native source mutation");
    auto* source = zip_source_buffer(archive, bytes.data(), bytes.size(), 0);
    check(source && zip_file_add(archive, name, source, ZIP_FL_OVERWRITE) >= 0 &&
              zip_close(archive) == 0,
          "replace native source part");
}

void testExplicitNoBorder(const std::filesystem::path& directory) {
    NewDocumentBody body{{NewTable{1, 2, false, {"One", "Two"}}}};
    const auto path = directory / "no-border.docx";
    check(DocxDocument::writeNew(path, body).saved, "write no-border source fixture");
    const auto original = member(path, "word/document.xml");
    std::ifstream golden(std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/owldocs-0.6.2-default-table.xml", std::ios::binary);
    check(golden.good(), "open frozen 0.6.2 automatic table XML");
    const std::string expected{std::istreambuf_iterator<char>(golden), {}};
    check(original == expected, "automatic table native representation changed from 0.6.2");
    pugi::xml_document xml;
    check(xml.load_buffer(original.data(), original.size()), "parse no-border fixture");
    auto properties = xml.child("w:document").child("w:body").child("w:tbl")
                          .child("w:tr").child("w:tc").child("w:tcPr");
    properties.append_child("w:tcBorders").append_child("w:top")
        .append_attribute("w:val") = "nil";
    std::ostringstream output;
    xml.save(output, "", pugi::format_raw);
    replace(path, "word/document.xml", output.str());
    const auto opened = DocxDocument::open(path);
    check(opened && !opened->bodyBlocks().empty(), "open explicit no-border fixture");
    const auto* table = std::get_if<ImportedTableBlock>(&opened->bodyBlocks().front());
    check(table && table->cells[0].border_top &&
              table->cells[0].border_top->width_eighth_points == 0,
          "explicit native no-border became inherited and would restore the table-style border");
}

void testAuthoredGeometryAndCellLayout(const std::filesystem::path& directory) {
    NewTable table{2, 2, true, {"Top left", "Centered", "Bottom", "Right"}};
    table.column_widths_twips = {1800, 3600};
    table.row_minimum_heights_twips = {900, 0};
    table.cell_layouts.resize(4);
    table.cell_layouts[0].vertical_alignment = BasicVerticalAlignment::top;
    table.cell_layouts[0].border_top = ImportedTableBorder{0, 0, BasicTableBorderStyle::none};
    table.cell_layouts[0].border_right = ImportedTableBorder{0x173b32, 12};
    table.cell_layouts[1].vertical_alignment = BasicVerticalAlignment::center;
    table.cell_layouts[1].border_left = ImportedTableBorder{0x173b32, 12};
    table.cell_layouts[2].vertical_alignment = BasicVerticalAlignment::bottom;
    table.cell_layouts[2].border_bottom = ImportedTableBorder{0x356078, 8};
    table.cell_layouts[3].border_bottom = ImportedTableBorder{0, 0, BasicTableBorderStyle::none};
    NewDocumentBody body{{table}};
    const auto path = directory / "authored.docx";
    check(DocxDocument::writeNew(path, body).saved, "write authored table geometry and cell layout");
    const auto native = member(path, "word/document.xml");
    pugi::xml_document xml;
    check(xml.load_buffer(native.data(), native.size()), "parse authored native table");
    const auto nativeTable = xml.child("w:document").child("w:body").child("w:tbl");
    const auto grid = nativeTable.child("w:tblGrid");
    check(grid.child("w:gridCol").attribute("w:w").as_uint() == 1800 &&
              grid.child("w:gridCol").next_sibling().attribute("w:w").as_uint() == 3600,
          "authored column widths were replaced by equal automatic widths");
    const auto height = nativeTable.child("w:tr").child("w:trPr").child("w:trHeight");
    check(height.attribute("w:val").as_uint() == 900 &&
              std::string(height.attribute("w:hRule").value()) == "atLeast" &&
              nativeTable.child("w:tr").child("w:trPr").child("w:tblHeader"),
          "row minimum height or repeating header was not native atLeast markup");
    check(!nativeTable.child("w:tr").next_sibling("w:tr").child("w:trPr").child("w:trHeight"),
          "automatic row acquired an exact or minimum height");
    auto cell = nativeTable.child("w:tr").child("w:tc");
    check(cell.child("w:tcPr").child("w:tcW").attribute("w:w").as_uint() == 1800 &&
              std::string(cell.child("w:tcPr").child("w:tcBorders").child("w:top")
                              .attribute("w:val").value()) == "nil",
          "native cell width or explicit no-border was not serialized");
    const auto opened = DocxDocument::open(path);
    check(opened && std::holds_alternative<ImportedTableBlock>(opened->bodyBlocks().front()),
          "authored table metadata is outside the editable native subset");
    const auto& imported = std::get<ImportedTableBlock>(opened->bodyBlocks().front());
    check(imported.column_widths_twips == table.column_widths_twips &&
              imported.row_minimum_heights_twips == table.row_minimum_heights_twips &&
              imported.cells[0].border_top &&
              imported.cells[0].border_top->style == BasicTableBorderStyle::none &&
              imported.cells[0].border_right == table.cell_layouts[0].border_right &&
              imported.cells[1].vertical_alignment == BasicVerticalAlignment::center &&
              imported.cells[2].vertical_alignment == BasicVerticalAlignment::bottom,
          "column/row geometry, explicit no-border, border weight/color, or vertical alignment changed on reopen");
    check(opened->assessRegeneration(body, {}, opened->sourceDefaults()).lossless,
          "authored table layout did not pass complete native package reproduction");
    const auto changedPath = directory / "externally-changed.docx";
    std::filesystem::copy_file(path, changedPath);
    auto changedXml = native;
    const auto changedWidth = changedXml.find("w:w=\"1800\"");
    check(changedWidth != std::string::npos, "locate native geometry mutation");
    changedXml.replace(changedWidth, 10, "w:w=\"1900\"");
    replace(changedPath, "word/document.xml", changedXml);
    const auto changed = DocxDocument::open(changedPath);
    check(changed && !changed->assessRegeneration(body, {}, changed->sourceDefaults()).lossless,
          "valid externally edited geometry bypassed the complete package proof");
    for (int invalid = 0; invalid < 6; ++invalid) {
        auto rejected = body;
        auto& candidate = std::get<NewTable>(rejected.blocks.front());
        if (invalid == 0) candidate.column_widths_twips = {0, 100};
        if (invalid == 1) candidate.column_widths_twips = {20000, 20000};
        if (invalid == 2) candidate.row_minimum_heights_twips = {500};
        if (invalid == 3) candidate.cell_layouts.pop_back();
        if (invalid == 4) candidate.cell_layouts[0].border_right->width_eighth_points = 97;
        if (invalid == 5) candidate.cell_layouts[0].border_top->width_eighth_points = 4;
        check(!DocxDocument::writeNew(path, rejected).saved &&
                  member(path, "word/document.xml") == native,
              "invalid table layout replaced the valid target");
    }
    auto exact = native;
    const auto atLeast = exact.find("atLeast");
    check(atLeast != std::string::npos, "locate native row minimum");
    exact.replace(atLeast, 7, "exact");
    replace(path, "word/document.xml", exact);
    const auto preserved = DocxDocument::open(path);
    check(preserved && std::holds_alternative<ImportedUnsupportedBodyBlock>(preserved->bodyBlocks().front()) &&
              !preserved->assessRegeneration(body, {}, preserved->sourceDefaults()).lossless,
          "foreign exact row height became an editable minimum and authorized destructive regeneration");
}
}  // namespace

int main() {
    char pattern[] = "/tmp/owldocs-table-format-XXXXXX";
    const auto* created = mkdtemp(pattern);
    check(created != nullptr, "create native table format test directory");
    const std::filesystem::path directory(created);
    testExplicitNoBorder(directory);
    testAuthoredGeometryAndCellLayout(directory);
    std::filesystem::remove_all(directory);
    std::cout << "Table format OOXML tests passed\n";
}
