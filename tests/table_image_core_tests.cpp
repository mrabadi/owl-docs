#include "docxstudio/core/document_session.h"
#include <array>
#include <cstdlib>
#include <iostream>

using namespace docxstudio::core;
namespace {
void check(bool value,const char* message) {
    if(!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
EncodedImagePayload picture() {
    return EncodedImagePayload({
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x02U,
        0x08U, 0x02U, 0x00U, 0x00U, 0x00U, 0xfdU, 0xd4U, 0x9aU,
        0x73U, 0x00U, 0x00U, 0x00U, 0x16U, 0x49U, 0x44U, 0x41U,
        0x54U, 0x78U, 0x9cU, 0x63U, 0x7cU, 0x16U, 0xa2U, 0xc0U,
        0xc0U, 0xc0U, 0xc0U, 0xc4U, 0xc0U, 0xc0U, 0xc0U, 0xc0U,
        0xc0U, 0x00U, 0x00U, 0x11U, 0x28U, 0x01U, 0x5eU, 0xb8U,
        0xf9U, 0xb4U, 0x71U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U,
        0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
    });
}
}
int main() {
    Document document;
    auto table=Table::create(2,2);
    check(bool(table),"table fixture");
    const auto tableId=table.value().id();
    check(bool(document.insertTable(std::nullopt,table.value())),"table insertion");
    check(bool(document.setTableCellText(tableId,0,0,u"AB")),"cell text fixture");
    const auto imageId=NodeId::generate();
    const auto bytes=picture();
    const auto insertedPicture=document.insertTableCellImage(tableId,0,0,1,bytes,ImageFormat::png,"Cell picture",381000,228600,imageId);
    if (!insertedPicture) std::cerr << insertedPicture.error().message << '\n';
    check(bool(insertedPicture),
          "bounded table-cell picture insertion");
    const auto* cell=document.findTable(tableId)->cell(0,0);
    check(cell->text==u"A\ufffcB" && cell->images.size()==1 && cell->images.front().utf16_offset==1 &&
          cell->images.front().encoded_payload==bytes && document.findImage(imageId),"table picture lost identity/payload/order");
    const auto inserted=document;
    check(!document.insertTableCellImage(tableId,0,1,0,bytes,ImageFormat::png,"Duplicate",381000,228600,imageId),
          "duplicate picture identity was accepted");
    check(document==inserted,"rejected table picture insertion mutated document");
    check(bool(document.replaceTableCellRange(tableId,0,0,0,1,u"XYZ")),"prefix replacement");
    check(document.findTable(tableId)->cell(0,0)->images.front().utf16_offset==3,"cell text edit failed to shift picture atom");
    check(bool(document.replaceTableCellRange(tableId,0,0,3,4,u"")) && !document.findImage(imageId),
          "range deletion left an orphan table picture");
    ImageLayout floating; floating.placement=ImagePlacement::square;
    auto withImage=inserted;
    check(!withImage.setImageLayout(imageId,floating) && withImage==inserted,"floating cell picture layout was accepted");
    DocumentSession session(document);
    const auto before=session.snapshot();
    std::array<Operation,1> operations{InsertTableCellImage{tableId,1,1,0,bytes,ImageFormat::png,"Undo picture",381000,228600}};
    check(bool(session.applyBatch(before.revision,operations)),"table picture session operation");
    const auto after=session.snapshot();
    check(bool(session.undo(after.revision)) && session.snapshot().document==before.document,"table picture Undo changed payload or table state");
    check(bool(session.redo(session.snapshot().revision)) && session.snapshot().document==after.document,"table picture Redo changed identity/payload");
    DocumentSessionLimits limits;
    limits.maximum_retained_history_image_bytes=0;
    DocumentSession bounded(inserted,limits);
    std::array<Operation,1> erasePicture{ReplaceTableCellRange{tableId,0,0,1,2,{},{}}};
    check(bool(bounded.applyBatch(bounded.snapshot().revision,erasePicture)),"bounded table picture deletion");
    check(!bounded.canUndo(),"deleted cell-picture payload bypasses the retained history byte budget");
    auto replacement=inserted;
    auto copiedCell=*inserted.findTable(tableId)->cell(0,0);
    copiedCell.id=inserted.findTable(tableId)->cell(0,1)->id;
    copiedCell.images.front().id=NodeId::generate();
    check(bool(replacement.setTableCellContent(tableId,0,1,copiedCell)),"validated cell content replacement");
    check(replacement.findTable(tableId)->cell(0,1)->images.front()==copiedCell.images.front(),
          "cell content replacement loses copied picture metadata");
    auto structure=inserted;
    const auto originalImage=*structure.findImage(imageId);
    check(bool(structure.insertTableRow(tableId,0,{NodeId::generate(),NodeId::generate()})),"insert row around cell picture");
    check(bool(structure.insertTableColumn(tableId,0,{NodeId::generate(),NodeId::generate(),NodeId::generate()})),
          "insert column around cell picture");
    check(structure.findTable(tableId)->cell(1,1)->images.front()==originalImage,
          "row or column insertion copied or discarded picture identity/payload");
    check(bool(structure.deleteTableRows(tableId,0,1)) && bool(structure.deleteTableColumns(tableId,0,1)) && structure==inserted,
          "deleting surrounding rows/columns fails to restore exact pictured table");
    check(bool(structure.deleteTableColumns(tableId,0,1)) && !structure.findImage(imageId),
          "deleting pictured column retains an orphan image");
    auto conflicting=copiedCell;
    conflicting.images.front().id=imageId;
    const auto beforeConflict=replacement;
    check(!replacement.setTableCellContent(tableId,0,1,std::move(conflicting)) && replacement==beforeConflict,
          "cell content replacement accepts a picture identity owned by another cell");
    std::cout << "Table image core tests passed\n";
}
