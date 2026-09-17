#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/FontFamilyPicker.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RibbonWidget.h"

#include <QAction>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QItemDelegate>
#include <QMenu>
#include <QModelIndex>
#include <QPainter>
#include <QProxyStyle>
#include <QSet>
#include <QStyleOptionViewItem>
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

int tabIndex(const QTabWidget& tabs, const QString& label) {
    for (int index = 0; index < tabs.count(); ++index) {
        if (tabs.tabText(index) == label) return index;
    }
    return -1;
}

class FontRowCaptureStyle final : public QProxyStyle {
public:
    void drawControl(ControlElement element, const QStyleOption* option,
                     QPainter* painter,
                     const QWidget* widget = nullptr) const override {
        if (element == QStyle::CE_ItemViewItem) {
            if (const auto* row =
                    qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
                renderedText = row->text;
                renderedFamily = row->font.family();
                renderedWithIcon = !row->icon.isNull();
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }

    mutable QString renderedText;
    mutable QString renderedFamily;
    mutable bool renderedWithIcon{false};
};

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Ribbon Widget Tests"));
    docxstudio::app::CommandRegistry commands;
    const QStringList tableCommandIds{
        QStringLiteral("table.insertRowAbove"),
        QStringLiteral("table.insertRowBelow"),
        QStringLiteral("table.insertColumnLeft"),
        QStringLiteral("table.insertColumnRight"),
        QStringLiteral("table.deleteRows"),
        QStringLiteral("table.deleteColumns"),
    };
    for (const auto& id : tableCommandIds) {
        commands.add(id, id, QKeySequence(), [] {});
    }
    const QStringList pictureCommandIds{
        QStringLiteral("picture.size"),
        QStringLiteral("picture.altText"),
        QStringLiteral("picture.layoutOptions"),
        QStringLiteral("picture.wrapInline"),
        QStringLiteral("picture.wrapSquare"),
        QStringLiteral("picture.wrapTopBottom"),
        QStringLiteral("picture.delete"),
    };
    for (const auto& id : pictureCommandIds) {
        commands.add(id, id, QKeySequence(), [] {},
                     id.startsWith(QStringLiteral("picture.wrap")));
    }

    docxstudio::app::RibbonWidget ribbon(commands);
    auto* stylePicker = ribbon.findChild<QComboBox*>(
        QStringLiteral("ribbon.paragraphStyle"));
    check(stylePicker && stylePicker->count() == 14 &&
              stylePicker->currentData().toString() == QStringLiteral("Normal"),
          "paragraph-style picker does not expose the complete built-in catalog");
    const int headingOne = stylePicker
        ? stylePicker->findData(QStringLiteral("Heading1"))
        : -1;
    check(headingOne >= 0 &&
              stylePicker->itemText(headingOne) == QStringLiteral("Heading 1") &&
              stylePicker->itemData(headingOne, Qt::FontRole).value<QFont>().bold(),
          "Heading 1 is missing its stable ID or visual preview");
    check(!stylePicker->itemData(0, Qt::ForegroundRole).isValid() &&
              !stylePicker->itemData(headingOne, Qt::ForegroundRole).isValid(),
          "paragraph-style rows force document colors instead of the active theme palette");
    QString requestedStyle;
    QObject::connect(&ribbon,
                     &docxstudio::app::RibbonWidget::paragraphStyleRequested,
                     [&requestedStyle](const QString& id) {
        requestedStyle = id;
    });
    check(QMetaObject::invokeMethod(
              stylePicker, "activated", Qt::DirectConnection,
              Q_ARG(int, headingOne)) &&
              requestedStyle == QStringLiteral("Heading1"),
          "activating a style row did not request its stable style ID");
    ribbon.setParagraphStyle(QStringLiteral("FirmLegalHeading"));
    check(stylePicker->count() == 15 &&
              stylePicker->currentData().toString() ==
                  QStringLiteral("FirmLegalHeading") &&
              stylePicker->currentText().startsWith(QStringLiteral("Custom:")),
          "an imported custom style was not represented without relabeling it");
    int styleModelMutations = 0;
    QObject::connect(stylePicker->model(), &QAbstractItemModel::rowsInserted,
                     [&styleModelMutations] { ++styleModelMutations; });
    QObject::connect(stylePicker->model(), &QAbstractItemModel::rowsRemoved,
                     [&styleModelMutations] { ++styleModelMutations; });
    ribbon.setParagraphStyle(QStringLiteral("FirmLegalHeading"));
    check(styleModelMutations == 0 && stylePicker->count() == 15,
          "an unchanged custom style rebuilt the picker model");
    requestedStyle.clear();
    check(QMetaObject::invokeMethod(
              stylePicker, "activated", Qt::DirectConnection,
              Q_ARG(int, stylePicker->currentIndex())) &&
              requestedStyle.isEmpty(),
          "the read-only custom-style context was incorrectly made actionable");
    ribbon.setParagraphStyle(QStringLiteral("Normal"));
    check(stylePicker->count() == 14 &&
              stylePicker->currentData().toString() == QStringLiteral("Normal"),
          "returning to a built-in style retained a stale custom row");
    ribbon.setParagraphStyle({});
    check(stylePicker->count() == 15 &&
              stylePicker->isEnabled() &&
              stylePicker->currentText() == QStringLiteral("Multiple styles"),
          "mixed paragraph styles are not communicated by the ribbon");
    ribbon.setParagraphStyleAvailable(
        false, QStringLiteral("Paragraph styles unavailable in tables"));
    check(!stylePicker->isEnabled() && stylePicker->count() == 15 &&
              stylePicker->currentText() ==
                  QStringLiteral("Paragraph styles unavailable in tables"),
          "an unavailable table context is still presented as mixed or actionable");
    styleModelMutations = 0;
    ribbon.setParagraphStyleAvailable(
        false, QStringLiteral("Paragraph styles unavailable in tables"));
    check(styleModelMutations == 0,
          "an unchanged unavailable style context rebuilt the picker model");
    ribbon.setParagraphStyleAvailable(true);
    check(stylePicker->isEnabled() && stylePicker->count() == 15 &&
              stylePicker->currentText() == QStringLiteral("Multiple styles"),
          "leaving an unavailable context did not restore the mixed style state");

    auto* fontPicker = ribbon.findChild<QFontComboBox*>(
        QStringLiteral("ribbon.fontFamily"));
    check(fontPicker != nullptr &&
              dynamic_cast<docxstudio::app::FontFamilyPicker*>(fontPicker),
          "ribbon does not use the document-editor font picker");
    check(fontPicker->itemDelegate() &&
              fontPicker->itemDelegate()->objectName() ==
                  QStringLiteral("fontFamilyNameOnlyDelegate"),
          "font picker still uses Qt's writing-system sample delegate");

    // Replacing Qt's noisy delegate must not filter multilingual fonts out of
    // the model: editing imported Arabic/CJK/Indic text still needs every
    // installed family to remain selectable.
    QFontComboBox stockFontPicker;
    QSet<QString> stockFamilies;
    QSet<QString> owlFamilies;
    for (int index = 0; index < stockFontPicker.count(); ++index) {
        stockFamilies.insert(stockFontPicker.itemText(index));
    }
    for (int index = 0; index < fontPicker->count(); ++index) {
        owlFamilies.insert(fontPicker->itemText(index));
    }
    check(owlFamilies == stockFamilies,
          "readable font picker filtered or added installed font families");

    // The clean label should still act as a live font preview: each family
    // name is painted in that family, with no sample suffix or type icon.
    check(fontPicker->count() > 0,
          "font picker unexpectedly contains no installed families");
    int resolvedPreviewIndex = -1;
    for (int index = 0; index < fontPicker->count(); ++index) {
        if (QFontDatabase::writingSystems(fontPicker->itemText(index))
                .contains(QFontDatabase::Latin)) {
            resolvedPreviewIndex = index;
            break;
        }
    }
    check(resolvedPreviewIndex >= 0,
          "font picker contains no family capable of a Latin preview");
    const QString previewFamily = fontPicker->itemText(resolvedPreviewIndex);
    auto* captureStyle = new FontRowCaptureStyle;
    captureStyle->setParent(fontPicker->view());
    fontPicker->view()->setStyle(captureStyle);
    QImage previewImage(500, 60, QImage::Format_ARGB32_Premultiplied);
    previewImage.fill(Qt::transparent);
    QPainter previewPainter(&previewImage);
    QStyleOptionViewItem previewOption;
    previewOption.rect = previewImage.rect();
    previewOption.widget = fontPicker->view();
    previewOption.font = fontPicker->font();
    fontPicker->itemDelegate()->paint(
        &previewPainter, previewOption,
        fontPicker->model()->index(resolvedPreviewIndex, 0));
    previewPainter.end();
    check(captureStyle->renderedText == previewFamily,
          "font picker rendered text beyond the family name");
    check(captureStyle->renderedFamily == previewFamily,
          "font picker family name was not rendered in its own typeface");
    check(!captureStyle->renderedWithIcon,
          "font picker restored Qt's distracting font-type icon");

    // A script-specific font can map ASCII to unrelated glyphs. It remains in
    // the complete font list, but its canonical name must stay legible in the
    // UI font instead of displaying as apparent non-English menu text.
    int scriptOnlyIndex = -1;
    for (int index = 0; index < fontPicker->count(); ++index) {
        const auto systems =
            QFontDatabase::writingSystems(fontPicker->itemText(index));
        if (!systems.isEmpty() &&
            !systems.contains(QFontDatabase::Latin) &&
            !systems.contains(QFontDatabase::Symbol)) {
            scriptOnlyIndex = index;
            break;
        }
    }
    if (scriptOnlyIndex >= 0) {
        captureStyle->renderedText.clear();
        captureStyle->renderedFamily.clear();
        QPainter scriptOnlyPainter(&previewImage);
        fontPicker->itemDelegate()->paint(
            &scriptOnlyPainter, previewOption,
            fontPicker->model()->index(scriptOnlyIndex, 0));
        scriptOnlyPainter.end();
        const QString scriptOnlyFamily = fontPicker->itemText(scriptOnlyIndex);
        check(captureStyle->renderedText == scriptOnlyFamily,
              "script-specific font label gained a native-script sample");
        check(captureStyle->renderedFamily == fontPicker->font().family(),
              "script-specific family name was not kept in the readable UI font");
    }

    // On a normal Ubuntu desktop, pick a non-Latin family and verify its row is
    // sized for the family name alone. QFontComboBox's stock delegate makes
    // this much wider by appending a native-script sample.
    for (int index = 0; index < fontPicker->count(); ++index) {
        const QString family = fontPicker->itemText(index);
        const auto systems = QFontDatabase::writingSystems(family);
        bool hasNonLatinText = false;
        for (const auto system : systems) {
            if (system != QFontDatabase::Any &&
                system != QFontDatabase::Latin &&
                system != QFontDatabase::Symbol &&
                !QFontDatabase::writingSystemSample(system).isEmpty()) {
                hasNonLatinText = true;
                break;
            }
        }
        if (!hasNonLatinText) continue;
        QStyleOptionViewItem option;
        option.font = fontPicker->font();
        const QModelIndex modelIndex = fontPicker->model()->index(index, 0);
        QFont previewFont = option.font;
        if (systems.contains(QFontDatabase::Latin)) {
            previewFont.setFamily(family);
        }
        const int familyNameWidth =
            QFontMetrics(previewFont).horizontalAdvance(family);
        check(fontPicker->itemDelegate()->sizeHint(option, modelIndex).width() <=
                  familyNameWidth + 24,
              "font picker row still reserves room for a non-English sample");
        break;
    }
    const QString fontPickerRenderPath =
        qEnvironmentVariable("OWL_DOCS_FONT_PICKER_RENDER_PATH");
    if (!fontPickerRenderPath.isEmpty()) {
        ribbon.resize(1280, 180);
        ribbon.show();
        fontPicker->showPopup();
        QApplication::processEvents();
        check(fontPicker->view()->viewport()->grab().save(fontPickerRenderPath),
              "could not save the font-picker visual regression image");
        fontPicker->hidePopup();
    }
    auto* tabs = ribbon.findChild<QTabWidget*>(QStringLiteral("ribbon.tabs"));
    check(tabs != nullptr, "ribbon has no tab widget");
    const int homeIndex = tabIndex(*tabs, QStringLiteral("Home"));
    const int tableIndex = tabIndex(*tabs, QStringLiteral("Table"));
    const int pictureIndex = tabIndex(*tabs, QStringLiteral("Picture"));
    check(homeIndex >= 0 && tableIndex >= 0 && pictureIndex >= 0,
          "ribbon is missing a primary or contextual tab");
    check(!tabs->isTabVisible(tableIndex),
          "Table tab is visible without a selected table");
    check(!tabs->isTabVisible(pictureIndex),
          "Picture tab is visible without a selected picture");
    for (const auto& id : tableCommandIds) {
        auto* action = commands.action(id);
        auto* button = ribbon.findChild<QToolButton*>(
            QStringLiteral("ribbonButton.%1").arg(id));
        check(action && button && button->defaultAction() == action,
              "Table command has no matching ribbon action/button");
        check(!action->isEnabled(),
              "Table command is enabled outside a table context");
        check(!button->icon().isNull() && !button->accessibleName().isEmpty() &&
                  !button->toolTip().isEmpty(),
              "Table command is not an accessible icon action");
    }
    for (const auto& id : pictureCommandIds) {
        check(!commands.action(id)->isEnabled(),
              "Picture command is enabled outside a picture context");
    }

    ribbon.setPictureContext(true,
                             docxstudio::core::ImagePlacement::square);
    check(tabs->isTabVisible(pictureIndex),
          "Picture tab did not appear for a selected picture");
    for (const auto& id : pictureCommandIds) {
        check(commands.action(id)->isEnabled(),
              "Picture command stayed disabled inside a picture context");
    }
    check(commands.action(QStringLiteral("picture.wrapSquare"))->isChecked() &&
              !commands.action(QStringLiteral("picture.wrapInline"))->isChecked() &&
              !commands.action(QStringLiteral("picture.wrapTopBottom"))->isChecked(),
          "Picture ribbon did not mirror the selected wrap mode");
    auto* pictureWrap = ribbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.picture.wrap"));
    check(pictureWrap && pictureWrap->menu() &&
              pictureWrap->menu()->actions().size() == 3 &&
              !pictureWrap->icon().isNull() &&
              !pictureWrap->accessibleName().isEmpty(),
          "Picture ribbon has no accessible visual wrap menu");
    tabs->setCurrentIndex(pictureIndex);
    ribbon.setPictureContext(false);
    check(!tabs->isTabVisible(pictureIndex) &&
              tabs->currentIndex() == homeIndex,
          "closing picture context did not return the ribbon to Home");

    ribbon.setTableContext(true);
    check(tabs->isTabVisible(tableIndex),
          "Table tab did not appear for a selected table");
    for (const auto& id : tableCommandIds) {
        check(commands.action(id)->isEnabled(),
              "Table command stayed disabled inside a table context");
    }

    auto* styles = ribbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.table.styles"));
    check(styles && styles->menu() && !styles->icon().isNull(),
          "Table ribbon has no visual style gallery");
    check(styles->accessibleName() == QStringLiteral("Table Styles") &&
              !styles->accessibleDescription().isEmpty(),
          "Table style gallery is not accessible");
    const auto styleActions = styles->menu()->actions();
    check(styleActions.size() >= 12,
          "Table style gallery does not offer a useful range of templates");
    QSet<QString> styleKeys;
    for (const auto* action : styleActions) {
        const QString key = action->data().toString();
        check(!key.isEmpty() && !action->text().isEmpty() &&
                  !action->icon().isNull(),
              "Table style template lacks a key, label, or preview");
        styleKeys.insert(key);
    }
    check(styleKeys.size() == styleActions.size() &&
              styleKeys.contains(QStringLiteral("plain")) &&
              styleKeys.contains(QStringLiteral("grid")) &&
              styleKeys.contains(QStringLiteral("aubergine")) &&
              styleKeys.contains(QStringLiteral("orange-accent")),
          "Table style template keys are missing or duplicated");
    QString selectedStyle;
    QObject::connect(&ribbon, &docxstudio::app::RibbonWidget::tableStyleRequested,
                     [&selectedStyle](const QString& key) {
                         selectedStyle = key;
                     });
    for (auto* action : styleActions) {
        if (action->data().toString() == QStringLiteral("aubergine")) {
            action->trigger();
            break;
        }
    }
    check(selectedStyle == QStringLiteral("aubergine"),
          "Table style choice did not emit its stable style key");

    auto* highlight = ribbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.format.highlightColor"));
    check(highlight && !highlight->icon().isNull(),
          "ribbon has no text highlighter icon");
    const QImage marker = highlight->icon().pixmap(24, 24).toImage()
                              .convertToFormat(QImage::Format_ARGB32);
    int upperYellowPixels = 0;
    int darkPixels = 0;
    for (int y = 0; y < marker.height(); ++y) {
        for (int x = 0; x < marker.width(); ++x) {
            const QColor pixel = marker.pixelColor(x, y);
            if (pixel.alpha() < 96) continue;
            if (y < 16 && pixel.red() > 180 && pixel.green() > 140 &&
                pixel.blue() < 120) {
                ++upperYellowPixels;
            }
            if (pixel.red() < 120 && pixel.green() < 120 && pixel.blue() < 120) {
                ++darkPixels;
            }
        }
    }
    check(upperYellowPixels > 1 && darkPixels > 8,
          "highlight action is not rendered as a colored marker silhouette");

    tabs->setCurrentIndex(tableIndex);
    ribbon.setTableContext(false);
    check(!tabs->isTabVisible(tableIndex) && tabs->currentIndex() == homeIndex,
          "closing table context did not return the ribbon to Home");
    for (const auto& id : tableCommandIds) {
        check(!commands.action(id)->isEnabled(),
              "hidden Table command remained enabled");
    }

    docxstudio::app::MainWindow window;
    auto* windowRibbonTabs = window.findChild<QTabWidget*>(
        QStringLiteral("ribbon.tabs"));
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    check(windowRibbonTabs && canvas,
          "main window has no ribbon or active document canvas");
    const int windowTableIndex = tabIndex(*windowRibbonTabs,
                                          QStringLiteral("Table"));
    check(windowTableIndex >= 0 &&
              !windowRibbonTabs->isTabVisible(windowTableIndex),
          "main-window Table tab starts in the wrong state");
    check(canvas->insertTable(2, 2, true),
          "could not create a table for contextual-ribbon integration test");
    QApplication::processEvents();
    check(windowRibbonTabs->isTabVisible(windowTableIndex),
          "selecting a table cell did not reveal the main-window Table tab");
    canvas->selectAll();
    canvas->selectAll();
    QApplication::processEvents();
    check(!windowRibbonTabs->isTabVisible(windowTableIndex),
          "leaving a table did not hide the main-window Table tab");

    return 0;
}
