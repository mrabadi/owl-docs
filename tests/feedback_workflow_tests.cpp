#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/RibbonWidget.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QColorDialog>
#include <QBuffer>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value,const char* message) { if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } }
void tableWorkflow() {
    using namespace docxstudio;
    app::MainWindow window;
    window.show();
    QApplication::processEvents();
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    check(canvas,"table workflow canvas");
    auto* insert=window.findChild<QAction*>("insert.table");
    check(insert,"table command");
    const auto before=canvas->snapshot();
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("insertTableDialog");
        auto* grid=dialog ? dialog->findChild<QTableWidget*>("insertTable.grid") : nullptr;
        check(grid,"Insert Table command does not expose the dimension grid");
        const auto rect=grid->visualItemRect(grid->item(2,3));
        QTest::mouseMove(grid->viewport(),rect.center());
        check(canvas->snapshot().revision==before.revision,"table hover mutates the document");
        QTest::mouseClick(grid->viewport(),Qt::LeftButton,Qt::NoModifier,rect.center());
    });
    insert->trigger();
    const auto after=canvas->snapshot();
    check(after.document.tables().size()==1 && after.document.tables()[0].rowCount()==3 &&
        after.document.tables()[0].columnCount()==4,"one grid click did not insert the selected table");
    canvas->undo();
    check(canvas->snapshot().document==before.document,"table grid insertion is not one Undo transaction");
    QTimer::singleShot(0,&window,[&] { window.findChild<QDialog*>("insertTableDialog")->reject(); });
    insert->trigger();
    check(canvas->snapshot().document==before.document,"cancelling table picker mutates the document");
    canvas->markSaved();
}
void equationWorkflow() {
    using namespace docxstudio;
    app::MainWindow window;
    window.resize(1280,860);
    window.show();
    QApplication::processEvents();
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    check(canvas && canvas->insertEquation(QStringLiteral("x^2"),false),"equation workflow fixture");
    const auto equation=canvas->snapshot().document.paragraphs().front().equations().front();
    check(canvas->selectEquation(equation.id),"equation selection fixture");
    auto* display=window.findChild<QAction*>("equation.display");
    auto* inlineAction=window.findChild<QAction*>("equation.inline");
    auto* remove=window.findChild<QAction*>("equation.delete");
    check(display && inlineAction && remove && display->isEnabled(),"selected equations have no connected direct commands");
    auto* page=window.findChild<QWidget*>("ribbon.equationTab");
    check(page && page->isVisible(),"MainWindow does not activate the equation ribbon");
    display->trigger();
    auto changed=canvas->snapshot().document.findEquation(equation.id);
    check(changed && changed->display && changed->canonical_latex==equation.canonical_latex,"display command loses equation identity or content");
    canvas->undo();
    check(!canvas->snapshot().document.findEquation(equation.id)->display,"equation placement Undo");
    check(canvas->selectEquation(equation.id),"equation selection after Undo");
    remove->trigger();
    check(!canvas->snapshot().document.findEquation(equation.id),"direct equation Delete is not connected");
    canvas->undo();
    check(canvas->snapshot().document.findEquation(equation.id),"equation Delete Undo");
    check(canvas->selectEquation(equation.id),"equation tab-restore fixture");
    canvas->markSaved();
    auto* tabs=window.findChild<QTabWidget*>("documentTabs");
    const int originalTab=tabs->currentIndex();
    window.findChild<QAction*>("file.new")->trigger();
    check(!page->isVisible() && !display->isEnabled(),"new document retains another tab's equation controls");
    tabs->setCurrentIndex(originalTab);
    check(page->isVisible() && inlineAction->isChecked() && !display->isChecked(),
        "restoring an equation tab loses its contextual controls or placement");
}
void cellPictureContextWorkflow() {
    using namespace docxstudio;
    app::MainWindow window;
    window.resize(820,600);window.show();
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    check(canvas && canvas->insertTable(1,1,false),"cell picture context table fixture");
    const auto table=canvas->snapshot().document.tables().front().id();
    check(canvas->activateTableCell(table,0,0,0),"cell picture context caret fixture");
    QImage picture(40,24,QImage::Format_RGB32);picture.fill(Qt::green);
    QByteArray bytes;QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && picture.save(&buffer,"PNG"),"cell picture context PNG");
    check(canvas->insertInlineImage({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        reinterpret_cast<const std::uint8_t*>(bytes.constData())+bytes.size()},QStringLiteral("Cell picture")),
        "cell picture context insertion");
    QApplication::processEvents();
    auto* page=window.findChild<QWidget*>("ribbon.pictureTab");
    check(page && page->isVisible() && window.findChild<QAction*>("picture.size")->isEnabled() &&
        window.findChild<QAction*>("picture.altText")->isEnabled() && window.findChild<QAction*>("picture.delete")->isEnabled() &&
        !window.findChild<QAction*>("picture.wrapSquare")->isEnabled() &&
        !window.findChild<QAction*>("picture.wrapTopBottom")->isEnabled() &&
        !window.findChild<QAction*>("picture.layoutOptions")->isEnabled(),
        "cell picture insertion hides its direct controls or offers unsupported floating layouts");
    canvas->markSaved();
    auto* tabs=window.findChild<QTabWidget*>("documentTabs");const int original=tabs->currentIndex();
    window.findChild<QAction*>("file.new")->trigger();
    check(!page->isVisible(),"new tab retains cell picture controls");
    tabs->setCurrentIndex(original);
    check(page->isVisible() && !window.findChild<QAction*>("picture.wrapSquare")->isEnabled(),
        "tab restore loses table picture capabilities");
}
void automaticColorWorkflow() {
    using namespace docxstudio;
    app::MainWindow window;
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    auto* ribbon=window.findChild<app::RibbonWidget*>();
    check(canvas && ribbon,"automatic color fixture");
    canvas->applyParagraphStyle(QStringLiteral("Title"));
    canvas->insertText(QStringLiteral("Styled title"));
    canvas->selectAll();
    const auto inherited=canvas->currentTextColor();
    canvas->setForeground(QColor(Qt::red));
    check(canvas->currentTextColor()==QColor(Qt::red),"direct color fixture");
    ribbon->textColorSelected(QColor{});
    check(canvas->currentTextColor()==inherited,"Automatic ignores the selection or hardcodes a color instead of restoring its style");
    check(!canvas->snapshot().document.paragraphs().front().styleOverrideMaskAt(1).foreground_argb,
        "Automatic leaves a direct color override that blocks later style changes");
    canvas->undo();
    check(canvas->currentTextColor()==QColor(Qt::red),"Automatic color is not one undoable change");
    canvas->redo();
    canvas->applyParagraphStyle(QStringLiteral("Heading2"));
    const auto* heading=core::findBuiltInParagraphStyle("Heading2");
    check(heading && canvas->currentTextColor().rgba()==heading->character_format.foreground_argb.value_or(0xff000000U),
        "Automatic color prevents a later style from changing the inherited color");
    canvas->selectAll();
    canvas->setForeground(QColor(Qt::red));
    canvas->toggleUnderline();
    QTest::keyClick(canvas,Qt::Key_Home,Qt::ControlModifier);
    for(int key=0;key<4;++key) QTest::keyClick(canvas,Qt::Key_Right,Qt::ShiftModifier);
    ribbon->textColorSelected(QColor{});
    const auto partial=canvas->snapshot();
    const auto& paragraph=partial.document.paragraphs().front();
    check(paragraph.characterFormatAt(1).foreground_argb==heading->character_format.foreground_argb &&
        paragraph.characterFormatAt(5).foreground_argb==0xffff0000U &&
        paragraph.characterFormatAt(1).underline==core::UnderlineStyle::single &&
        paragraph.characterFormatAt(5).underline==core::UnderlineStyle::single &&
        !paragraph.styleOverrideMaskAt(1).foreground_argb && paragraph.styleOverrideMaskAt(5).foreground_argb,
        "Automatic changes unselected text or discards unrelated emphasis");
    canvas->markSaved();
}
void customColorWorkflow() {
    using namespace docxstudio;
    QSettings settings;
    settings.remove(QStringLiteral("palette/text/recentColors"));
    app::MainWindow window;
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    auto* ribbon=window.findChild<app::RibbonWidget*>();
    canvas->insertText(QStringLiteral("Color choice"));
    canvas->selectAll();
    ribbon->textColorRequested();
    auto* picker=window.findChild<QColorDialog*>(QStringLiteral("textColorPicker"));
    check(picker,"custom color dialog");
    picker->setCurrentColor(QColor(QStringLiteral("#123456")));
    picker->setCurrentColor(QColor(QStringLiteral("#234567")));
    check(settings.value(QStringLiteral("palette/text/recentColors")).toStringList().isEmpty(),
        "live color previews pollute Recent Colors");
    picker->close();
    QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QApplication::processEvents();
    const auto recent=settings.value(QStringLiteral("palette/text/recentColors")).toStringList();
    check(recent.size()==1 && recent.front()==QStringLiteral("#ff234567"),
        "closing the custom picker does not retain exactly the committed color");
    canvas->markSaved();
}
void marginSaveWorkflow() {
    using namespace docxstudio;
    app::MainWindow window;
    auto* canvas=window.findChild<app::DocumentCanvas*>();
    canvas->insertText(QStringLiteral("Body preserved through margin changes"));
    const auto header=QStringLiteral("Header first\nHeader second\tCentered\tRight");
    const auto footer=QStringLiteral("Footer first\nFooter second\t\tPage {PAGE} of {PAGES}");
    check(canvas->setHeaderFooterText(header,footer),"margin save story fixture");
    canvas->setMarginsPoints(36,36,36,36);
    QTemporaryDir output;
    const auto path=output.filePath(QStringLiteral("margins.docx"));
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QFileDialog*>();
        check(dialog,"margin Save As dialog");
        dialog->selectFile(path);
        check(QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection),"margin Save As destination");
    });
    window.findChild<QAction*>("file.saveAs")->trigger();
    check(QFileInfo::exists(path),"margin document save");
    app::MainWindow reopened;
    check(reopened.openPath(path),"margin document reopen");
    auto* tabs=reopened.findChild<QTabWidget*>("documentTabs");
    auto* restored=qobject_cast<app::DocumentCanvas*>(tabs->currentWidget());
    check(restored && restored->headerText()==header && restored->footerText()==footer && restored->marginTopPoints()==36,
        "margin save/reopen changes story content or requested margins");
    restored->setMarginsPoints(54,54,54,54);
    reopened.findChild<QAction*>("file.save")->trigger();
    app::MainWindow again;
    check(again.openPath(path),"second margin document reopen");
    auto* againCanvas=qobject_cast<app::DocumentCanvas*>(again.findChild<QTabWidget*>("documentTabs")->currentWidget());
    check(againCanvas && againCanvas->headerText()==header && againCanvas->footerText()==footer && againCanvas->marginTopPoints()==54,
        "repeated margin save/reopen corrupts header/footer content");
}
}
int main(int argc,char** argv) {
    QTemporaryDir state;
    qputenv("XDG_DATA_HOME",state.path().toUtf8());
    qputenv("XDG_CONFIG_HOME",state.path().toUtf8());
    QApplication application(argc,argv);
    QStandardPaths::setTestModeEnabled(true);
    if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("table")) tableWorkflow();
    else if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("equation")) equationWorkflow();
    else if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("automatic")) automaticColorWorkflow();
    else if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("margins")) marginSaveWorkflow();
    else if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("custom")) customColorWorkflow();
    else if (argc>1 && QString::fromLocal8Bit(argv[1])==QStringLiteral("cell-context")) cellPictureContextWorkflow();
    else { tableWorkflow(); equationWorkflow(); automaticColorWorkflow(); marginSaveWorkflow(); customColorWorkflow(); cellPictureContextWorkflow(); }
    std::cout << "Feedback workflows passed\n";
}
