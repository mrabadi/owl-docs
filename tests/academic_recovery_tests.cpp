#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/core/academic.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace docxstudio::core;
using docxstudio::app::RecoveryCodec;
namespace {
void check(bool result, const char* message) {
    if (!result) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}
int main() {
    Document document;
    const auto paragraph = document.paragraphs().front().id();
    const auto note = NodeId::generate();
    const auto field = NodeId::generate();
    check(bool(document.setNote({note, NoteKind::footnote, u"Preserved note\nsecond line"})), "note fixture");
    check(bool(document.insertField({paragraph, 0}, {field, 0, FieldKind::note_reference, note})), "field fixture");
    const auto toc = NodeId::generate();
    check(bool(document.insertToc(paragraph, {toc, 3})), "TOC fixture");
    check(bool(document.setCaption(paragraph, CaptionMetadata{CaptionLabel::figure, NodeId::generate()})), "caption fixture");
    const auto iconBytes=[](const char* size) {
        const auto path=std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"packaging/icons/hicolor"/size/"apps/owl-docs.png";
        std::ifstream input(path,std::ios::binary);
        check(bool(input),"open original story image fixture");
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(input),{}};
    };
    const EncodedImagePayload firstImage(iconBytes("16x16"));
    const EncodedImagePayload secondImage(iconBytes("24x24"));
    for (const bool footer : {false,true}) {
        const auto first=NodeId::generate();
        const auto second=NodeId::generate();
        check(bool(document.insertHeaderFooterImage(footer,0,firstImage,ImageFormat::png,"First story image",152400,152400,first)),"insert first story image");
        check(bool(document.insertHeaderFooterImage(footer,1,secondImage,ImageFormat::png,"Surviving story image",304800,304800,second)),"insert second story image");
        check(bool(footer ? document.setFooterText(u"\ufffc",StoryTextEditRange{0,1})
                          : document.setHeaderText(u"\ufffc",StoryTextEditRange{0,1})),"delete first adjacent story image explicitly");
        const auto& images=footer ? document.footerImages() : document.headerImages();
        check(images.size()==1 && images.front().id==second && images.front().encoded_payload==secondImage,
              "surviving image identity or payload changed before recovery");
    }
    std::string error;
    const auto encoded = RecoveryCodec::encode({document, {}}, error);
    check(encoded.has_value(), "encode academics");
    const auto decoded = RecoveryCodec::decode(*encoded, error);
    if (!decoded) std::cerr << error << '\n';
    check(decoded.has_value(), "decode academics");
    check(decoded->document == document, "academic identity and semantics roundtrip");
    auto corrupted = nlohmann::json::parse(*encoded);
    corrupted["paragraphs"][0]["fields"][0]["offset"] = 12;
    check(!RecoveryCodec::decode(corrupted.dump(), error), "reject orphan field metadata");
    Document legacy;
    const auto plain = RecoveryCodec::encode({legacy, {}}, error);
    auto old = nlohmann::json::parse(*plain);
    old["version"] = 13;
    old.erase("notes"); old.erase("tables_of_contents");
    for (auto& p : old["paragraphs"]) { p.erase("fields"); p.erase("caption"); p.erase("bibliography_entry"); }
    const auto migrated = RecoveryCodec::decode(old.dump(), error);
    check(migrated && migrated->document == legacy, "migrate schema13 without academics");
    std::cout << "Academic recovery tests passed\n";
}
