#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/NavigationDock.h"
#include "docxstudio/app/ChatDock.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QImage>
#include <QHBoxLayout>
#include <QTimer>
#include <QMenu>
#include <QPalette>
#include <QListWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>
#include <QStyleOptionComboBox>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void testCompactWritingBar() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"format.bold", "format.italic", "format.underline",
             "paragraph.bullets", "paragraph.numbering", "paragraph.alignLeft",
             "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
             "format.strike", "format.superscript", "format.subscript"}) {
        commands.add(id, id, QKeySequence(), [] {});
    }
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(760, 84);
    bar.show();
    QApplication::processEvents();
    auto* tabs = bar.findChild<QTabWidget*>("ribbon.tabs");
    check(tabs && !tabs->tabBar()->isVisible(),
          "writing bar still requires switching ribbon tabs");
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    check(more && more->isVisible() && more->menu() &&
              !more->menu()->actions().empty(),
          "secondary formatting has no labelled overflow menu");
    for (const auto* id : {"format.bold", "format.italic", "format.underline"}) {
        auto* button = bar.findChild<QToolButton*>(
            QStringLiteral("ribbonButton.%1").arg(id));
        check(button && button->isVisible() &&
                  bar.rect().contains(button->mapTo(&bar, button->rect().center())),
              "essential formatting is clipped at laptop width");
    }
}

void testContextsExpandBelowPrimary() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"table.insertRowAbove", "table.insertRowBelow", "table.insertColumnLeft",
             "table.insertColumnRight", "table.deleteRows", "table.deleteColumns", "picture.size",
             "picture.altText", "picture.layoutOptions", "picture.wrapInline", "picture.wrapSquare",
             "picture.wrapTopBottom", "picture.delete", "paragraph.bullets", "paragraph.numbering",
             "paragraph.decreaseIndent", "paragraph.increaseIndent", "paragraph.listProperties"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(820, 180);
    bar.show();
    const auto visibleBelow = [&](const char* name, const char* command) {
        QApplication::processEvents();
        bar.resize(bar.width(), std::max(180, bar.sizeHint().height()));
        QApplication::processEvents();
        auto* home = bar.findChild<QWidget*>("workspace.homeControls");
        auto* page = bar.findChild<QWidget*>(QString::fromUtf8(name));
        auto* button = page ? page->findChild<QToolButton*>(QString::fromUtf8(command)) : nullptr;
        check(home && home->isVisible() && page && page->isVisible() && button && button->isVisible() &&
                  page->mapToGlobal(QPoint()).y() >= home->mapToGlobal(home->rect().bottomLeft()).y(),
              "selection controls require an object menu or replace the primary formatting row");
        check(bar.rect().contains(QRect(button->mapTo(&bar, QPoint()), button->size())),
              "expanded contextual command is clipped");
        for (auto* control : page->findChildren<QToolButton*>()) {
            if (!control->isVisible()) continue;
            if (!bar.rect().contains(QRect(control->mapTo(&bar, QPoint()), control->size()))) {
                const auto position = control->mapTo(&bar, QPoint());
                std::cerr << "clipped " << control->objectName().toStdString() << " at " << position.x() << ',' << position.y()
                          << " size " << control->width() << 'x' << control->height() << " in " << bar.width() << 'x' << bar.height()
                          << " page " << page->width() << 'x' << page->height() << " preferred " << bar.sizeHint().height() << '\n';
                bar.grab().save(QStringLiteral("/tmp/owldocs-table-ribbon-clipped.png"));
            }
            check(bar.rect().contains(QRect(control->mapTo(&bar, QPoint()), control->size())),
                  "an expanded contextual control is clipped at a supported width");
        }
        for (const auto* wrapper : {"workspace.table", "workspace.picture", "workspace.list"}) {
            auto* redundant = bar.findChild<QToolButton*>(QString::fromUtf8(wrapper));
            check(!redundant || !redundant->isVisible(), "context still requires a redundant object dropdown");
        }
    };
    bar.setTableContext(true);
    visibleBelow("ribbon.tableTab", "ribbonButton.table.alignment");
    bar.resize(420, 180);
    visibleBelow("ribbon.tableTab", "ribbonButton.table.alignment");
    bar.resize(820, 180);
    bar.setTableContext(false);
    bar.setPictureContext(true);
    visibleBelow("ribbon.pictureTab", "ribbonButton.picture.size");
    bar.setTableContext(true);
    visibleBelow("ribbon.pictureTab", "ribbonButton.picture.size");
    bar.setTableContext(false);
    bar.setPictureContext(false);
    bar.setListContext(true, 2);
    visibleBelow("ribbon.listTab", "ribbonButton.paragraph.increaseIndent");
    bar.resize(420, 180);
    visibleBelow("ribbon.listTab", "ribbonButton.paragraph.increaseIndent");
    bar.setListContext(false, 1);
    QApplication::processEvents();
    check(bar.findChild<QWidget*>("workspace.homeControls")->isVisible(), "leaving context hides primary formatting");
}

void testPictureWrapChoicesAreDirect() {
    docxstudio::app::CommandRegistry commands;
    int squareRequests = 0;
    for (const auto* id : {"picture.size", "picture.altText", "picture.layoutOptions", "picture.wrapInline",
                          "picture.wrapSquare", "picture.wrapTopBottom", "picture.delete", "picture.editExcalidraw"})
        commands.add(id, id, QKeySequence(), [&, id] { if (QString::fromLatin1(id) == "picture.wrapSquare") ++squareRequests; });
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(420, 180);
    bar.show();
    bar.setPictureContext(true, docxstudio::core::ImagePlacement::square);
    QApplication::processEvents();
    auto* page = bar.findChild<QWidget*>("ribbon.pictureTab");
    for (const auto* id : {"picture.wrapInline", "picture.wrapSquare", "picture.wrapTopBottom"}) {
        auto* button = page->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id));
        check(button && button->isVisible() && bar.rect().contains(QRect(button->mapTo(&bar, QPoint()), button->size())),
              "picture wrapping still requires opening a Wrap Text dropdown");
    }
    const auto iconImage = [&](const char* id) {
        return page->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id))->icon().pixmap(22, 22).toImage();
    };
    check(iconImage("picture.wrapInline") != iconImage("picture.wrapSquare") &&
              iconImage("picture.wrapSquare") != iconImage("picture.wrapTopBottom") &&
              iconImage("picture.wrapInline") != iconImage("picture.wrapTopBottom"),
          "direct wrapping choices use indistinguishable icons");
    page->findChild<QToolButton*>("ribbonButton.picture.wrapSquare")->click();
    check(squareRequests == 1, "direct picture wrap choice does not invoke its command");
}

void testEquationAndCellPictureContexts() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"equation.edit", "equation.inline", "equation.display", "equation.delete",
                          "picture.size", "picture.altText", "picture.layoutOptions", "picture.wrapInline",
                          "picture.wrapSquare", "picture.wrapTopBottom", "picture.delete"})
        commands.add(id, id, QKeySequence(), [] {});
    for (const auto* id : {"equation.inline", "equation.display"}) commands.action(id)->setCheckable(true);
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(420, 180);
    bar.show();
    bar.setEquationContext(true, true);
    QApplication::processEvents();
    auto* page = bar.findChild<QWidget*>("ribbon.equationTab");
    check(page && page->isVisible(), "selected equations have no expanded equation controls");
    for (const auto* id : {"equation.edit", "equation.inline", "equation.display", "equation.delete"}) {
        auto* button = page->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id));
        check(button && button->isVisible() && button->isEnabled() &&
                  bar.rect().contains(QRect(button->mapTo(&bar, QPoint()), button->size())),
              "equation edit, placement or delete requires a wrapper menu");
    }
    check(commands.action("equation.display")->isChecked() && !commands.action("equation.inline")->isChecked(),
          "equation controls do not reflect display placement");
    bar.setEquationContext(false);
    bar.setPictureContext(true, docxstudio::core::ImagePlacement::inline_with_text, false, false);
    QApplication::processEvents();
    check(!page->isVisible() && !commands.action("equation.edit")->isEnabled(),
          "equation controls remain active after leaving the selection");
    check(commands.action("picture.size")->isEnabled() && commands.action("picture.altText")->isEnabled() &&
              commands.action("picture.delete")->isEnabled() && commands.action("picture.wrapInline")->isEnabled() &&
              !commands.action("picture.wrapSquare")->isEnabled() && !commands.action("picture.wrapTopBottom")->isEnabled() &&
              !commands.action("picture.layoutOptions")->isEnabled(),
          "cell-picture controls offer unsupported floating layouts or disable supported edits");
}

void testRibbonKeyboardActivation() {
    docxstudio::app::CommandRegistry commands;
    int boldRequests = 0;
    int rowRequests = 0;
    commands.add("format.bold", "Bold", QKeySequence(), [&] { ++boldRequests; });
    commands.add("format.italic", "Italic", QKeySequence(), [] {});
    commands.add("table.insertRowAbove", "Insert row above", QKeySequence(), [] {});
    commands.add("table.insertRowBelow", "Insert row below", QKeySequence(), [&] { ++rowRequests; });
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(1280, 180);
    bar.show();
    QApplication::processEvents();
    auto* size = bar.findChild<QComboBox*>("ribbon.fontSize");
    auto* bold = bar.findChild<QToolButton*>("ribbonButton.format.bold");
    size->setFocus();
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    check(QApplication::focusWidget() == bold, "Tab skips the direct primary formatting buttons");
    QTest::keyClick(bold, Qt::Key_Return);
    check(boldRequests == 1, "Enter does not activate the focused formatting button");
    bar.setTableContext(true);
    QApplication::processEvents();
    auto* insert = bar.findChild<QToolButton*>("ribbonButton.table.insert");
    auto* remove = bar.findChild<QToolButton*>("ribbonButton.table.delete");
    check(insert && remove, "table insertion/deletion has no dropdown controls");
    insert->setFocus();
    QTest::keyClick(insert, Qt::Key_Tab);
    check(QApplication::focusWidget() == remove, "Tab skips contextual dropdowns");
    QTimer::singleShot(0, insert->menu(), [&] {
        check(insert->menu()->isVisible(), "Enter does not open the table insertion dropdown");
        insert->menu()->setActiveAction(commands.action("table.insertRowBelow"));
        QTest::keyClick(insert->menu(), Qt::Key_Return);
    });
    insert->setFocus();
    QTest::keyClick(insert, Qt::Key_Return);
    check(rowRequests == 1, "Enter does not execute the chosen table insertion once");
    auto* styles = bar.findChild<QToolButton*>("ribbonButton.table.styles");
    bool opened = false;
    QObject::connect(styles->menu(), &QMenu::aboutToShow, [&] { opened = true; });
    QTimer::singleShot(0, styles->menu(), &QMenu::hide);
    styles->setFocus();
    QTest::keyClick(styles, Qt::Key_Return);
    check(opened, "Enter does not open the focused table-style chooser");
}

void testMoreContainsOnlyOverflow() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"format.bold", "format.italic", "format.underline", "format.strike",
             "format.superscript", "format.subscript", "paragraph.bullets", "paragraph.numbering",
             "paragraph.alignLeft", "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
             "paragraph.decreaseIndent", "paragraph.increaseIndent", "paragraph.listProperties"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.show();
    auto* home = bar.findChild<QWidget*>("workspace.homeControls");
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    for (const int width : {1280, 820, 420}) {
        bar.resize(width, 180);
        QApplication::processEvents();
        for (const auto* id : {"format.bold", "format.italic", "format.underline", "format.strike",
                 "format.superscript", "format.subscript", "paragraph.bullets", "paragraph.numbering",
                 "paragraph.alignLeft", "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
                 "paragraph.decreaseIndent", "paragraph.increaseIndent"}) {
            auto* button = home->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id));
            check(button && (button->isVisible() != more->menu()->actions().contains(commands.action(id))),
                  "More duplicates a direct command or omits an overflowed command");
            if (width == 1280) check(button->isVisible(), "a primary action is hidden despite available writing-row space");
            if (button->isVisible()) check(bar.rect().contains(QRect(button->mapTo(&bar, QPoint()), button->size())),
                                           "a direct primary command is clipped");
        }
        for (const auto* name : {"ribbonButton.format.textColor", "ribbonButton.format.highlightColor"}) {
            auto* color = home->findChild<QToolButton*>(QString::fromLatin1(name));
            if (color->isVisible()) check(bar.rect().contains(QRect(color->mapTo(&bar, QPoint()), color->size())),
                                           "a visible color chooser is clipped");
            check(color->isVisible() != more->menu()->actions().contains(color->menu()->menuAction()),
                  "More duplicates a directly available color chooser");
        }
        check(!more->menu()->actions().contains(commands.action("paragraph.listProperties")),
              "More contains a contextual command unrelated to spatial overflow");
        if (more->isVisible()) check(bar.rect().contains(QRect(more->mapTo(&bar, QPoint()), more->size())),
                                      "the overflow button itself is clipped");
        check(more->isVisible() == !more->menu()->actions().empty(), "More remains visible without actual overflow");
        if (width == 1280) check(!more->isVisible(), "More is shown when all primary controls fit");
        if (width == 820) check(home->findChild<QToolButton*>("ribbonButton.format.textColor")->isVisible(),
                                "font color is hidden despite ordinary laptop writing-row space");
    }
}

void testContextChangesPreserveIndentAccess() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"paragraph.decreaseIndent", "paragraph.increaseIndent", "picture.size", "equation.edit"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(1280, 180);
    bar.show();
    bar.setListContext(true, 2);
    bar.setPictureContext(true);
    QApplication::processEvents();
    auto* home = bar.findChild<QWidget*>("workspace.homeControls");
    auto* list = bar.findChild<QWidget*>("ribbon.listTab");
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    for (const auto* id : {"paragraph.decreaseIndent", "paragraph.increaseIndent"}) {
        auto* button = home->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id));
        check(!list->isVisible() && (button->isVisible() || more->menu()->actions().contains(commands.action(id))),
              "switching from list to another object hides indentation from both writing row and overflow");
    }
    bar.setPictureContext(false);
    bar.setEquationContext(true);
    QApplication::processEvents();
    check(home->findChild<QToolButton*>("ribbonButton.paragraph.increaseIndent")->isVisible(),
          "equation context does not restore ordinary primary indentation");
}

void testEditableFigureContext() {
    docxstudio::app::CommandRegistry commands;
    commands.add("picture.editExcalidraw", "Edit Figure", QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(820, 180);
    bar.show();
    bar.setPictureContext(true, docxstudio::core::ImagePlacement::inline_with_text, true);
    QApplication::processEvents();
    auto* label = bar.findChild<QLabel*>("ribbon.pictureContextLabel");
    auto* edit = bar.findChild<QToolButton*>("ribbonButton.picture.editExcalidraw");
    check(label && label->isVisible() && label->text() == "Figure" && edit && edit->isVisible(),
          "editable local figures do not identify their context or expose direct Edit Figure");
    bar.setPictureContext(true);
    QApplication::processEvents();
    check(label->text() == "Picture" && !edit->isVisible(),
          "ordinary pictures retain an unavailable editable-figure control");
}

void testTableRibbonStaysOnOneRow() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"table.selectAll", "table.insertRowAbove", "table.insertRowBelow", "table.insertColumnLeft",
             "table.insertColumnRight", "table.deleteRows", "table.deleteColumns", "table.distributeColumns", "table.distributeRows"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.show();
    bar.setTableContext(true);
    auto* page = bar.findChild<QWidget*>("ribbon.tableTab");
    for (const int width : {1280, 820, 420, 820, 1280}) {
        bar.resize(width, 180);
        QApplication::processEvents();
        check(page && page->isVisible() && page->height() <= 48,
              "table ribbon grows into multiple rows instead of one compact row");
        int center = -1;
        for (auto* button : page->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!button->isVisible()) continue;
            check(page->rect().contains(QRect(button->mapTo(page, QPoint()), button->size())),
                  "single-row table control is clipped at a supported width");
            if (center < 0) center = button->geometry().center().y();
            check(std::abs(center - button->geometry().center().y()) <= 1,
                  "table controls do not share one row");
        }
        auto* overflow = page->findChild<QToolButton*>("ribbonButton.table.more");
        check(overflow && overflow->menu() && (overflow->isVisible() == (width == 420)),
              "table overflow appears without a space constraint or is missing when needed");
        for (const auto* name : {"insert", "delete", "size", "alignment", "borders", "styles"}) {
            auto* button = page->findChild<QToolButton*>(QStringLiteral("ribbonButton.table.%1").arg(name));
            check(button && button->menu() && !button->accessibleName().isEmpty() &&
                      (button->isVisible() != overflow->menu()->actions().contains(button->menu()->menuAction())),
                  "a table dropdown is duplicated, unavailable, or missing its accessible name");
        }
        for (const auto* name : {"ribbon.tableColumnWidth", "ribbon.tableRowMinimumHeight", "ribbon.tableBorderWidth"}) {
            auto* spin = page->findChild<QDoubleSpinBox*>(name);
            check(spin && !spin->isVisible(), "detailed table sizing or line-weight controls occupy permanent ribbon rows");
        }
    }
}

void testTableAlignmentDropdown() {
    docxstudio::app::CommandRegistry commands;
    int alignmentRequests = 0;
    int selectRequests = 0;
    for (const auto* placement : {"topLeft", "topCenter", "topRight", "middleLeft", "middleCenter", "middleRight", "bottomLeft", "bottomCenter", "bottomRight"}) {
        const QString id = QStringLiteral("table.align.%1").arg(placement);
        commands.add(id, id, QKeySequence(), [&] { ++alignmentRequests; }, true);
    }
    commands.add("table.selectAll", "Select Table", QKeySequence(), [&] { ++selectRequests; });
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(1280, 300);
    bar.show();
    bar.setTableContext(true);
    QApplication::processEvents();
    auto* grid = bar.findChild<QWidget*>("ribbon.tableAlignment");
    check(grid && !grid->isVisible(), "table alignment grid occupies ribbon space before its chooser is opened");
    auto* chooser = bar.findChild<QToolButton*>("ribbonButton.table.alignment");
    check(chooser && chooser->isVisible() && chooser->menu() && !chooser->accessibleName().isEmpty(),
          "table text positioning has no accessible dropdown");
    for (const int width : {1280, 820, 420}) {
        bar.resize(width, 300);
        QApplication::processEvents();
        chooser->menu()->popup(chooser->mapToGlobal(QPoint(0, chooser->height())));
        QApplication::processEvents();
        for (const auto* placement : {"topLeft", "topCenter", "topRight", "middleLeft", "middleCenter", "middleRight", "bottomLeft", "bottomCenter", "bottomRight"}) {
            auto* button = grid->findChild<QToolButton*>(QStringLiteral("ribbonButton.table.align.%1").arg(placement));
            check(button && button->isVisible() && button->isEnabled() && !button->accessibleName().isEmpty() &&
                      chooser->menu()->rect().contains(QRect(button->mapTo(chooser->menu(), QPoint()), button->size())),
                  "a table text-position choice is missing or clipped in its dropdown");
        }
        auto* center = grid->findChild<QToolButton*>("ribbonButton.table.align.middleCenter");
        center->setFocus();
        QTest::keyClick(center, Qt::Key_Return);
        check(!chooser->menu()->isVisible() && !grid->isVisible(), "choosing table text position does not dismiss its dropdown");
    }
    chooser->menu()->popup(chooser->mapToGlobal(QPoint(0, chooser->height())));
    QApplication::processEvents();
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
    check(!chooser->menu()->isVisible() && alignmentRequests == 3,
          "Escape in table text positioning changes alignment or fails to close");
    bar.resize(1280, 180);
    QApplication::processEvents();
    auto* select = bar.findChild<QToolButton*>("ribbonButton.table.selectAll");
    check(select && select->isVisible(), "whole-table formatting has no direct Select Table route");
    select->click();
    check(alignmentRequests == 3 && selectRequests == 1, "table controls do not execute their advertised commands once");
    bar.setTableContext(false);
    check(!grid->isVisible() && !commands.action("table.align.middleCenter")->isEnabled(),
          "table positioning stays active outside its selection");
}

void testTablePopupsCloseWithContext() {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(820, 180);
    bar.show();
    for (const auto* name : {"alignment", "size", "borders", "insert", "delete", "styles"}) {
        bar.setTableContext(true);
        QApplication::processEvents();
        auto* chooser = bar.findChild<QToolButton*>(QStringLiteral("ribbonButton.table.%1").arg(name));
        check(chooser && chooser->menu(), "missing table context chooser");
        chooser->menu()->popup(chooser->mapToGlobal(QPoint(0, chooser->height())));
        QApplication::processEvents();
        check(chooser->menu()->isVisible(), "table context chooser cannot open");
        bar.setTableContext(false);
        QApplication::processEvents();
        check(!chooser->menu()->isVisible(), "table dropdown remains open after leaving its document or table context");
    }
    bar.setTableContext(true);
    auto* alignment = bar.findChild<QToolButton*>("ribbonButton.table.alignment");
    alignment->menu()->popup(alignment->mapToGlobal(QPoint(0, alignment->height())));
    QApplication::processEvents();
    bar.setPictureContext(true);
    QApplication::processEvents();
    check(!alignment->menu()->isVisible(), "table dropdown remains over a higher-priority picture context");
}

void testTableSizeOpensForKeyboardEditing() {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(820, 180);
    bar.show();
    bar.activateWindow();
    bar.setTableContext(true);
    bar.setTableDimensions(72, 0);
    QApplication::processEvents();
    auto* chooser = bar.findChild<QToolButton*>("ribbonButton.table.size");
    auto* column = bar.findChild<QDoubleSpinBox*>("ribbon.tableColumnWidth");
    int requests = 0;
    QObject::connect(&bar, &docxstudio::app::RibbonWidget::tableColumnWidthRequested,
        [&](double points) { check(points == 96, "Size keyboard entry lost its value"); ++requests; });
    QObject::connect(chooser->menu(), &QMenu::aboutToShow, [&] {
        QTimer::singleShot(0, chooser->menu(), [&] {
            auto* focus = QApplication::focusWidget();
            check(focus && (focus == column || column->isAncestorOf(focus)),
                  "keyboard-opened Size dropdown does not focus the column width field");
            QTest::keyClicks(focus, "96");
            QTest::keyClick(focus, Qt::Key_Return);
            check(requests == 1 && chooser->menu()->isVisible(), "editing Size loses its popup or commits more than once");
            QTest::keyClick(focus, Qt::Key_Escape);
        });
    });
    chooser->setFocus();
    QTest::keyClick(chooser, Qt::Key_Return);
    check(requests == 1 && !chooser->menu()->isVisible(), "Size keyboard edit cannot finish and dismiss");
}

void testTableDimensionsCommitAndSelectionSync() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* placement : {"topLeft", "topCenter", "topRight", "middleLeft", "middleCenter", "middleRight", "bottomLeft", "bottomCenter", "bottomRight"}) {
        const QString id = QStringLiteral("table.align.%1").arg(placement);
        commands.add(id, id, QKeySequence(), [] {}, true);
    }
    docxstudio::app::RibbonWidget bar(commands);
    int widthRequests = 0, heightRequests = 0;
    double width = 0, height = -1;
    QObject::connect(&bar, &docxstudio::app::RibbonWidget::tableColumnWidthRequested,
        [&](double points) { ++widthRequests; width = points; });
    QObject::connect(&bar, &docxstudio::app::RibbonWidget::tableRowMinimumHeightRequested,
        [&](double points) { ++heightRequests; height = points; });
    bar.resize(820, 400);
    bar.show();
    bar.setTableContext(true);
    bar.setTableDimensions(72.5, 0);
    bar.setTableCellAlignment(std::pair{docxstudio::core::ParagraphAlignment::center,
        docxstudio::core::TableCellVerticalAlignment::bottom});
    QApplication::processEvents();
    auto* column = bar.findChild<QDoubleSpinBox*>("ribbon.tableColumnWidth");
    auto* row = bar.findChild<QDoubleSpinBox*>("ribbon.tableRowMinimumHeight");
    auto* chooser = bar.findChild<QToolButton*>("ribbonButton.table.size");
    check(chooser && chooser->isVisible() && chooser->menu() && column && row && !column->isVisible() && !row->isVisible(),
          "table dimensions are not grouped in a compact Size dropdown");
    chooser->menu()->popup(chooser->mapToGlobal(QPoint(0, chooser->height())));
    QApplication::processEvents();
    check(column && row && column->isVisible() && row->isVisible() &&
              !column->accessibleName().isEmpty() && !row->accessibleName().isEmpty(),
          "Size dropdown has no keyboard-editable table dimensions");
    check(column->value() == 72.5 && row->value() == 0 && row->text().contains("Auto") &&
              widthRequests == 0 && heightRequests == 0,
          "selection synchronization changes table geometry or loses Auto row height");
    check(commands.action("table.align.bottomCenter")->isChecked() &&
              !commands.action("table.align.topLeft")->isChecked(),
          "the table text-position grid does not mirror the active cell");
    bar.setTableCellAlignment(std::nullopt);
    for (const auto* placement : {"topLeft", "topCenter", "topRight", "middleLeft", "middleCenter", "middleRight", "bottomLeft", "bottomCenter", "bottomRight"})
        check(!commands.action(QStringLiteral("table.align.%1").arg(placement))->isChecked(),
              "mixed table alignment incorrectly presents a single selected position");
    column->setFocus();
    column->selectAll();
    QTest::keyClicks(column, "96");
    check(widthRequests == 0, "typing an unfinished column width mutates the document");
    QTest::keyClick(column, Qt::Key_Return);
    check(widthRequests == 1 && width == 96, "committing a column width does not request the exact point size once");
    QTest::keyClick(column, Qt::Key_Return);
    check(widthRequests == 1, "recommitting an unchanged width creates another edit");
    row->setFocus();
    row->selectAll();
    QTest::keyClicks(row, "24");
    QTest::keyClick(row, Qt::Key_Return);
    check(heightRequests == 1 && height == 24, "committing a row minimum height does not request the exact point size");
    bar.setTableDimensions(144, 0);
    check(widthRequests == 1 && heightRequests == 1 && column->value() == 144 && row->value() == 0,
          "subsequent table selection synchronization creates a document edit");
    bar.setTableDimensions(-1, -1);
    check(column->text().contains("Mixed") && row->text().contains("Mixed") && widthRequests == 1 && heightRequests == 1,
          "mixed table dimensions are shown as a misleading fixed size");
    column->setFocus();
    QTest::keyClick(column, Qt::Key_Return);
    check(widthRequests == 1, "leaving an unchanged mixed column size creates a document edit");
    row->setFocus();
    row->selectAll();
    QTest::keyClicks(row, "0");
    QTest::keyClick(row, Qt::Key_Return);
    check(heightRequests == 2 && height == 0 && row->text().contains("Auto"),
          "mixed row sizes cannot be set to Auto without losing the dimension state");
}

void testTableBorderAndDistributionControls() {
    docxstudio::app::CommandRegistry commands;
    int borderRequests = 0, distributeRequests = 0;
    for (const auto* preset : {"all", "outside", "inside", "none", "top", "bottom", "left", "right"}) {
        const auto id = QStringLiteral("table.borders.%1").arg(preset);
        commands.add(id, id, QKeySequence(), [&] { ++borderRequests; });
    }
    commands.add("table.distributeColumns", "Distribute Columns", QKeySequence(), [&] { ++distributeRequests; });
    commands.add("table.distributeRows", "Distribute Rows", QKeySequence(), [&] { ++distributeRequests; });
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(820, 400);
    bar.show();
    bar.setTableContext(true);
    QApplication::processEvents();
    auto* borders = bar.findChild<QToolButton*>("ribbonButton.table.borders");
    auto* weight = bar.findChild<QDoubleSpinBox*>("ribbon.tableBorderWidth");
    auto* color = bar.findChild<QToolButton*>("ribbonButton.table.borderColor");
    check(borders && borders->isVisible() && borders->menu() && weight && !weight->isVisible() && color && !color->isVisible() && color->menu(),
          "table border details occupy permanent ribbon space instead of a dropdown");
    borders->menu()->popup(borders->mapToGlobal(QPoint(0, borders->height())));
    QApplication::processEvents();
    check(weight->isVisible() && color->isVisible(), "Borders dropdown does not expose line weight and color");
    check(bar.tableBorderWidthPoints() == 0.5 && bar.tableBorderColor() == QColor(Qt::black),
          "border choices do not begin with a half-point black line");
    weight->setValue(1.25);
    check(bar.tableBorderWidthPoints() == 1.25 && borderRequests == 0,
          "choosing line weight mutates cells before a border preset is applied");
    for (const auto* preset : {"all", "outside", "inside", "none", "top", "bottom", "left", "right"})
        check(borders->menu()->actions().contains(commands.action(QStringLiteral("table.borders.%1").arg(preset))),
              "a supported table border preset is missing from its direct chooser");
    color->menu()->popup(color->mapToGlobal(QPoint(0, color->height())));
    QApplication::processEvents();
    QToolButton* red = nullptr;
    for (auto* swatch : color->menu()->findChildren<QToolButton*>())
        if (swatch->property("paletteColor").value<QColor>() == QColor(Qt::red)) red = swatch;
    check(red && red->isVisible(), "table border color has no visible red palette choice");
    red->setFocus();
    QTest::keyClick(red, Qt::Key_Return);
    check(bar.tableBorderColor() == QColor(Qt::red) && !color->menu()->isVisible() && borderRequests == 0,
          "border color choice fails keyboard activation or applies a border prematurely");
    commands.action("table.borders.outside")->trigger();
    borders->menu()->hide();
    auto* size = bar.findChild<QToolButton*>("ribbonButton.table.size");
    check(size && size->menu(), "distribution has no Size dropdown");
    for (const auto* id : {"table.distributeColumns", "table.distributeRows"}) {
        auto* action = commands.action(QString::fromLatin1(id));
        check(size->menu()->actions().contains(action), "Size dropdown omits a distribution command");
        size->menu()->popup(size->mapToGlobal(QPoint(0, size->height())));
        QApplication::processEvents();
        size->menu()->setActiveAction(action);
        QTest::keyClick(size->menu(), Qt::Key_Return);
    }
    check(borderRequests == 1 && distributeRequests == 2, "table border/distribution controls do not invoke their commands");
    bar.setTableContext(false);
    check(!commands.action("table.borders.outside")->isEnabled() && !commands.action("table.distributeRows")->isEnabled(),
          "table border or distribution commands stay enabled without a table");
}

void testApeTekWorkspaceColors() {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget bar(commands);
    check(bar.palette().color(QPalette::Window) == QColor("#f4f5f3") &&
              bar.palette().color(QPalette::Highlight) == QColor("#245648"),
          "workspace controls do not use the ApeTek neutral and forest palette");
}

void testHeadingNavigation() {
    docxstudio::app::NavigationDock dock;
    dock.setHeadings({{QStringLiteral("Introduction"), 1},
                       {QStringLiteral("Methods"), 2}});
    dock.setMode(docxstudio::app::NavigationDock::Mode::outline);
    dock.show();
    QApplication::processEvents();
    auto* outline = dock.findChild<QListWidget*>("navigation.outline");
    check(outline && outline->isVisible() && outline->count() == 2 &&
              outline->item(1)->data(Qt::AccessibleTextRole).toString()
                  .contains("Heading level 2"),
          "outline does not present accessible document headings");
    int activated = -1;
    QObject::connect(&dock, &docxstudio::app::NavigationDock::headingActivated,
                      [&](int index) { activated = index; });
    outline->setCurrentRow(1);
    outline->setFocus();
    QTest::keyClick(outline, Qt::Key_Return);
    check(activated == 1, "Enter on a heading did not request navigation");
}

void testAccessibleCodexSettings() {
    docxstudio::app::ChatDock dock;
    dock.show();
    QApplication::processEvents();
    for (const auto* name : {"codex.model", "codex.effort", "codex.speed"}) {
        auto* choice = dock.findChild<QComboBox*>(QString::fromLatin1(name));
        check(choice && !choice->accessibleName().isEmpty(),
              "Codex model controls are not labelled for keyboard and assistive users");
    }
    auto* settings = dock.findChild<QToolButton*>("codex.settingsToggle");
    auto* model = dock.findChild<QComboBox*>("codex.model");
    check((!settings || !settings->isVisible()) && model->isVisible(),
          "Codex model settings require opening another disclosure");
    auto* input = dock.findChild<QPlainTextEdit*>("codex.input");
    check(input && !input->accessibleName().isEmpty(),
          "Codex message composer has no accessible name");
}

void testCodexChoicesUseFriendlyLabelsAndWireIds() {
    docxstudio::app::ChatDock dock;
    dock.setModels({QStringLiteral("gpt-default"), QStringLiteral("gpt-6.1-sol"),
                    QStringLiteral("gpt-5.6-luna")});
    dock.setEfforts({QStringLiteral("low"), QStringLiteral("medium"),
                    QStringLiteral("high"), QStringLiteral("xhigh"), QStringLiteral("ultra")});
    dock.setServiceTiers({QStringLiteral("default"), QStringLiteral("fast")}, "default");
    dock.setConnected(true);
    dock.resize(320, 600);
    dock.show();
    QApplication::processEvents();
    auto* model = dock.findChild<QComboBox*>("codex.model");
    auto* effort = dock.findChild<QComboBox*>("codex.effort");
    auto* speed = dock.findChild<QComboBox*>("codex.speed");
    check(model && effort && speed && model->isVisible() && effort->isVisible() && speed->isVisible(),
          "Codex choices are hidden behind model settings");
    for (auto* choice : {model, effort, speed}) {
        QStyleOptionComboBox option;
        option.initFrom(choice);
        option.currentText = choice->currentText();
        const auto edit = choice->style()->subControlRect(QStyle::CC_ComboBox, &option,
            QStyle::SC_ComboBoxEditField, choice);
        for (int index = 0; index < choice->count(); ++index) {
            if (choice == model && choice->itemData(index) != "gpt-6.1-sol") continue;
            check(edit.width() >= choice->fontMetrics().horizontalAdvance(choice->itemText(index)),
                  "a visible Codex choice clips its selected label");
        }
    }
    check(model->currentText() == QStringLiteral("GPT-6.1 Sol") &&
              dock.selectedModel() == QStringLiteral("gpt-6.1-sol"),
          "Codex does not display and select the preferred advertised Sol model");
    QStringList labels;
    for (int index = 0; index < effort->count(); ++index) labels.push_back(effort->itemText(index));
    check(labels == QStringList{"Light", "Medium", "High", "XHigh", "Ultra"} &&
              dock.selectedEffort() == QStringLiteral("low"),
          "reasoning choices are not the five labelled server capabilities with Light selected");
    QString changedModel;
    QObject::connect(&dock, &docxstudio::app::ChatDock::modelChanged,
                     [&](const QString& value) { changedModel = value; });
    model->setCurrentIndex(model->findData("gpt-default"));
    model->setCurrentIndex(model->findData("gpt-6.1-sol"));
    check(changedModel == QStringLiteral("gpt-6.1-sol"),
          "a friendly model label is sent as the backend model ID");
    effort->setCurrentIndex(effort->findData("ultra"));
    speed->setCurrentIndex(speed->findData("fast"));
    QString sentModel, sentEffort, sentTier;
    QObject::connect(&dock, &docxstudio::app::ChatDock::sendRequested,
                     [&](const QString&, const QString& wireModel, const QString& wireEffort,
                         const QString& wireTier) {
        sentModel = wireModel; sentEffort = wireEffort; sentTier = wireTier;
    });
    dock.findChild<QPlainTextEdit*>("codex.input")->setPlainText("Use the selected options");
    dock.findChild<QPushButton*>("codex.send")->click();
    check(sentModel == "gpt-6.1-sol" && sentEffort == "ultra" && sentTier == "fast",
          "visible Codex choices do not send their corresponding wire IDs");
    dock.setModels({"gpt-default", "gpt-5.6-luna"});
    check(dock.selectedModel() == "gpt-default",
          "an unavailable preferred model replaces the server-ordered fallback");
}

void testFormattingAvailabilityInOverflow() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"format.bold", "format.italic", "format.underline", "paragraph.bullets", "paragraph.numbering",
                          "paragraph.alignLeft", "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(420, 52);
    bar.show();
    QApplication::processEvents();
    bar.setParagraphStyleAvailable(false, QStringLiteral("Styles unavailable in tables"));
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    bool found = false;
    for (auto* action : more->menu()->actions()) {
        if (action->text() == QStringLiteral("Paragraph style")) {
            found = true;
            check(!action->isEnabled(),
                  "overflow offers named styles when the current selection cannot apply them");
        }
    }
    check(found, "overflow has no paragraph-style route");
}

void testWritingBarAtMinimumViewport() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"format.bold", "format.italic", "format.underline"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(420, 92);
    bar.show();
    QApplication::processEvents();
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    check(bar.width() == 420 && more && more->isVisible() &&
              bar.rect().contains(more->mapTo(&bar, more->rect().center())),
          "writing controls force a wider window or clip overflow at the minimum viewport");
}

void testWritingBarLeavesRoomForDocument() {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget bar(commands);
    check(bar.sizeHint().height() <= 60,
          "ordinary writing still reserves a duplicate task-menu row");
}

void testAlignmentNeedsNoOverflow() {
    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"format.bold", "format.italic", "format.underline",
             "paragraph.bullets", "paragraph.numbering", "paragraph.alignLeft",
             "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify"})
        commands.add(id, id, QKeySequence(), [] {});
    docxstudio::app::RibbonWidget bar(commands);
    bar.show();
    for (int width : {915, 760, 420}) {
        bar.resize(width, 52);
        QApplication::processEvents();
        for (const auto* id : {"paragraph.alignLeft", "paragraph.alignCenter",
                              "paragraph.alignRight", "paragraph.justify"}) {
            auto* button = bar.findChild<QToolButton*>(
                QStringLiteral("ribbonButton.%1").arg(id));
            check(button && button->isVisible() &&
                      bar.rect().contains(button->mapTo(&bar, button->rect().center())),
                  "paragraph alignment requires overflow or is clipped from the writing bar");
        }
    }
}

void testListControlsExpandBelowWritingRow() {
    docxstudio::app::CommandRegistry commands;
    int indentRequests = 0;
    for (const auto* id : {"format.bold", "format.italic", "format.underline",
             "paragraph.bullets", "paragraph.numbering", "paragraph.alignLeft",
             "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
             "paragraph.decreaseIndent", "paragraph.listProperties"})
        commands.add(id, id, QKeySequence(), [] {});
    commands.add("paragraph.increaseIndent", "Increase indent", QKeySequence(),
                 [&] { ++indentRequests; });
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(915, 160);
    bar.show();
    bar.setListContext(true, 3);
    QApplication::processEvents();
    auto* controls = bar.findChild<QWidget*>("ribbon.listTab");
    check(controls && controls->isVisible(),
          "list context requires a separate List dropdown instead of direct controls");
    for (int width : {915, 420}) {
        bar.resize(width, 160);
        QApplication::processEvents();
        for (const auto* name : {"ribbonButton.paragraph.increaseIndent",
                                "ribbonButton.paragraph.decreaseIndent",
                                "ribbonButton.paragraph.listProperties"}) {
            auto* button = controls->findChild<QToolButton*>(QString::fromLatin1(name));
            check(button && button->isVisible() &&
                      bar.rect().contains(QRect(button->mapTo(&bar, QPoint()), button->size())),
                  "expanded list controls are clipped");
        }
        if (qEnvironmentVariableIsSet("OWL_DOCS_LIST_RENDER"))
            bar.grab().save(QStringLiteral("/tmp/owl-docs-inline-list-%1.png").arg(width));
    }
    controls->findChild<QToolButton*>("ribbonButton.paragraph.increaseIndent")->click();
    check(indentRequests == 1, "inline list indent control does not invoke its command");
    bar.setListContext(false, 1);
    check(!controls->isVisible(), "list controls persist outside their context");
}

void testPopupLabelHasSeparateChevron() {
    docxstudio::app::CommandRegistry commands;
    docxstudio::app::RibbonWidget bar(commands);
    bar.resize(420, 52);
    bar.show();
    QApplication::processEvents();
    auto* more = bar.findChild<QToolButton*>("workspace.moreFormatting");
    check(more && more->width() >= more->fontMetrics().horizontalAdvance(more->text()) + 28,
          "More menu indicator overlaps its label");
    const QImage image = more->grab().toImage();
    const QString path = qEnvironmentVariable("OWL_DOCS_POPUP_RENDER_PATH");
    if (!path.isEmpty()) check(image.save(path), "could not capture popup indicator");
    const QColor ink = more->palette().color(QPalette::ButtonText);
    int chevronPixels = 0;
    const qreal ratio = image.devicePixelRatio();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (x / ratio < more->width() - 17 || x / ratio > more->width() - 5 ||
                std::abs(y / ratio - more->height() / 2.0) > 5) continue;
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - ink.red()) < 40 &&
                std::abs(pixel.green() - ink.green()) < 40 &&
                std::abs(pixel.blue() - ink.blue()) < 40) ++chevronPixels;
        }
    }
    check(chevronPixels >= 6, "More has no visible separate dropdown chevron");
    bool opened = false;
    QObject::connect(more->menu(), &QMenu::aboutToShow, [&] { opened = true; });
    QTimer::singleShot(0, more->menu(), &QMenu::hide);
    more->showMenu();
    check(opened, "More does not open its formatting menu");
}
}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-size-keyboard")) {
        testTableSizeOpensForKeyboardEditing();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-popups")) {
        testTablePopupsCloseWithContext();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-row")) {
        testTableRibbonStaysOnOneRow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-borders")) {
        testTableBorderAndDistributionControls();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-dimensions")) {
        testTableDimensionsCommitAndSelectionSync();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("table-alignment")) {
        testTableAlignmentDropdown();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("context-switch")) {
        testContextChangesPreserveIndentAccess();
        testEditableFigureContext();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("figure")) {
        testEditableFigureContext();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("keyboard")) {
        testRibbonKeyboardActivation();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("overflow")) {
        testMoreContainsOnlyOverflow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("equation-cell")) {
        testEquationAndCellPictureContexts();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("picture-direct")) {
        testPictureWrapChoicesAreDirect();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("contexts")) {
        testContextsExpandBelowPrimary();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("alignment")) {
        testAlignmentNeedsNoOverflow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("inline-list")) {
        testListControlsExpandBelowWritingRow();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("popup")) {
        testPopupLabelHasSeparateChevron();
        return 0;
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("chat-choices")) {
        testAccessibleCodexSettings();
        testCodexChoicesUseFriendlyLabelsAndWireIds();
        return 0;
    }
    testTableRibbonStaysOnOneRow();
    testTableAlignmentDropdown();
    testTablePopupsCloseWithContext();
    testTableSizeOpensForKeyboardEditing();
    testTableDimensionsCommitAndSelectionSync();
    testTableBorderAndDistributionControls();
    testContextsExpandBelowPrimary();
    testPictureWrapChoicesAreDirect();
    testEquationAndCellPictureContexts();
    testRibbonKeyboardActivation();
    testMoreContainsOnlyOverflow();
    testContextChangesPreserveIndentAccess();
    testEditableFigureContext();
    testCompactWritingBar();
    testApeTekWorkspaceColors();
    testHeadingNavigation();
    testAccessibleCodexSettings();
    testCodexChoicesUseFriendlyLabelsAndWireIds();
    testFormattingAvailabilityInOverflow();
    testWritingBarAtMinimumViewport();
    testWritingBarLeavesRoomForDocument();
    testAlignmentNeedsNoOverflow();
    testListControlsExpandBelowWritingRow();
    testPopupLabelHasSeparateChevron();
    return 0;
}
