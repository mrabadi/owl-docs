#include "docxstudio/core/document_session.h"
#include <array>
#include <cstdlib>
#include <iostream>

using namespace docxstudio::core;
namespace { void check(bool value,const char* message) { if(!value) { std::cerr<<"FAILED: "<<message<<'\n'; std::exit(1); } } }
int main() {
    Document document;
    auto made=Table::create(2,2); check(bool(made),"table fixture");
    const auto id=made.value().id();
    check(bool(document.insertTable(std::nullopt,std::move(made.value()))),"insert table fixture");
    const auto ordinary=document;
    TableGeometry geometry{{2400,3600},{480,0}};
    TableCellLayout layout;
    layout.vertical_alignment=TableCellVerticalAlignment::bottom;
    layout.border_top=TableBorder{TableBorderStyle::single,12,0xff336699U};
    layout.border_right=TableBorder{TableBorderStyle::none,0,0xff000000U};
    check(bool(document.setTableGeometry(id,geometry)) && bool(document.setTableCellLayout(id,0,1,layout)),"table layout authoring");
    check(document.findTable(id)->geometry()==geometry && document.findTable(id)->cell(0,1)->layout==layout,"table geometry/layout was not durable");
    const auto styled=document;
    check(!document.setTableGeometry(id,{{2400},{}}) && document==styled,"invalid geometry count mutated table");
    check(!document.setTableGeometry(id,{{0,3000},{}}) && document==styled,"zero column width accepted");
    check(!document.setTableGeometry(id,{{20000,20000},{}}) && document==styled,"over-limit total width accepted");
    auto invalid=layout; invalid.border_top->width_eighth_points=97;
    check(!document.setTableCellLayout(id,0,1,invalid) && document==styled,"invalid border thickness mutated table");
    invalid=layout; invalid.border_right->width_eighth_points=4;
    check(!document.setTableCellLayout(id,0,1,invalid) && document==styled,"explicit-none border has noncanonical width");
    check(!document.setTableCellLayout(id,2,0,layout) && document==styled,"out-of-range cell layout accepted");
    DocumentSession session(ordinary);
    const auto before=session.snapshot();
    std::array<Operation,2> operations{SetTableGeometry{id,geometry},SetTableCellLayout{id,0,1,layout}};
    check(bool(session.applyBatch(before.revision,operations)),"layout operations batch");
    const auto after=session.snapshot();
    check(after.document==styled && bool(session.undo(after.revision)) && session.snapshot().document==ordinary,"layout Undo lost exact prior state");
    check(bool(session.redo(session.snapshot().revision)) && session.snapshot().document==styled,"layout Redo lost exact metadata");
    auto structure=styled;
    check(bool(structure.insertTableRow(id,1,{NodeId::generate(),NodeId::generate()})),"row insertion with minima");
    check(structure.findTable(id)->geometry().row_minimum_heights_twips==std::vector<std::uint32_t>({480,480,0}),"row minimum inheritance is inconsistent");
    check(bool(structure.insertTableColumn(id,1,{NodeId::generate(),NodeId::generate(),NodeId::generate()})),"column insertion with widths");
    check(structure.findTable(id)->geometry().column_widths_twips==std::vector<std::uint32_t>({2400,2400,3600}),"column width inheritance is inconsistent");
    check(bool(structure.deleteTableRows(id,1,1)) && bool(structure.deleteTableColumns(id,1,1)) && structure==styled,"row/column deletion did not retain original geometry and identities");
    auto bounded=styled;
    check(bool(bounded.setTableGeometry(id,{{16000,15680},{}})),"bounded column fixture");
    const auto beforeInsertion=bounded;
    check(!bounded.insertTableColumn(id,1,{NodeId::generate(),NodeId::generate()}) && bounded==beforeInsertion,
          "over-limit column inheritance mutated cell identities or geometry before rejecting");
    check(bool(document.setTableGeometry(id,{})) && bool(document.setTableCellLayout(id,0,1,{})) && document==ordinary,"clearing direct layout fails to restore inherited table");
    std::cout<<"Table layout core tests passed\n";
}
