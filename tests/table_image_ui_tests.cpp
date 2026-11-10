#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/app/EditorToolBridge.h"
#include "docxstudio/codex/editor_tools.hpp"
#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMenu>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::vector<std::uint8_t> picture(const char* encoding="PNG") {
    QImage image(40,24,QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#d51724")));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer,encoding), "picture fixture encoding failed");
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size()};
}
QRect pictureBounds(const QImage& image) {
    QRect bounds;
    for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
        const auto pixel=image.pixelColor(x,y);
        if(pixel.red()>180 && pixel.green()<60 && pixel.blue()<70) bounds|=QRect(x,y,1,1);
    }
    return bounds;
}
QImage canvasRaster(docxstudio::app::DocumentCanvas& canvas) {
    QApplication::processEvents();
    QImage raster(canvas.viewport()->size(),QImage::Format_RGB32);
    QPainter painter(&raster); canvas.viewport()->render(&painter);
    return raster;
}
void nativeRoundTrip() {
    QTemporaryDir directory;
    check(directory.isValid(),"native table picture temp directory");
    docxstudio::app::MainWindow window;
    window.resize(1100,900); window.show();
    auto* canvas=window.findChild<docxstudio::app::DocumentCanvas*>();
    check(canvas && canvas->insertTable(1,2,false),"native table fixture");
    const auto tableId=canvas->snapshot().document.tables().front().id();
    check(canvas->activateTableCell(tableId,0,0,0) && canvas->insertInlineImage(picture(),QStringLiteral("Native cell picture")),
          "native cell picture fixture");
    check(canvas->activateTableCell(tableId,0,0,0),"native cell prefix caret");
    canvas->insertText(QStringLiteral("Before "));
    check(canvas->snapshot().document.findTable(tableId)->cell(0,0)->text==u"Before \ufffc",
          "typing prose before a table picture rejects or loses its atom");
    check(canvas->activateTableCell(tableId,0,0,8),"native second picture caret");
    canvas->insertText(QStringLiteral(" between "));
    check(canvas->insertInlineImage(picture("JPEG"),QStringLiteral("Native JPEG cell picture")),"native JPEG cell picture fixture");
    const auto beforeSave=canvas->snapshot().document;
    const auto path=directory.filePath(QStringLiteral("table-picture.docx"));
    auto* saveAs=window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs,"Save As command is unavailable");
    QTimer dismiss;
    QObject::connect(&dismiss,&QTimer::timeout,&window,[] {
        for (auto* widget:QApplication::topLevelWidgets())
            if (auto* message=qobject_cast<QMessageBox*>(widget); message && message->isVisible()) message->accept();
    });
    dismiss.start(1);
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QFileDialog*>();
        check(dialog,"Save As did not open its file dialog");
        dialog->selectFile(path);
        check(QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection),"accepting table picture Save As");
    });
    saveAs->trigger();
    check(QFileInfo::exists(path) && !canvas->isModified(),"native save cannot retain table picture content");
    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),"table picture DOCX cannot reopen");
    auto* restored=reopened.findChild<docxstudio::app::DocumentCanvas*>();
    check(restored && restored->snapshot().document==beforeSave,
          "native DOCX save/reopen lost table/cell/picture identity, payload, or geometry");
    check(restored->activateTableCell(tableId,0,0,0),"reopened cell image is not editable");
    restored->insertText(QStringLiteral("Prefix "));
    const auto edited=restored->snapshot().document;
    check(edited.findTable(tableId)->cell(0,0)->text.starts_with(u"Prefix Before "),
          "reopened table picture rejected the actual prefix edit");
    auto* save=reopened.findChild<QAction*>(QStringLiteral("file.save"));
    check(save,"Save command is unavailable"); save->trigger();
    check(!restored->isModified(),"source-proof safe table picture edit cannot save");
    docxstudio::app::MainWindow again;
    check(again.openPath(path),"edited native table picture cannot reopen");
    const auto* finalCanvas=again.findChild<docxstudio::app::DocumentCanvas*>();
    check(finalCanvas && finalCanvas->snapshot().document==edited,"second native roundtrip corrupts cell image anchors or bytes");
    if(qEnvironmentVariableIsSet("OWL_DOCS_CAPTURE_TABLE_IMAGES")) {
        directory.setAutoRemove(false);
        std::cout << "Table image native fixture: " << path.toStdString() << '\n';
    }
}
void rectangularClipboard(docxstudio::app::SpellChecker& spelling) {
    docxstudio::app::DocumentCanvas source(spelling),destination(spelling);
    check(source.insertTable(2,2,false) && destination.insertTable(3,3,false),"rectangular picture clipboard fixture");
    const auto sourceId=source.snapshot().document.tables().front().id();
    const auto destinationId=destination.snapshot().document.tables().front().id();
    check(source.activateTableCell(sourceId,0,0) && source.insertInlineImage(picture(),QStringLiteral("Grid PNG")) &&
          source.activateTableCell(sourceId,1,1) && source.insertInlineImage(picture("JPEG"),QStringLiteral("Grid JPEG")),
          "rectangular PNG/JPEG clipboard fixture");
    const auto original=source.snapshot().document;
    check(source.selectTableCells(sourceId,0,0,1,1),"rectangular picture clipboard selection"); source.copy();
    check(destination.activateTableCell(destinationId,1,1),"rectangular picture paste origin");
    const auto before=destination.snapshot().document;
    destination.paste();
    const auto pasted=destination.snapshot().document;
    const auto* result=pasted.findTable(destinationId);
    check(result->cell(1,1)->images.size()==1 && result->cell(2,2)->images.size()==1 &&
          result->cell(1,1)->id==before.findTable(destinationId)->cell(1,1)->id &&
          result->cell(2,2)->images.front().encoded_payload==original.findTable(sourceId)->cell(1,1)->images.front().encoded_payload &&
          result->cell(1,1)->images.front().id!=original.findTable(sourceId)->cell(0,0)->images.front().id,
          "rectangular cell clipboard loses picture placement, identity or encoded bytes");
    destination.undo(); check(destination.snapshot().document==before,"rectangular picture paste is not one Undo transaction");
    destination.redo(); check(destination.snapshot().document==pasted,"rectangular picture Redo changed picture identities");
    check(destination.selectTableCells(destinationId,1,1,2,2),"rectangular picture Cut selection"); destination.cut();
    const auto cut=destination.snapshot().document;
    check(cut.findTable(destinationId)->cell(1,1)->text.empty() && cut.findTable(destinationId)->cell(2,2)->images.empty(),
          "rectangular Cut leaves covered cell pictures behind");
    destination.undo(); check(destination.snapshot().document==pasted,"rectangular picture Cut Undo lost original atom identities");
}
}
int main(int argc,char** argv) {
    QTemporaryDir privateSettings;
    check(privateSettings.isValid(),"private table picture test settings");
    qputenv("XDG_DATA_HOME",privateSettings.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CONFIG_HOME",privateSettings.filePath(QStringLiteral("config")).toUtf8());
    QApplication application(argc,argv);
    if (application.arguments().contains(QStringLiteral("--docx-only"))) {
        nativeRoundTrip(); return 0;
    }
    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.resize(1000,900); canvas.show();
    check(canvas.insertTable(1,2,false), "table fixture insertion failed");
    const auto tableId=canvas.snapshot().document.tables().front().id();
    check(canvas.activateTableCell(tableId,0,0,0), "cell activation failed");
    const auto before=canvas.snapshot().document;
    QObject::connect(&canvas,&docxstudio::app::DocumentCanvas::operationFailed,&canvas,
        [](const QString& message) { std::cerr << message.toStdString() << '\n'; });
    check(canvas.insertInlineImage(picture(),QStringLiteral("Cell picture")),
          "Insert Picture rejects a real PNG at the active table-cell caret");
    const auto after=canvas.snapshot().document;
    check(after.findTable(tableId)->cell(0,0)->text == std::u16string(1,docxstudio::core::kInlineObjectReplacementCharacter),
          "table picture is flattened to text rather than one inline atom");
    canvas.undo();
    check(canvas.snapshot().document==before,"Undo did not restore the exact pre-picture table");
    canvas.redo();
    check(canvas.snapshot().document==after,"Redo did not restore the exact table picture");
    if (application.arguments().contains(QStringLiteral("--recovery-only"))) {
        std::string error;
        const auto encoded=docxstudio::app::RecoveryCodec::encode({after,{}},error);
        check(bool(encoded),"recovery encode rejected table picture");
        const auto decoded=docxstudio::app::RecoveryCodec::decode(*encoded,error);
        if (!decoded) std::cerr << error << '\n';
        check(decoded && decoded->document==after,"recovery lost table picture identity/payload/geometry");
        auto json=docxstudio::codex::Json::parse(*encoded);
        check(json.at("version")==15,"pictured table recovery did not use its compatible schema15");
        json["version"]=14;
        check(!docxstudio::app::RecoveryCodec::decode(json.dump(),error),"schema14 accepted unsupported table picture atoms");
        const auto ordinary=docxstudio::app::RecoveryCodec::encode({before,{}},error);
        check(ordinary && docxstudio::codex::Json::parse(*ordinary).at("version")==14,
              "ordinary table recovery unnecessarily changed its schema version");
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--codex-only"))) {
        QString summary, error;
        const auto* cell=after.findTable(tableId)->cell(0,0);
        const auto result=docxstudio::app::invokeEditorTool(canvas,QStringLiteral("table-images"),
            QString::fromLatin1(docxstudio::codex::kEditorReadTool.data(),
                static_cast<qsizetype>(docxstudio::codex::kEditorReadTool.size())),
            {{"documentId","table-images"},{"scope","blocks"},
             {"tableCellTargets",{{{"tableId",tableId.toString()},{"cellId",cell->id.toString()}}}}},summary,error);
        check(error.isEmpty() && result.at("blocks").size()==1,"Codex cell picture read failed");
        const auto& attributes=result.at("blocks").at(0).at("attributes");
        check(attributes.contains("images") && attributes.at("images").size()==1,
              "Codex cell read exposes a bare placeholder without picture metadata");
        const auto& image=attributes.at("images").at(0);
        check(image.at("id")==cell->images.front().id.toString() && image.at("utf16Offset")==0 &&
              image.at("mimeType")=="image/png" && image.at("widthEmu")==cell->images.front().width_emu &&
              image.at("encodedBytes")==cell->images.front().encoded_payload.size(),
              "Codex cell image metadata lost identity, geometry or payload size");
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-only"))) {
        const auto imageId=after.findTable(tableId)->cell(0,0)->images.front().id;
        check(canvas.selectedInlineImageId()==imageId && canvas.selectInlineImage(imageId),
              "a table-cell picture cannot be selected for normal Picture controls");
        check(canvas.resizeSelectedInlineImage(35,21),"selected table picture cannot be resized");
        check(canvas.setSelectedImageAccessibleName(QStringLiteral("Edited cell alt")),"selected table picture alt text cannot be edited");
        const auto resized=canvas.snapshot().document;
        check(resized.findImage(imageId)->width_emu==444500,"table picture size lost its EMU geometry");
        check(canvas.deleteSelectedInlineImage() && !canvas.snapshot().document.findImage(imageId),
              "Delete Picture does not remove the selected cell atom");
        canvas.undo();
        check(canvas.snapshot().document==resized,"cell picture deletion Undo loses exact metadata");
        check(canvas.activateTableCell(tableId,0,1,0),"picture click deselection fixture");
        const auto bounds=pictureBounds(canvasRaster(canvas));
        check(bounds.isValid(),"table picture click fixture has no visible raster");
        QTest::mouseClick(canvas.viewport(),Qt::LeftButton,Qt::NoModifier,bounds.center());
        check(canvas.selectedInlineImageId()==imageId,"clicking the cell picture does not select its image atom");
        bool foundLayout=false,layoutDisabled=false;
        QTimer::singleShot(0,&canvas,[&] {
            for(auto* widget:QApplication::topLevelWidgets()) if(auto* menu=qobject_cast<QMenu*>(widget);menu && menu->isVisible()) {
                if(auto* action=menu->findChild<QAction*>(QStringLiteral("context.pictureLayout"))) {
                    foundLayout=true; layoutDisabled=!action->isEnabled();
                }
                menu->close();
            }
        });
        QContextMenuEvent context(QContextMenuEvent::Mouse,bounds.center(),canvas.viewport()->mapToGlobal(bounds.center()));
        QApplication::sendEvent(canvas.viewport(),&context);
        check(foundLayout && layoutDisabled,"cell picture context menu exposes unsupported floating layout controls");
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--clipboard-only"))) {
        const auto original=after.findTable(tableId)->cell(0,0)->images.front();
        check(canvas.selectTableCells(tableId,0,0,0,0),"cell clipboard selection");
        canvas.copy();
        check(canvas.activateTableCell(tableId,0,1,0),"cell clipboard destination");
        const auto beforePaste=canvas.snapshot().document;
        canvas.paste();
        const auto pastedSnapshot=canvas.snapshot();
        const auto* pasted=pastedSnapshot.document.findTable(tableId)->cell(0,1);
        check(pasted->images.size()==1 && pasted->images.front().encoded_payload==original.encoded_payload &&
              pasted->images.front().id!=original.id && pasted->images.front().width_emu==original.width_emu,
              "copying and pasting a table cell loses its picture payload or reuses its identity");
        canvas.undo();
        check(canvas.snapshot().document==beforePaste,"table-cell picture paste is not one atomic Undo transaction");
        check(canvas.activateTableCell(tableId,0,0,1),"mixed cell clipboard fixture");
        canvas.insertText(QStringLiteral(" middle "));
        check(canvas.insertInlineImage(picture("JPEG"),QStringLiteral("JPEG clipboard picture")),"mixed cell JPEG fixture");
        const auto mixedSource=canvas.snapshot().document;
        const auto* mixed=mixedSource.findTable(tableId)->cell(0,0);
        check(mixed->text==u"\ufffc middle \ufffc","typing prose beside a cell picture was rejected or changed its ordering");
        check(canvas.activateTableCell(tableId,0,0,mixed->text.size()),"mixed range clipboard caret");
        QTest::keyClick(&canvas,Qt::Key_Home,Qt::ShiftModifier);
        check(!canvas.selectedText().isEmpty(),"mixed range clipboard selection");
        canvas.copy();
        check(canvas.activateTableCell(tableId,0,1,0),"mixed range clipboard destination");
        canvas.paste();
        const auto mixedPasted=canvas.snapshot().document;
        const auto* mixedCopy=mixedPasted.findTable(tableId)->cell(0,1);
        check(mixedCopy->text==mixed->text && mixedCopy->images.size()==2 &&
              mixedCopy->images[0].encoded_payload==mixed->images[0].encoded_payload &&
              mixedCopy->images[1].encoded_payload==mixed->images[1].encoded_payload &&
              mixedCopy->images[0].id!=mixed->images[0].id && mixedCopy->images[1].id!=mixed->images[1].id,
              "mixed prose and multiple picture clipboard loses order or encoded content");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"mixed picture range paste is not one Undo transaction");
        check(canvas.activateTableCell(tableId,0,0,mixed->text.size()),"mixed Cut selection caret");
        QTest::keyClick(&canvas,Qt::Key_Home,Qt::ShiftModifier);
        canvas.cut();
        check(canvas.snapshot().document.findTable(tableId)->cell(0,0)->text.empty(),"mixed picture range Cut loses atomic deletion");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"mixed picture Cut Undo loses identity or bytes");
        docxstudio::app::DocumentCanvas other(spelling);
        other.paste();
        const auto copiedDocument=other.snapshot().document;
        check(copiedDocument.tables().size()==1 && copiedDocument.tables().front().cell(0,0)->text==mixed->text &&
              copiedDocument.tables().front().cell(0,0)->images.size()==2,
              "pasting pictured cell content into the body loses its native table or image atoms");
        const auto copiedImage=copiedDocument.tables().front().cell(0,0)->images.front().id;
        check(other.selectInlineImage(copiedImage),"cross-document picture selection");
        other.copy();
        check(canvas.activateTableCell(tableId,0,1,0),"native picture clipboard cell destination");
        canvas.paste();
        const auto nativePasted=canvas.snapshot().document;
        const auto* nativeCopy=nativePasted.findTable(tableId)->cell(0,1);
        check(nativeCopy->images.size()==1 && nativeCopy->images.front().encoded_payload==mixed->images.front().encoded_payload &&
              nativeCopy->images.front().id!=copiedImage && nativeCopy->images.front().layout==docxstudio::core::ImageLayout{},
              "native individual-picture clipboard cannot paste into a table cell");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"native picture cell paste is not one Undo transaction");
        check(canvas.activateTableCell(tableId,0,0,0),"pictured cell ordinary typing caret");
        canvas.insertText(QStringLiteral("x"));
        check(canvas.snapshot().document.findTable(tableId)->cell(0,0)->text==u"x"+mixed->text,
              "ordinary typing beside existing cell pictures was rejected");
        QTest::keyClick(&canvas,Qt::Key_Backspace);
        check(canvas.snapshot().document==mixedSource,"Backspace beside cell pictures changed surviving atoms");
        check(canvas.activateTableCell(tableId,0,0,1),"pictured cell Delete caret");
        QTest::keyClick(&canvas,Qt::Key_Delete);
        const auto deletedProse=canvas.snapshot().document;
        check(deletedProse.findTable(tableId)->cell(0,0)->images.size()==2 &&
              deletedProse.findTable(tableId)->cell(0,0)->images[1].utf16_offset==mixed->images[1].utf16_offset-1,
              "Delete beside cell pictures loses or fails to shift surviving atoms");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"Delete prose Undo changed cell picture identity");
        QApplication::clipboard()->setText(QStringLiteral("Text "));
        check(canvas.activateTableCell(tableId,0,0,0),"pictured cell plain paste caret");
        canvas.paste();
        check(canvas.snapshot().document.findTable(tableId)->cell(0,0)->text==u"Text "+mixed->text,
              "plain text paste beside cell pictures was rejected");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"plain paste Undo changed cell pictures");
        check(canvas.activateTableCell(tableId,0,0,mixed->text.size()),"whole pictured cell replacement caret");
        QTest::keyClick(&canvas,Qt::Key_Home,Qt::ShiftModifier);
        canvas.insertText(QStringLiteral("Replacement"));
        const auto replaced=canvas.snapshot().document;
        check(replaced.findTable(tableId)->cell(0,0)->text==u"Replacement" && replaced.findTable(tableId)->cell(0,0)->images.empty(),
              "replacing the whole cell did not remove exactly the selected pictures");
        canvas.undo(); check(canvas.snapshot().document==mixedSource,"whole cell replacement Undo lost picture payloads");
        check(canvas.selectTableCells(tableId,0,0,0,0),"pictured fragment caret clipboard selection");
        canvas.copy();
        check(canvas.activateTableCell(tableId,0,1,0),"pictured fragment existing suffix fixture");
        canvas.insertText(QStringLiteral("TAIL"));
        check(canvas.activateTableCell(tableId,0,1,0),"pictured fragment insertion before suffix");
        canvas.paste();
        canvas.insertText(QStringLiteral("!"));
        check(canvas.snapshot().document.findTable(tableId)->cell(0,1)->text==mixed->text+u"!TAIL",
              "paste leaves the cell caret after unrelated suffix text instead of the pasted pictures");
        rectangularClipboard(spelling);
        return 0;
    }
    const auto raster=canvasRaster(canvas);
    const auto screenBounds=pictureBounds(raster);
    check(screenBounds.width()*screenBounds.height()>100,"inline table picture has no rendered raster pixels");
    const auto pdf=privateSettings.filePath(QStringLiteral("table-picture.pdf"));
    QString pdfError;
    check(canvas.exportPdf(pdf,pdfError),"table picture PDF export failed");
    QProcess renderer;
    const auto pdfRoot=privateSettings.filePath(QStringLiteral("table-picture-page"));
    renderer.start(QStringLiteral("/usr/bin/pdftoppm"),{QStringLiteral("-f"),QStringLiteral("1"),
        QStringLiteral("-singlefile"),QStringLiteral("-png"),QStringLiteral("-r"),QStringLiteral("96"),pdf,pdfRoot});
    check(renderer.waitForStarted(5000) && renderer.waitForFinished(15000) && renderer.exitCode()==0,
          "table picture PDF rasterization failed");
    const QImage pdfRaster(pdfRoot+QStringLiteral(".png"));
    const auto pdfBounds=pictureBounds(pdfRaster);
    check(pdfBounds.isValid() && std::abs(pdfBounds.width()-screenBounds.width())<=3 &&
          std::abs(pdfBounds.height()-screenBounds.height())<=3,"table picture screen and PDF geometry diverge");
    if(qEnvironmentVariableIsSet("OWL_DOCS_CAPTURE_TABLE_IMAGES")) {
        privateSettings.setAutoRemove(false);
        raster.save(privateSettings.filePath(QStringLiteral("table-picture-canvas.png")));
        std::cout << "Table image captures: " << privateSettings.path().toStdString() << '\n';
    }
    std::cout << "Table image UI tests passed\n";
}
