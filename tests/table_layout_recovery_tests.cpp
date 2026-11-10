#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/codex/editor_tools.hpp"
#include <cstdlib>
#include <iostream>
namespace { void check(bool value,const char* message) { if(!value) { std::cerr<<"FAILED: "<<message<<'\n'; std::exit(1); } } }
int main() {
    using namespace docxstudio;
    core::Document document;
    auto table=core::Table::create(2,2); check(bool(table),"recovery table fixture");
    const auto id=table.value().id(); check(bool(document.insertTable(std::nullopt,std::move(table.value()))),"recovery insert fixture");
    std::string error;
    const auto old=app::RecoveryCodec::encode({document,{}},error); check(bool(old),"ordinary recovery encode");
    check(bool(document.setTableGeometry(id,{{2400,3600},{480,0}})),"recovery geometry fixture");
    core::TableCellLayout layout; layout.vertical_alignment=core::TableCellVerticalAlignment::center;
    layout.border_left=core::TableBorder{core::TableBorderStyle::none,0,0xff000000U};
    check(bool(document.setTableCellLayout(id,1,1,layout)),"recovery layout fixture");
    const auto encoded=app::RecoveryCodec::encode({document,{}},error); check(bool(encoded),"layout recovery encode");
    auto json=codex::Json::parse(*encoded); check(json.at("version")==16,"new table metadata did not use schema16");
    const auto decoded=app::RecoveryCodec::decode(*encoded,error);
    check(decoded && decoded->document==document,"recovery lost table geometry, explicit none or vertical alignment");
    auto malformed=json;
    malformed["tables"][0]["geometry"]["column_widths_twips"][0]=18446744073709551615ULL;
    check(!app::RecoveryCodec::decode(malformed.dump(),error),"recovery narrowed an overflowing column width");
    malformed=json; malformed["tables"][0]["geometry"]["row_minimum_heights_twips"]={1,2,3};
    check(!app::RecoveryCodec::decode(malformed.dump(),error),"recovery accepted mismatched row dimensions");
    malformed=json; malformed["tables"][0]["cells"][3]["layout"]["left"]["width_eighth_points"]=4;
    check(!app::RecoveryCodec::decode(malformed.dump(),error),"recovery accepted noncanonical explicit-none border");
    malformed=json; malformed["tables"][0]["cells"][3]["layout"]["left"]["argb"]=0x00336699U;
    check(!app::RecoveryCodec::decode(malformed.dump(),error),"recovery accepted an unsupported transparent border");
    malformed=json; malformed["tables"][0]["cells"][3]["layout"]["vertical_alignment"]="diagonal";
    check(!app::RecoveryCodec::decode(malformed.dump(),error),"recovery silently defaulted unknown vertical alignment");
    json["version"]=15;
    check(!app::RecoveryCodec::decode(json.dump(),error),"schema15 accepted unsupported table layout metadata");
    check(bool(document.setTableGeometry(id,{})) && bool(document.setTableCellLayout(id,1,1,{})),"clear recovered metadata");
    const auto cleared=app::RecoveryCodec::encode({document,{}},error);
    check(cleared && *cleared==*old,"ordinary schema14 bytes changed after clearing table layout");
    std::cout<<"Table layout recovery tests passed\n";
}
