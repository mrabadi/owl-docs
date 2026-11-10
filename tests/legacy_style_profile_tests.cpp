#include "docxstudio/ooxml/docx_document.h"
#include <zip.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace docxstudio::ooxml;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
NewDocumentBody fixtureBody() {
    NewDocumentBody body;
    for (const auto* style : {"Normal","NoSpacing","Title","Subtitle","Quote","Heading1","Heading2","Heading3","Heading4","Heading5","Heading6","Heading7","Heading8","Heading9"}) {
        NewParagraph paragraph{{NewRun{std::string(style)+" legacy text", {}}}};
        paragraph.style_id=style;
        body.blocks.emplace_back(paragraph);
    }
    body.blocks.emplace_back(NewParagraph{{NewRun{"", {}, EquationPayload{R"(\frac{a}{b})", true}}}});
    body.blocks.emplace_back(NewTable{2,2,true,{"A","B","C","D"}});
    return body;
}
std::string member(const std::filesystem::path& path, const char* name) {
    int error=0; auto* archive=zip_open(path.c_str(),ZIP_RDONLY,&error);
    check(archive,"open fixture ZIP");
    zip_stat_t stat{}; check(zip_stat(archive,name,0,&stat)==0,"locate fixture member");
    auto* file=zip_fopen(archive,name,0); std::string bytes(static_cast<std::size_t>(stat.size),'\0');
    check(file && zip_fread(file,bytes.data(),bytes.size())==static_cast<zip_int64_t>(bytes.size()),"read fixture member");
    zip_fclose(file);zip_discard(archive);return bytes;
}
void replaceMember(const std::filesystem::path& path,const char* name,const std::string& bytes) {
    int error=0;auto* archive=zip_open(path.c_str(),0,&error);check(archive,"open mutation ZIP");
    auto* source=zip_source_buffer(archive,bytes.data(),bytes.size(),0);
    check(source && zip_file_add(archive,name,source,ZIP_FL_OVERWRITE)>=0,"replace fixture member");
    check(zip_close(archive)==0,"close mutation ZIP");
}
}
int main() {
    const auto fixture=std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"tests/fixtures/owldocs-0.6.0-styles.docx";
    const auto old=DocxDocument::open(fixture);
    check(old && old->sourceDefaults().legacy_style_defaults,"0.6.0 style profile was not detected");
    check(old->paragraphs()[2].runs.front().format.foreground_rgb==0x0077216fU &&
          old->paragraphs()[5].runs.front().format.font_size_half_points==32,"old document appearance changed during import");
    const auto neutralFixture=std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"tests/fixtures/owldocs-0.6.1-styles.docx";
    const auto neutral=DocxDocument::open(neutralFixture);
    check(neutral && neutral->sourceDefaults().neutral_style_defaults &&
          !neutral->sourceDefaults().legacy_style_defaults,
          "0.6.1 neutral style profile was not detected");
    check(neutral->paragraphs()[2].runs.front().format.foreground_rgb==0U &&
          neutral->paragraphs()[5].runs.front().format.font_size_half_points==40,
          "0.6.1 neutral appearance changed during import");
    auto body=fixtureBody();
    auto neutralBody=body;
    for (std::size_t index=0;index<14;++index) {
        auto& paragraph=std::get<NewParagraph>(neutralBody.blocks[index]);
        paragraph.runs[0].text=*paragraph.style_id+" neutral 0.6.1 text";
    }
    check(neutral->assessRegeneration(neutralBody,neutral->bodyPageSettings().value(),neutral->sourceDefaults()).lossless,
          "0.6.1 rich package cannot reproduce its neutral style definitions");
    check(old->assessRegeneration(body,old->bodyPageSettings().value(),old->sourceDefaults()).lossless,
          "old rich package cannot reproduce its original style definitions");
    char pattern[]="/tmp/owldocs-style-profile-XXXXXX";
    const auto directory=std::filesystem::path(mkdtemp(pattern));
    const auto oldCopy=directory/"old-profile.docx";
    check(DocxDocument::writeNew(oldCopy,body,{},old->sourceDefaults()).saved,"legacy profile write failed");
    check(member(oldCopy,"word/styles.xml")==member(fixture,"word/styles.xml"),"legacy style output changed");
    const auto modernPath=directory/"modern.docx";
    check(DocxDocument::writeNew(modernPath,body).saved,"modern profile write failed");
    const auto modern=DocxDocument::open(modernPath);
    check(modern && !modern->sourceDefaults().legacy_style_defaults,"new document unexpectedly adopted legacy styles");
    check(modern->paragraphs()[2].runs.front().format.foreground_rgb==0x173b32U &&
          modern->paragraphs()[5].runs.front().format.font_size_half_points==40,"new Title/Heading1 defaults are not modern");
    struct ExpectedStyle {
        std::int32_t half_points;
        bool bold;
        bool italic;
        std::uint32_t color;
        std::uint32_t before;
        std::uint32_t after;
    };
    constexpr std::array<ExpectedStyle,12> expected{{
        {56,true,false,0x173b32,0,240},{28,false,false,0x626a66,0,240},
        {22,false,true,0x404040,120,120},{40,true,false,0x173b32,360,160},
        {32,true,false,0x2f6b57,280,120},{28,true,false,0x356078,240,80},
        {24,true,false,0x66507a,200,80},{22,true,false,0x6f5747,160,60},
        {22,true,true,0x2f6b57,160,60},{22,false,true,0x626a66,120,40},
        {22,false,true,0x626a66,120,40},{22,false,false,0x626a66,120,40}}};
    for (std::size_t index=0;index<expected.size();++index) {
        const auto& paragraph=modern->paragraphs()[index+2];
        const auto& format=paragraph.runs.front().format;
        const auto& style=expected[index];
        check(format.font_size_half_points==style.half_points &&
              format.bold.value_or(false)==style.bold && format.italic.value_or(false)==style.italic &&
              format.foreground_rgb==style.color && paragraph.space_before_twips.value_or(0)==style.before &&
              paragraph.space_after_twips.value_or(0)==style.after,
              "modern native paragraph style differs from the requested appearance");
    }
    const auto opaque=directory/"opaque.docx";
    std::filesystem::copy_file(fixture,opaque);
    replaceMember(opaque,"customXml/opaque.bin","retained opaque source");
    const auto opaqueOpened=DocxDocument::open(opaque);
    check(opaqueOpened && opaqueOpened->sourceDefaults().legacy_style_defaults &&
          !opaqueOpened->assessRegeneration(body,{},opaqueOpened->sourceDefaults()).lossless,
          "legacy style identification bypassed complete source coverage");
    const auto altered=directory/"altered.docx";
    std::filesystem::copy_file(fixture,altered);
    replaceMember(altered,"word/styles.xml",member(fixture,"word/styles.xml")+"<!-- foreign change -->");
    const auto alteredOpened=DocxDocument::open(altered);
    check(alteredOpened && !alteredOpened->sourceDefaults().legacy_style_defaults,
          "a nonexact styles part was trusted as the legacy catalog");
    std::filesystem::remove_all(directory);
    std::cout<<"Legacy style profile tests passed\n";
}
