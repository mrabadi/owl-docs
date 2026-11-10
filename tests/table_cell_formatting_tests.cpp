#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QTemporaryDir>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace docxstudio;
namespace { void check(bool value,const char* message) { if(!value) { std::cerr<<"FAILED: "<<message<<'\n'; std::exit(1); } } }
int main(int argc,char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME",settings.path().toUtf8()); qputenv("XDG_DATA_HOME",settings.path().toUtf8());
    QApplication application(argc,argv);
    app::SpellChecker spelling; app::DocumentCanvas canvas(spelling);
    canvas.resize(1000,900); canvas.show();
    check(canvas.insertTable(2,3,false),"formatting fixture");
    auto document=canvas.snapshot().document; const auto id=document.tables().front().id();
    check(bool(document.setTableGeometry(id,{{2000,3000,4000},{0,600}})),"formatting geometry fixture");
    canvas.setDocument(document); check(canvas.selectTable(id),"whole table fixture selection");
    check(canvas.selectedTableColumnWidthPoints()<0 && canvas.selectedTableRowMinimumHeightPoints()<0,"mixed dimension getter invents one value");
    check(canvas.setSelectedTableColumnWidth(100),"numeric table column width");
    check(canvas.snapshot().document.findTable(id)->geometry()==core::TableGeometry{{2000,2000,2000},{0,600}},"numeric width changed raw row minima or missed selected columns");
    const auto stable=canvas.snapshot(); check(canvas.setSelectedTableColumnWidth(100) && canvas.snapshot().revision==stable.revision,"no-op numeric width creates history");
    check(!canvas.setSelectedTableColumnWidth(17) && !canvas.setSelectedTableColumnWidth(500) &&
          !canvas.setSelectedTableColumnWidth(std::numeric_limits<double>::quiet_NaN()) && canvas.snapshot().document==stable.document,"invalid column width mutates table");
    canvas.undo(); check(canvas.snapshot().document==document,"numeric width is not one Undo transaction");
    check(canvas.selectTableCells(id,0,0,0,2) && canvas.selectedTableRowMinimumHeightPoints()==0,"automatic row getter is not zero");
    check(canvas.setSelectedTableRowMinimumHeight(40),"numeric row minimum");
    check(canvas.snapshot().document.findTable(id)->geometry()==core::TableGeometry{{2000,3000,4000},{800,600}},"row minimum changed widths or unselected rows");
    canvas.undo(); check(canvas.snapshot().document==document,"row minimum Undo changed table");
    check(canvas.selectTable(id) && canvas.distributeSelectedTableColumns(),"distribute selected columns");
    check(canvas.snapshot().document.findTable(id)->geometry()==core::TableGeometry{{3000,3000,3000},{0,600}},"column distribution changes fixed total or raw row minima");
    canvas.undo(); check(canvas.snapshot().document==document,"column distribution Undo");
    check(canvas.distributeSelectedTableRows(),"distribute selected rows");
    const auto distributed=canvas.snapshot().document.findTable(id)->geometry();
    check(distributed.column_widths_twips==document.findTable(id)->geometry().column_widths_twips &&
          distributed.row_minimum_heights_twips.size()==2 && distributed.row_minimum_heights_twips[0]==distributed.row_minimum_heights_twips[1] &&
          distributed.row_minimum_heights_twips[0]>=600,"row distribution ignores measured maximum or changes widths");
    canvas.undo(); check(canvas.snapshot().document==document,"row distribution Undo");
    check(canvas.activateTableCell(id,0,0) && canvas.setSelectedTableCellAlignment(core::ParagraphAlignment::right,core::TableCellVerticalAlignment::bottom),"cell alignment");
    check(canvas.selectTableCells(id,0,0,0,1) && !canvas.selectedTableCellAlignment(),"mixed alignment getter invents one position");
    const auto beforeAlignment=canvas.snapshot().document;
    check(canvas.setSelectedTableCellAlignment(core::ParagraphAlignment::center,core::TableCellVerticalAlignment::center),"rectangle alignment");
    check(canvas.selectedTableCellAlignment()==std::pair{core::ParagraphAlignment::center,core::TableCellVerticalAlignment::center},"rectangle alignment getter");
    canvas.undo(); check(canvas.snapshot().document==beforeAlignment,"both alignment axes are not atomic");
    canvas.setDocument(document); check(canvas.activateTableCell(id,0,0),"shared border fixture");
    check(canvas.setSelectedTableBorders(QStringLiteral("right"),1,QColor(QStringLiteral("#336699"))),"selected cell right border");
    const auto bordered=canvas.snapshot().document; const auto* table=bordered.findTable(id);
    check(table->cell(0,0)->layout.border_right && table->cell(0,1)->layout.border_left==table->cell(0,0)->layout.border_right &&
          !table->cell(0,1)->layout.border_top,"shared border does not mirror neighbor opposite side or changes unrelated properties");
    const auto borderRevision=canvas.snapshot().revision;
    check(canvas.setSelectedTableBorders(QStringLiteral("right"),1,QColor(QStringLiteral("#336699"))) && canvas.snapshot().revision==borderRevision,"no-op border creates history");
    check(canvas.setSelectedTableBorders(QStringLiteral("none"),0.5,Qt::black),"clear selected cell borders");
    const auto cleared=canvas.snapshot().document.findTable(id)->cell(0,0)->layout;
    check(cleared.border_top && cleared.border_right && cleared.border_bottom && cleared.border_left &&
          cleared.border_right->style==core::TableBorderStyle::none && cleared.border_right->width_eighth_points==0,"No Borders remains inherited");
    canvas.undo(); check(canvas.snapshot().document==bordered,"shared border clearing is not one Undo transaction");
    check(!canvas.setSelectedTableBorders(QStringLiteral("unknown"),1,Qt::black) && !canvas.setSelectedTableBorders(QStringLiteral("all"),0,Qt::black) &&
          !canvas.setSelectedTableBorders(QStringLiteral("all"),1,QColor(0,0,0,10)) && canvas.snapshot().document==bordered,"invalid border mutates document");
    canvas.setDocument(document);
    app::ImportedTablePresentation presentation; presentation.tableId=id;
    presentation.cellIds={document.findTable(id)->cell(0,0)->id};
    app::ImportedTableCellPresentation imported;
    imported.alignment=core::ParagraphAlignment::right; imported.verticalAlignment=app::ImportedCellVerticalAlignment::bottom;
    presentation.cells={imported};
    canvas.setImportedPresentation({}, {presentation});
    check(canvas.activateTableCell(id,0,0),"imported alignment fixture");
    check(canvas.selectedTableCellAlignment()==std::pair{core::ParagraphAlignment::right,core::TableCellVerticalAlignment::bottom},
          "cell alignment getter ignores inherited imported right/bottom presentation");
    check(canvas.setSelectedTableCellAlignment(core::ParagraphAlignment::left,core::TableCellVerticalAlignment::top),"override imported alignment");
    const auto overridden=canvas.snapshot().document;
    check(overridden.findTable(id)->cell(0,0)->paragraph_format.alignment==core::ParagraphAlignment::left &&
          overridden.findTable(id)->cell(0,0)->layout.vertical_alignment==core::TableCellVerticalAlignment::top,
          "Top Left skips explicit overrides for imported right/bottom alignment");
    canvas.undo(); check(canvas.snapshot().document==document,"imported alignment override is not one Undo transaction");
    presentation.cellIds={document.findTable(id)->cell(0,0)->id,document.findTable(id)->cell(0,0)->id};
    presentation.cells={imported,imported}; canvas.setImportedPresentation({}, {presentation});
    check(canvas.selectedTableCellAlignment()==std::pair{core::ParagraphAlignment::left,core::TableCellVerticalAlignment::top},
          "duplicate imported cell identities override semantic alignment");
    presentation.cellIds.clear(); presentation.cells.resize(6); presentation.cells[0]=imported;
    canvas.setImportedPresentation({}, {presentation});
    check(canvas.selectedTableCellAlignment()==std::pair{core::ParagraphAlignment::right,core::TableCellVerticalAlignment::bottom},
          "same-shape legacy imported alignment was ignored");
    presentation.cells.resize(1); canvas.setImportedPresentation({}, {presentation});
    check(canvas.selectedTableCellAlignment()==std::pair{core::ParagraphAlignment::left,core::TableCellVerticalAlignment::top},
          "stale legacy cell ordinal presentation overrides alignment");
    QString summary,error;
    core::TableCellLayout previewLayout; previewLayout.vertical_alignment=core::TableCellVerticalAlignment::center;
    const auto beforePreview=canvas.snapshot();
    check(canvas.createOperationsPreview(beforePreview.revision,{core::SetTableCellLayout{id,0,0,previewLayout}},QStringLiteral("Table preview"),summary,error),"table formatting preview fixture");
    check(!canvas.setSelectedTableColumnWidth(100) && !canvas.setSelectedTableRowMinimumHeight(40) &&
          !canvas.distributeSelectedTableColumns() && !canvas.distributeSelectedTableRows() &&
          !canvas.setSelectedTableBorders(QStringLiteral("all"),1,Qt::black) &&
          !canvas.setSelectedTableCellAlignment(core::ParagraphAlignment::right,core::TableCellVerticalAlignment::bottom) &&
          canvas.snapshot().document==beforePreview.document,"table controls mutate live content during preview");
    canvas.discardPreview();
    canvas.beginHeaderFooterEditing(); const auto inStory=canvas.snapshot().document;
    check(!canvas.setSelectedTableColumnWidth(100) && !canvas.setSelectedTableRowMinimumHeight(40) &&
          !canvas.setSelectedTableBorders(QStringLiteral("none"),0.5,Qt::black) &&
          !canvas.setSelectedTableCellAlignment(core::ParagraphAlignment::left,core::TableCellVerticalAlignment::top) && canvas.snapshot().document==inStory,
          "body table formatting mutates hidden selection while story editing");
    std::cout<<"Table cell formatting tests passed\n";
}
