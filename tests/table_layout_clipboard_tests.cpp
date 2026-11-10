#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
namespace { void check(bool value,const char* message) { if(!value) { std::cerr<<"FAILED: "<<message<<'\n'; std::exit(1); } } }
int main(int argc,char** argv) {
    QTemporaryDir settings; check(settings.isValid(),"clipboard private settings");
    qputenv("XDG_DATA_HOME",settings.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME",settings.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc,argv);
    using namespace docxstudio;
    core::Document document;
    auto table=core::Table::create(2,2); check(bool(table),"clipboard table fixture");
    const auto id=table.value().id();
    check(bool(document.insertTable(std::nullopt,std::move(table.value()))) &&
          bool(document.setTableGeometry(id,{{2400,3600},{480,600}})),"clipboard geometry fixture");
    core::TableCellLayout layout;
    layout.vertical_alignment=core::TableCellVerticalAlignment::bottom;
    layout.border_left=core::TableBorder{core::TableBorderStyle::none,0,0xff000000U};
    check(bool(document.setTableCellLayout(id,0,1,layout)),"clipboard cell layout fixture");
    app::SpellChecker spelling;
    app::DocumentCanvas source(spelling),destination(spelling);
    source.setDocument(document);
    check(source.selectTableCells(id,0,1,1,1),"clipboard geometry subrectangle selection");
    source.copy(); destination.paste();
    const auto copied=destination.snapshot().document;
    check(copied.tables().size()==1,"pasted cell rectangle is not a native table");
    const auto& restored=copied.tables().front();
    check(restored.geometry()==core::TableGeometry{{3600},{480,600}},"copied table rectangle loses its column widths or row minimum heights");
    check(restored.cell(0,0)->layout==layout,"copied cell loses vertical alignment or explicit-none border");
    destination.undo(); check(destination.snapshot().document.tables().empty(),"table geometry clipboard paste is not one Undo transaction");
    destination.redo(); check(destination.snapshot().document==copied,"table geometry clipboard Redo loses metadata or IDs");
    std::cout<<"Table layout clipboard tests passed\n";
}
