#include "docxstudio/ooxml/docx_document.h"
#include <QBuffer>
#include <QImage>
#include <zip.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace docxstudio::ooxml;
namespace {
void check(bool value,const char* message) { if(!value) { std::cerr<<"FAILED: "<<message<<'\n';std::exit(1); } }
NewInlineImage picture(bool jpeg) {
    QImage pixels(19,11,QImage::Format_RGB32);pixels.fill(jpeg ? QColor("#173b32") : QColor("#66507a"));
    QByteArray bytes;QBuffer buffer(&bytes);check(buffer.open(QIODevice::WriteOnly) && pixels.save(&buffer,jpeg ? "JPEG" : "PNG"),"encode picture fixture");
    NewInlineImage value;value.name=jpeg ? "Cell JPEG" : "Cell PNG";value.accessible_name=jpeg ? "Green cell picture" : "Purple cell picture";
    value.format=jpeg ? RasterImageFormat::jpeg : RasterImageFormat::png;value.width_emu=456000;value.height_emu=264000;
    value.bytes={bytes.begin(),bytes.end()};return value;
}
std::string member(const std::filesystem::path& path,const char* name) {
    int error=0;auto* zip=zip_open(path.c_str(),ZIP_RDONLY,&error);check(zip,"open image package");
    zip_stat_t stat{};check(zip_stat(zip,name,0,&stat)==0,"locate picture member");
    auto* input=zip_fopen(zip,name,0);std::string bytes(static_cast<std::size_t>(stat.size),'\0');
    check(input && zip_fread(input,bytes.data(),bytes.size())==static_cast<zip_int64_t>(bytes.size()),"read picture member");zip_fclose(input);zip_discard(zip);return bytes;
}
void replace(const std::filesystem::path& path,const char* name,const std::string& bytes) {
    int error=0;auto* zip=zip_open(path.c_str(),0,&error);check(zip,"open mutated package");
    auto* source=zip_source_buffer(zip,bytes.data(),bytes.size(),0);
    check(source && zip_file_add(zip,name,source,ZIP_FL_OVERWRITE)>=0 && zip_close(zip)==0,"replace package member");
}
}
int main() {
    char pattern[]="/tmp/owldocs-cell-images-XXXXXX";const auto* made=mkdtemp(pattern);check(made,"create temporary picture directory");const std::filesystem::path directory(made);
    const auto png=picture(false);const auto jpeg=picture(true);
    NewTable table{1,2,false,{"BeforeAfter","Other"}};
    NewParagraph first;first.runs={NewRun{"Before",{}},NewRun{"",{},std::nullopt,png},NewRun{"After",{}},NewRun{"",{},std::nullopt,jpeg}};
    NewParagraph second;second.runs={NewRun{"Other",{}},NewRun{"",{},std::nullopt,jpeg}};
    table.cell_paragraphs={first,second};NewDocumentBody body{{table,NewParagraph{{NewRun{"Tail",{}}}}}};
    const auto path=directory/"pictures.docx";const auto saved=DocxDocument::writeNew(path,body);
    if (!saved.saved && saved.error) std::cerr<<saved.error->message<<'\n';
    check(saved.saved,"write real pictures inside native table cells");
    const auto opened=DocxDocument::open(path);check(opened && opened->bodyBlocks().size()==2,"reopen table picture document");
    const auto* imported=std::get_if<ImportedTableBlock>(&opened->bodyBlocks()[0]);
    check(imported && imported->cells.size()==2,"supported inline table pictures remain preserved view-only");
    const auto& runs=opened->paragraphs()[imported->cells[0].source_paragraph_index].runs;
    std::vector<const InlineImagePayload*> images;std::string text;
    for (const auto& run:runs) for (const auto& fragment:run.fragments) {
        if (fragment.inline_image) { images.push_back(&*fragment.inline_image);text+='@'; }
        else text+=fragment.text;
    }
    check(text=="Before@After@" && images.size()==2,"cell picture/text ordering changed");
    check(images[0]->bytes==png.bytes && images[1]->bytes==jpeg.bytes && images[0]->width_emu==png.width_emu && images[1]->height_emu==jpeg.height_emu && images[0]->accessible_name==png.accessible_name,"cell picture bytes, geometry, or accessible name changed");
    const auto pngMember=member(path,"word/media/image1.png");
    check(std::vector<std::uint8_t>(pngMember.begin(),pngMember.end())==png.bytes && member(path,"word/_rels/document.xml.rels").find("media/image2.jpg")!=std::string::npos,"cell pictures are not native PNG/JPEG package members");
    check(opened->assessRegeneration(body,{},opened->sourceDefaults()).lossless,"table picture full-package reproduction proof failed");
    const auto native=member(path,"word/document.xml");
    auto floating=body;
    std::get<NewTable>(floating.blocks[0]).cell_paragraphs[0].runs[1].inline_image->layout.placement=ImagePlacement::square;
    check(!DocxDocument::writeNew(path,floating).saved && member(path,"word/document.xml")==native,
          "unsupported floating cell image overwrote the existing target");
    for (const bool external:{false,true}) {
        const auto corrupt=directory/(external ? "external.docx" : "malformed.docx");
        std::filesystem::copy_file(path,corrupt);
        if (external) {
            auto rels=member(corrupt,"word/_rels/document.xml.rels");
            const std::string target="Target=\"media/image1.png\"";
            const auto at=rels.find(target);check(at!=std::string::npos,"locate picture relationship");
            rels.replace(at,target.size(),"Target=\"https://example.invalid/picture.png\" TargetMode=\"External\"");
            replace(corrupt,"word/_rels/document.xml.rels",rels);
        } else replace(corrupt,"word/media/image1.png","not a raster picture");
        auto preserved=DocxDocument::open(corrupt);
        check(preserved && std::holds_alternative<ImportedUnsupportedBodyBlock>(preserved->bodyBlocks()[0]) &&
              !preserved->assessRegeneration(body,{},preserved->sourceDefaults()).lossless,
              "malformed or external table picture became editable or authorized regeneration");
        const auto tail=std::get<ImportedParagraphBlock>(preserved->bodyBlocks()[1]).source_paragraph_index;
        const auto span=preserved->paragraphs()[tail].runs[0].fragments[0].text_span_id;
        check(span && preserved->replaceText(*span,"Edited tail").accepted,"unsupported cell picture blocked a safe outside text patch");
        const auto patched=directory/(external ? "external-patched.docx" : "malformed-patched.docx");
        check(preserved->saveAs(patched).saved && member(patched,"word/media/image1.png")==member(corrupt,"word/media/image1.png") &&
              member(patched,"word/_rels/document.xml.rels")==member(corrupt,"word/_rels/document.xml.rels"),
              "safe body text patch changed an unsupported table image or relationship");
    }
    std::filesystem::remove_all(directory);std::cout<<"Table image OOXML tests passed\n";
}
