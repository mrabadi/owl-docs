#include "docxstudio/ooxml/docx_document.h"
#include <zip.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <iterator>
#include <unistd.h>

using namespace docxstudio::ooxml;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::string member(const std::filesystem::path& path, const char* name) {
    int error = 0;
    auto* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
    check(archive, "open zip");
    zip_stat_t stat{};
    check(zip_stat(archive, name, 0, &stat) == 0, "locate native part");
    auto* file = zip_fopen(archive, name, 0);
    std::string bytes(static_cast<std::size_t>(stat.size), '\0');
    check(zip_fread(file, bytes.data(), bytes.size()) == static_cast<zip_int64_t>(bytes.size()), "read native part");
    zip_fclose(file); zip_discard(archive);
    return bytes;
}
void replaceMember(const std::filesystem::path& path, const char* name, const std::string& bytes) {
    int error = 0;
    auto* archive = zip_open(path.c_str(), 0, &error);
    check(archive, "open zip for mutation");
    auto* source = zip_source_buffer(archive, bytes.data(), bytes.size(), 0);
    check(source && zip_file_add(archive, name, source, ZIP_FL_OVERWRITE) >= 0, "mutate native part");
    check(zip_close(archive) == 0, "close mutated zip");
}
}
int main() {
    char pattern[] = "/tmp/owldocs-academic-XXXXXX";
    const auto directory = std::filesystem::path(mkdtemp(pattern));
    const auto path = directory / "academic.docx";
    NewRun mark;
    mark.field = AcademicFieldPayload{AcademicFieldKind::note_reference,
        "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222", "1"};
    mark.field->note_number = 1;
    NewParagraph paragraph{{NewRun{"Body", {}}, mark}};
    paragraph.stable_id = "33333333-3333-4333-8333-333333333333";
    NewDocumentBody body{{paragraph}};
    body.notes.push_back({"22222222-2222-4222-8222-222222222222", false, 1, "Preserved note"});
    body.notes.push_back({"44444444-4444-4444-8444-444444444444", true, 1, "Preserved endnote"});
    body.academic_recovery = "{\"snapshot\":\"current only\"}";
    const auto result = DocxDocument::writeNew(path, body);
    if (!result && result.error) std::cerr << result.error->message << '\n';
    check(result.saved, "save native note document");
    const auto document = member(path, "word/document.xml");
    check(document.find("w:footnoteReference") != std::string::npos, "native footnote reference");
    check(member(path, "word/footnotes.xml").find("Preserved note") != std::string::npos, "native note part");
    const auto settings = member(path, "word/settings.xml");
    check(settings.find("<w:endnotePr><w:pos w:val=\"docEnd\"/><w:numFmt w:val=\"decimal\"") != std::string::npos,
          "native endnotes use document-wide decimal numbering");
    check(document.find("<w:endnotePr><w:pos w:val=\"docEnd\"/><w:numFmt w:val=\"decimal\"") != std::string::npos,
          "native section repeats decimal endnote numbering for office compatibility");
    auto reopened = DocxDocument::open(path);
    check(reopened && reopened->academicRecovery() == body.academic_recovery, "verified academic snapshot reopens");
    check(reopened->assessRegeneration(body, {}, {}).lossless, "academic package is reproducible");
    for (const auto* name : {"word/document.xml", "word/footnotes.xml", "word/endnotes.xml", "word/_rels/document.xml.rels", "[Content_Types].xml"}) {
        const auto changed = directory / "changed.docx";
        std::filesystem::copy_file(path, changed, std::filesystem::copy_options::overwrite_existing);
        replaceMember(changed, name, member(changed, name) + "<!-- changed in another editor -->");
        const auto opened = DocxDocument::open(changed);
        check(opened && !opened->academicRecovery(), "any native academic part change invalidates snapshot");
    }
    auto missingNote = body;
    missingNote.notes.clear();
    check(!DocxDocument::writeNew(directory / "missing-note.docx", missingNote).saved,
          "reject note reference with missing native note part");
    const auto fileBytes = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto originalBytes = fileBytes(path);
    auto invalidNote = body;
    invalidNote.notes.front().text = std::string("bad\x01", 4);
    const auto refused = DocxDocument::writeNew(path, invalidNote);
    check(!refused.saved && refused.error && refused.error->code == ErrorCode::unsafe_edit,
          "XML-invalid note text is rejected before save");
    check(fileBytes(path) == originalBytes, "invalid note save preserves existing target bytes");
    NewParagraph noPrefixCaption{{NewRun{"Remaining caption prose", {}}}};
    noPrefixCaption.stable_id = "55555555-5555-4555-8555-555555555555";
    noPrefixCaption.hidden_sequence_target = AcademicFieldPayload{AcademicFieldKind::caption_number,
        noPrefixCaption.stable_id, noPrefixCaption.stable_id, "Figure 1"};
    NewParagraph noPrefixSource{{NewRun{"Remaining bibliography prose", {}}}};
    noPrefixSource.stable_id = "66666666-6666-4666-8666-666666666666";
    noPrefixSource.hidden_sequence_target = AcademicFieldPayload{AcademicFieldKind::bibliography_number,
        noPrefixSource.stable_id, noPrefixSource.stable_id, "1"};
    NewRun fullReference;
    fullReference.field = AcademicFieldPayload{AcademicFieldKind::cross_reference,
        "77777777-7777-4777-8777-777777777777", noPrefixCaption.stable_id, "Remaining caption prose"};
    fullReference.field->full_text = true;
    NewDocumentBody noPrefixes{{noPrefixCaption, noPrefixSource, NewParagraph{{fullReference}}}};
    const auto noPrefixPath = directory / "no-prefix-targets.docx";
    check(DocxDocument::writeNew(noPrefixPath, noPrefixes).saved, "save deleted-prefix full-reference fixture");
    const auto noPrefixXml = member(noPrefixPath, "word/document.xml");
    const auto captionWhole = noPrefixXml.find("w:name=\"od_55555555555545558555555555555555\"");
    const auto sourceWhole = noPrefixXml.find("w:name=\"od_66666666666646668666666666666666\"");
    const auto captionSequence = noPrefixXml.find(" SEQ Figure ");
    const auto sourceSequence = noPrefixXml.find(" SEQ Reference ");
    check(captionWhole != std::string::npos && sourceWhole != std::string::npos &&
          captionSequence != std::string::npos && sourceSequence != std::string::npos &&
          noPrefixXml.find("<w:bookmarkEnd", captionSequence) < noPrefixXml.rfind("<w:bookmarkStart", captionWhole) &&
          noPrefixXml.find("<w:bookmarkEnd", sourceSequence) < noPrefixXml.rfind("<w:bookmarkStart", sourceWhole),
          "hidden sequence targets must end before full-text paragraph bookmarks start");
    check(noPrefixXml.find(" REF od_55555555555545558555555555555555 ") != std::string::npos,
          "full-text references still target the visible paragraph bookmark");
    const auto noPrefixReopened = DocxDocument::open(noPrefixPath);
    check(noPrefixReopened && noPrefixReopened->assessRegeneration(noPrefixes, {}, {}).lossless,
          "outside hidden targets retain complete native regeneration proof");
    std::filesystem::remove_all(directory);
    std::cout << "Academic OOXML tests passed\n";
}
