#include "docxstudio/app/TableInsertDialog.h"
#include <QApplication>
#include <QTableWidget>
#include <QSpinBox>
#include <QTest>
#include <cstdlib>
#include <iostream>
namespace { void check(bool value, const char* message) { if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); } } }
int main(int argc, char** argv) {
    QApplication application(argc,argv);
    docxstudio::app::TableInsertDialog picker;
    picker.show();
    QApplication::processEvents();
    auto* grid = picker.findChild<QTableWidget*>(QStringLiteral("insertTable.grid"));
    check(grid && grid->rowCount()==8 && grid->columnCount()==10, "table picker has no Word-style10x8 grid");
    const auto cell = grid->visualItemRect(grid->item(2,3));
    QTest::mouseMove(grid->viewport(),cell.center());
    QApplication::processEvents();
    check(picker.rowCount()==3 && picker.columnCount()==4 && picker.result()!=QDialog::Accepted,
          "grid hover does not preview the chosen dimensions without inserting");
    check(grid->selectedItems().size()==12, "grid does not highlight the full selected rectangle");
    QTest::mouseClick(grid->viewport(),Qt::LeftButton,Qt::NoModifier,cell.center());
    check(picker.result()==QDialog::Accepted && picker.rowCount()==3 && picker.columnCount()==4,
          "one grid click does not accept the highlighted table");
    docxstudio::app::TableInsertDialog cancel;
    cancel.show();
    QTest::keyClick(&cancel,Qt::Key_Escape);
    check(cancel.result()==QDialog::Rejected && !cancel.isVisible(), "Escape does not cancel table insertion");
    docxstudio::app::TableInsertDialog keyboard;
    keyboard.show();
    auto* keyboardGrid=keyboard.findChild<QTableWidget*>(QStringLiteral("insertTable.grid"));
    keyboardGrid->setFocus();
    for (int key=0;key<25;++key) QTest::keyClick(keyboardGrid,Qt::Key_Right);
    check(keyboard.columnCount()==10,"arrow-key grid selection extends outside the visible grid");
    for (int key=0;key<25;++key) QTest::keyClick(keyboardGrid,Qt::Key_Down);
    check(keyboard.rowCount()==8,"arrow-key grid selection extends below the visible grid");
    keyboard.findChild<QSpinBox*>(QStringLiteral("insertTable.rows"))->setValue(40);
    keyboard.findChild<QSpinBox*>(QStringLiteral("insertTable.columns"))->setValue(14);
    check(keyboard.rowCount()==40 && keyboard.columnCount()==14,"manual sizes cannot exceed the quick grid");
    QTest::keyClick(keyboardGrid,Qt::Key_Return);
    check(keyboard.result()==QDialog::Accepted && keyboard.rowCount()==40 && keyboard.columnCount()==14,
          "keyboard Insert loses larger manual dimensions");
    std::cout << "Table insertion picker passed\n";
}
