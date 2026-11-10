#include "docxstudio/ooxml/docx_document.h"
#include <pugixml.hpp>
#include <zip.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace docxstudio::ooxml;
namespace {
void check(bool condition,const char* message) {
    if (!condition) { std::cerr<<"FAILED: "<<message<<'\n';std::exit(1); }
}
std::string file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);check(bool(input),"open story fixture");
    return {std::istreambuf_iterator<char>(input),{}};
}
std::string member(const std::filesystem::path& path,const char* name) {
    int error=0;auto* archive=zip_open(path.c_str(),ZIP_RDONLY,&error);check(archive,"open story package");
    zip_stat_t stat{};check(zip_stat(archive,name,0,&stat)==0,"locate story part");
    auto* input=zip_fopen(archive,name,0);std::string text(static_cast<std::size_t>(stat.size),'\0');
    check(input && zip_fread(input,text.data(),text.size())==static_cast<zip_int64_t>(text.size()),"read story part");
    zip_fclose(input);zip_discard(archive);return text;
}
void replace(const std::filesystem::path& path,const char* name,const std::string& bytes) {
    int error=0;auto* archive=zip_open(path.c_str(),0,&error);check(archive,"open source mutation");
    auto* source=zip_source_buffer(archive,bytes.data(),bytes.size(),0);
    check(source && zip_file_add(archive,name,source,ZIP_FL_OVERWRITE)>=0 && zip_close(archive)==0,"replace story part");
}
NewInlineImage image(const char* size,const char* name) {
    const auto bytes=file(std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"packaging/icons/hicolor"/size/"apps/owl-docs.png");
    NewInlineImage value;value.name=name;value.accessible_name=name;value.width_emu=152400;value.height_emu=152400;
    value.bytes={bytes.begin(),bytes.end()};return value;
}
}
int main() {
    char pattern[]="/tmp/owldocs-regional-story-XXXXXX";const auto* created=mkdtemp(pattern);check(created,"create test directory");
    const std::filesystem::path directory(created);
    NewDocumentBody body{{NewParagraph{{NewRun{"Body",{}}}}}};
    body.header_text=std::string("\xef\xbf\xbc First\nSecond\tCenter {PAGE}\tRight \xef\xbf\xbc\nLast");
    body.footer_text=std::string("Bottom\nLeft \xef\xbf\xbc\tMiddle\tPage {PAGE} of {PAGES}\nRight");
    const auto first=image("16x16","First regional picture");
    const auto second=image("24x24","Second regional picture");
    body.header_images={{0,first},{body.header_text->rfind("\xef\xbf\xbc"),second}};
    body.footer_images={{body.footer_text->find("\xef\xbf\xbc"),first}};
    const auto path=directory/"regional.docx";
    check(DocxDocument::writeNew(path,body).saved,"write independent regional stories");
    const auto header=member(path,"word/header1.xml");
    const auto footer=member(path,"word/footer1.xml");
    for (const auto* xml : {&header,&footer}) {
        pugi::xml_document parsed;check(parsed.load_buffer(xml->data(),xml->size()),"parse native story XML");
        const auto table=parsed.document_element().child("w:tbl");
        check(table && std::distance(table.child("w:tr").children("w:tc").begin(),table.child("w:tr").children("w:tc").end())==3,
              "multiline regional story does not use three independent native cells");
        check(table.child("w:tblPr").child("w:tblBorders").child("w:top").attribute("w:val").value()==std::string("nil"),
              "regional native table has visible borders");
        auto cell=table.child("w:tr").child("w:tc");
        const auto expectedVertical=xml==&footer ? "bottom" : "top";
        for (const auto region : table.child("w:tr").children("w:tc"))
            check(std::string(region.child("w:tcPr").child("w:vAlign").attribute("w:val").value())==expectedVertical,
                  "native regional cell vertical alignment differs from the Canvas story edge");
        check(std::string(cell.child("w:p").child("w:pPr").child("w:jc").attribute("w:val").value())=="left","left cell alignment");
        cell=cell.next_sibling("w:tc");
        check(std::string(cell.child("w:p").child("w:pPr").child("w:jc").attribute("w:val").value())=="center","center cell alignment");
        cell=cell.next_sibling("w:tc");
        check(std::string(cell.child("w:p").child("w:pPr").child("w:jc").attribute("w:val").value())=="right","right cell alignment");
    }
    const auto reopened=DocxDocument::open(path);
    check(reopened && reopened->headerText()==body.header_text && reopened->footerText()==body.footer_text,
          "regional native stories did not reconstruct original region text and fields");
    check(reopened->headerImages().size()==2 && reopened->footerImages().size()==1 &&
          reopened->headerImages()[1].text_offset_bytes==body.header_images[1].text_offset_bytes &&
          reopened->headerImages()[1].image.bytes==second.bytes &&
          reopened->footerImages()[0].text_offset_bytes==body.footer_images[0].text_offset_bytes &&
          reopened->footerImages()[0].image.bytes==first.bytes,"regional native image offsets or payloads changed");
    check(reopened->assessRegeneration(body,{},reopened->sourceDefaults()).lossless,"regional native full-package proof failed");
    NewDocumentBody single{{NewParagraph{{NewRun{"Body",{}}}}}};
    single.header_text="Left\tCenter\tRight";
    const auto singlePath=directory/"single.docx";
    check(DocxDocument::writeNew(singlePath,single).saved,"write unchanged single-line story shape");
    const auto golden=file(std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"tests/fixtures/pre-regional-table-single-line-header.xml");
    check(member(singlePath,"word/header1.xml")==golden,"single-line header representation changed");
    auto spacedFields=single;
    spacedFields.header_text="{PAGE} {PAGES}\t \t ";
    const auto spacedPath=directory/"single-fields.docx";
    check(DocxDocument::writeNew(spacedPath,spacedFields).saved,"write single-line field spaces");
    const auto spacedSource=DocxDocument::open(spacedPath);
    check(spacedSource && spacedSource->headerText()==spacedFields.header_text &&
              spacedSource->assessRegeneration(spacedFields,{},spacedSource->sourceDefaults()).lossless,
          "single-line story drops spaces between fields or whitespace-only regions");
    NewDocumentBody wrapped=single;
    wrapped.header_text="{PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES}\tCenter {PAGE}\tRight";
    wrapped.footer_text="Bottom left\tA long middle footer region that wraps within its own third of the page without an explicit line break\t{PAGE} of {PAGES}";
    wrapped.header_regional_layout=true;
    wrapped.footer_regional_layout=true;
    const auto wrappedPath=directory/"wrapped.docx";
    check(DocxDocument::writeNew(wrappedPath,wrapped).saved,"write naturally wrapped regional stories");
    for (const auto* name : {"word/header1.xml","word/footer1.xml"}) {
        const auto xml=member(wrappedPath,name);
        pugi::xml_document parsed;check(parsed.load_buffer(xml.data(),xml.size()),"parse wrapped story XML");
        check(parsed.document_element().child("w:tbl"),"authoring layout hint did not produce independent regional cells");
    }
    const auto wrappedSource=DocxDocument::open(wrappedPath);
    check(wrappedSource && wrappedSource->headerText()==wrapped.header_text && wrappedSource->footerText()==wrapped.footer_text,
          "naturally wrapped story source did not reconstruct exact region text");
    check(wrappedSource->assessRegeneration(wrapped,{},wrappedSource->sourceDefaults()).lossless,
          "naturally wrapped story full-package proof failed");
    auto unprojected=wrapped;
    unprojected.header_regional_layout=false;
    unprojected.footer_regional_layout=false;
    check(!wrappedSource->assessRegeneration(unprojected,{},wrappedSource->sourceDefaults()).lossless,
          "wrapped story table became rewriteable without its matching layout projection");
    const auto legacyPath=directory/"legacy.docx";
    std::filesystem::copy_file(singlePath,legacyPath);
    auto legacy=golden;
    const auto at=legacy.find("<w:t>Left</w:t>");check(at!=std::string::npos,"locate legacy text");
    legacy.replace(at,std::string("<w:t>Left</w:t>").size(),"<w:t>First</w:t><w:br/><w:t>Second</w:t>");
    replace(legacyPath,"word/header1.xml",legacy);
    const auto old=DocxDocument::open(legacyPath);
    single.header_text="First\nSecond\tCenter\tRight";
    check(old && !old->assessRegeneration(single,{},old->sourceDefaults()).lossless,"old sequential multiline story became silently rewriteable");
    const auto patch=old->replaceText(*old->paragraphs()[0].runs[0].fragments[0].text_span_id,"Edited");
    check(patch.accepted && old->saveAs(directory/"patched.docx").saved,"old multiline story blocked a safe body text patch");
    check(member(directory/"patched.docx","word/header1.xml")==legacy,"body text patch changed original multiline story");
    auto malformed=header;
    const auto width=malformed.find("w:type=\"dxa\"");check(width!=std::string::npos,"locate native table width");
    malformed.replace(width,std::string("w:type=\"dxa\"").size(),"w:type=\"pct\"");
    replace(path,"word/header1.xml",malformed);
    const auto altered=DocxDocument::open(path);
    check(altered && !altered->headerText() && !altered->assessRegeneration(body,{},altered->sourceDefaults()).lossless,
          "noncanonical regional table was trusted as supported");
    std::filesystem::remove_all(directory);
    std::cout<<"Regional story OOXML tests passed\n";
}
