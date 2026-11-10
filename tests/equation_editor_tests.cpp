#include "docxstudio/app/EquationEditorDialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    docxstudio::app::EquationEditorDialog dialog(QStringLiteral("\\frac{a}"), true);
    dialog.show();
    QApplication::processEvents();
    auto* source = dialog.findChild<QPlainTextEdit*>(QStringLiteral("insertEquation.latex"));
    auto* error = dialog.findChild<QLabel*>(QStringLiteral("insertEquation.error"));
    auto* display = dialog.findChild<QCheckBox*>(QStringLiteral("insertEquation.display"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>(QStringLiteral("insertEquation.buttons"));
    check(source && error && display && buttons, "equation editor lacks named accessible controls");
    check(source->toPlainText() == QStringLiteral("\\frac{a}") && display->isChecked(), "initial source or mode was lost");
    check(!error->text().isEmpty() && !buttons->button(QDialogButtonBox::Ok)->isEnabled(), "invalid source is not diagnosed in place");
    dialog.accept();
    check(dialog.isVisible() && source->toPlainText() == QStringLiteral("\\frac{a}"), "invalid accept closed editor or lost input");
    source->setPlainText(QStringLiteral("\\frac{a}{b}\n+ x^2"));
    check(error->text().isEmpty() && buttons->button(QDialogButtonBox::Ok)->isEnabled(), "correcting source did not clear error");
    check(dialog.canonicalLatex() == QStringLiteral("\\frac{a}{b} + x^{2}") && dialog.displayEquation(), "corrected equation did not canonicalize");
    dialog.accept();
    check(dialog.result() == QDialog::Accepted, "valid equation could not be accepted");
    docxstudio::app::EquationEditorDialog oversized(QString(9000, QLatin1Char('a')));
    check(!oversized.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled(),
          "dialog accepts a source larger than the document equation limit");
    std::cout << "equation editor tests passed\n";
}
