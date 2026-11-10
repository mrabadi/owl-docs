#include "docxstudio/app/ColorPaletteWidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void swatchGridSelectsExactColor() {
    using docxstudio::app::ColorPaletteWidget;
    ColorPaletteWidget palette(ColorPaletteWidget::Usage::Text);
    palette.show();
    QApplication::processEvents();
    const auto buttons=palette.findChildren<QAbstractButton*>();
    QList<QAbstractButton*> swatches;
    QSet<QRgb> unique;
    QSet<int> columns;
    QSet<int> rows;
    for (auto* button : buttons) {
        const QColor color=button->property("paletteColor").value<QColor>();
        if (!color.isValid()) continue;
        swatches.push_back(button);
        unique.insert(color.rgba());
        columns.insert(button->mapTo(&palette,QPoint{}).x());
        rows.insert(button->mapTo(&palette,QPoint{}).y());
        check(button->text().isEmpty(),"color choices still display a list of color names");
        check(button->toolTip().contains(color.name(),Qt::CaseInsensitive) &&
                  !button->accessibleName().isEmpty(),"a swatch lacks its exact-color tooltip or accessible name");
        const QRect bounds(button->mapTo(&palette,QPoint{}),button->size());
        check(button->isVisible() && palette.rect().contains(bounds),
              "a palette swatch is clipped");
    }
    check(unique.size()>=80 && columns.size()>=10 && rows.size()>=6,
          "color picker does not provide a substantial two-dimensional theme/shade grid");
    QSignalSpy selected(&palette,&ColorPaletteWidget::colorSelected);
    auto* choice=swatches[swatches.size()/2];
    const QColor expected=choice->property("paletteColor").value<QColor>();
    QTest::mouseClick(choice,Qt::LeftButton);
    check(selected.size()==1 && selected[0][0].value<QColor>()==expected,
          "choosing a swatch does not select its exact document color once");
}

void keyboardGridNavigation() {
    using docxstudio::app::ColorPaletteWidget;
    ColorPaletteWidget palette(ColorPaletteWidget::Usage::Text);
    palette.show();
    palette.activateWindow();
    QApplication::processEvents();
    auto* first=palette.findChild<QAbstractButton*>(QStringLiteral("palette.swatch.0"));
    check(first,"first theme swatch is missing");
    first->setFocus();
    QApplication::processEvents();
    const QPoint origin=first->mapTo(&palette,QPoint{});
    QTest::keyClick(first,Qt::Key_Right);
    auto* right=qobject_cast<QAbstractButton*>(QApplication::focusWidget());
    check(right && right!=first && right->mapTo(&palette,QPoint{}).x()>origin.x() &&
          right->mapTo(&palette,QPoint{}).y()==origin.y(),
          "Right arrow does not move across theme columns");
    QTest::keyClick(right,Qt::Key_Down);
    auto* down=qobject_cast<QAbstractButton*>(QApplication::focusWidget());
    check(down && down!=right && down->mapTo(&palette,QPoint{}).x()==right->mapTo(&palette,QPoint{}).x() &&
          down->mapTo(&palette,QPoint{}).y()>origin.y(),
          "Down arrow does not move to the next shade in the column");
    QSignalSpy selected(&palette,&ColorPaletteWidget::colorSelected);
    QTest::keyClick(down,Qt::Key_Return);
    check(selected.size()==1 && selected[0][0].value<QColor>()==down->property("paletteColor").value<QColor>(),
          "Enter does not select the keyboard-focused color exactly once");
}

void currentDefaultCustomAndRecentColors() {
    using docxstudio::app::ColorPaletteWidget;
    QSettings().clear();
    ColorPaletteWidget text(ColorPaletteWidget::Usage::Text);
    text.show();
    QApplication::processEvents();
    QSignalSpy selected(&text,&ColorPaletteWidget::colorSelected);
    QSignalSpy automatic(&text,&ColorPaletteWidget::defaultRequested);
    QSignalSpy custom(&text,&ColorPaletteWidget::customColorRequested);
    auto* reset=text.findChild<QAbstractButton*>(QStringLiteral("palette.default"));
    auto* more=text.findChild<QAbstractButton*>(QStringLiteral("palette.custom"));
    check(reset && reset->text()==QStringLiteral("Automatic") && more,
          "text palette lacks Automatic or custom color choice");
    QTest::mouseClick(reset,Qt::LeftButton);
    QTest::mouseClick(more,Qt::LeftButton);
    check(automatic.size()==1 && custom.size()==1 && selected.isEmpty(),
          "default/custom choice emits an ordinary color or wrong action");
    reset->setFocus();QTest::keyClick(reset,Qt::Key_Return);
    more->setFocus();QTest::keyClick(more,Qt::Key_Return);
    check(automatic.size()==2 && custom.size()==2,
          "Enter does not activate default/custom palette controls");
    const QColor forest(QStringLiteral("#173b32"));
    text.setCurrentColor(forest);
    check(selected.isEmpty(),"updating caret color emits a document mutation");
    bool checked=false;
    for (auto* button:text.findChildren<QAbstractButton*>())
        if (button->property("paletteColor").value<QColor>()==forest) checked|=button->isChecked();
    check(checked,"current document color is not marked in the grid");
    check(text.findChildren<QWidget*>(QStringLiteral("palette.recents")).front()->isHidden(),
          "merely moving the caret filled recent colors");
    for (int index=0;index<12;++index) text.rememberColor(QColor(10+index,40,80));
    text.rememberColor(QColor(14,40,80));
    text.rememberColor({});
    ColorPaletteWidget reopened(ColorPaletteWidget::Usage::Text);
    auto* recent=reopened.findChild<QWidget*>(QStringLiteral("palette.recents"));
    const auto buttons=recent ? recent->findChildren<QAbstractButton*>() : QList<QAbstractButton*>{};
    check(buttons.size()==10 && buttons[0]->property("paletteColor").value<QColor>()==QColor(14,40,80),
          "recent choices are not persistent, deduplicated, or bounded to ten");
    QSet<QRgb> values;
    for (auto* button:buttons) values.insert(button->property("paletteColor").value<QColor>().rgba());
    check(values.size()==10,"recent choices contain duplicates");
    ColorPaletteWidget highlight(ColorPaletteWidget::Usage::Highlight);
    auto* clear=highlight.findChild<QAbstractButton*>(QStringLiteral("palette.default"));
    check(clear && clear->text()==QStringLiteral("No Color") &&
          highlight.findChild<QWidget*>(QStringLiteral("palette.recents"))->isHidden(),
          "highlight lacks No Color or shares text-color history");
    QSignalSpy cleared(&highlight,&ColorPaletteWidget::defaultRequested);
    clear->click();
    check(cleared.size()==1,"No Color does not request clearing the highlight");
}
}

int main(int argc,char** argv) {
    QTemporaryDir state;
    check(state.isValid(),"create isolated color-palette settings");
    qputenv("XDG_CONFIG_HOME",state.path().toUtf8());
    QApplication application(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ColorPalette"));
    swatchGridSelectsExactColor();
    keyboardGridNavigation();
    currentDefaultCustomAndRecentColors();
    const auto capture=qEnvironmentVariable("OWL_DOCS_CAPTURE_COLOR_PALETTE");
    if (!capture.isEmpty()) {
        for (const bool highlight:{false,true}) {
            docxstudio::app::ColorPaletteWidget palette(highlight
                ? docxstudio::app::ColorPaletteWidget::Usage::Highlight : docxstudio::app::ColorPaletteWidget::Usage::Text);
            palette.setCurrentColor(QColor(QStringLiteral("#173b32")));palette.show();
            QApplication::processEvents();
            check(palette.grab().save(capture+(highlight ? QStringLiteral("-highlight.png") : QStringLiteral("-text.png"))),"capture palette grid");
        }
    }
    std::cout << "Color palette tests passed\n";
}
