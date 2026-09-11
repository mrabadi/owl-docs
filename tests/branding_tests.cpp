#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/OwlDocsIcon.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFontComboBox>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QXmlStreamReader>

#include <cstdlib>
#include <array>
#include <cmath>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString readFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "could not open branding asset");
    return QString::fromUtf8(file.readAll());
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "could not open binary branding asset");
    return file.readAll();
}

void checkXml(const QString& source, const char* message) {
    QXmlStreamReader reader(source);
    while (!reader.atEnd()) reader.readNext();
    check(!reader.hasError(), message);
}

QStringList paragraphTexts(const docxstudio::app::DocumentCanvas& canvas) {
    QStringList result;
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.push_back(QString::fromUtf16(
            paragraph.text().data(),
            static_cast<qsizetype>(paragraph.text().size())));
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Owl Docs Branding Tests"));

    const QString root = QStringLiteral(OWL_DOCS_SOURCE_DIR);
    const QString desktop = readFile(
        root + QStringLiteral("/packaging/debian/owl-docs.desktop"));
    check(desktop.contains(QStringLiteral("Name=Owl Docs\n")),
          "desktop file has the wrong product name");
    check(desktop.contains(QStringLiteral("TryExec=owl-docs\n")) &&
              desktop.contains(QStringLiteral("Exec=owl-docs %f\n")) &&
              desktop.contains(QStringLiteral("Icon=owl-docs\n")) &&
              desktop.contains(QStringLiteral("StartupWMClass=owl-docs\n")),
          "desktop executable or icon identity is inconsistent");
    check(!desktop.contains(QStringLiteral("docx-studio"), Qt::CaseInsensitive),
          "desktop file retains the old product identity");

    const QString mime = readFile(
        root + QStringLiteral("/packaging/debian/owl-docs.xml"));
    checkXml(mime, "Owl Docs MIME metadata is not valid XML");
    check(mime.contains(QStringLiteral("Owl Docs")) &&
              mime.contains(QStringLiteral("*.docx")),
          "MIME metadata is missing branding or the DOCX glob");

    const QString icon = readFile(
        root + QStringLiteral("/packaging/icons/owl-docs.svg"));
    checkXml(icon, "Owl Docs icon is not valid SVG XML");
    check(icon.contains(QStringLiteral("<title id=\"title\">Owl Docs</title>")),
          "Owl Docs icon has no accessible title");
    check(icon.contains(QStringLiteral("width=\"128\"")) &&
              icon.contains(QStringLiteral("height=\"128\"")),
          "Owl Docs SVG has no explicit intrinsic size");
    check(icon.contains(QStringLiteral("low-poly geometric owl")) &&
              !icon.contains(QStringLiteral("<linearGradient")) &&
              !icon.contains(QStringLiteral("<filter")) &&
              !icon.contains(QStringLiteral("<circle")),
          "Owl Docs icon is not the flat geometric mascot artwork");
    for (const auto& color : {QStringLiteral("#E95420"),
                              QStringLiteral("#77216F"),
                              QStringLiteral("#5E2750")}) {
        check(icon.contains(color), "Owl Docs icon is missing an Ubuntu brand color");
    }
    check(readBytes(QStringLiteral(":/icons/owl-docs.svg")) ==
              readBytes(root + QStringLiteral("/packaging/icons/owl-docs.svg")),
          "embedded SVG does not match the packaged canonical artwork");

    constexpr std::array<int, 8> iconSizes{16, 24, 32, 48, 64, 128, 256, 512};
    const QIcon applicationIcon = docxstudio::app::owlDocsApplicationIcon();
    for (const int size : iconSizes) {
        const QString sourcePath =
            root + QStringLiteral("/packaging/icons/hicolor/%1x%1/apps/owl-docs.png")
                       .arg(size);
        const QString resourcePath =
            QStringLiteral(":/icons/owl-docs-%1.png").arg(size);
        const QByteArray sourceBytes = readBytes(sourcePath);
        check(readBytes(resourcePath) == sourceBytes,
              "embedded PNG does not match its packaged icon asset");
        QImage sourceImage;
        check(sourceImage.loadFromData(sourceBytes) &&
                  sourceImage.size() == QSize(size, size),
              "packaged icon PNG has the wrong dimensions");
        const QPixmap pixmap = applicationIcon.pixmap(size, size);
        check(!pixmap.isNull() && pixmap.size() == QSize(size, size),
              "application icon is missing an exact launcher size");
    }

    const QImage smallIcon = applicationIcon.pixmap(16, 16).toImage()
                                 .convertToFormat(QImage::Format_ARGB32);
    int transparentPixels = 0;
    int orangePixels = 0;
    int auberginePixels = 0;
    int lightPixels = 0;
    for (int y = 0; y < smallIcon.height(); ++y) {
        for (int x = 0; x < smallIcon.width(); ++x) {
            const QColor pixel = smallIcon.pixelColor(x, y);
            if (pixel.alpha() < 32) ++transparentPixels;
            if (pixel.alpha() < 128) continue;
            if (pixel.red() > 175 && pixel.green() > 35 &&
                pixel.green() < 155 && pixel.blue() < 125) {
                ++orangePixels;
            }
            if (pixel.red() > 45 && pixel.red() < 150 &&
                pixel.green() < 90 && pixel.blue() > 35 &&
                pixel.blue() < 145) {
                ++auberginePixels;
            }
            if (pixel.red() > 215 && pixel.green() > 190 &&
                pixel.blue() > 180) {
                ++lightPixels;
            }
        }
    }
    check(transparentPixels > 0 && orangePixels > 4 &&
              auberginePixels > 4 && lightPixels > 2,
          "16 px icon lost the owl silhouette or Ubuntu color contrast");
    const QPixmap renderedIcon =
        applicationIcon.pixmap(512, 512);
    check(!renderedIcon.isNull() && renderedIcon.size() == QSize(512, 512),
          "Owl Docs application icon cannot be rendered at 512 px");
    const QString renderPath = qEnvironmentVariable("OWL_DOCS_ICON_RENDER_PATH");
    if (!renderPath.isEmpty()) {
        check(renderedIcon.save(renderPath), "could not save the rendered Owl Docs icon");
    }

    docxstudio::app::MainWindow window;
    check(window.windowTitle().endsWith(QStringLiteral("Owl Docs")),
          "main window retains the old product title");
    check(!window.windowIcon().isNull(),
          "main window does not expose the geometric Owl Docs icon");
    check(window.windowIcon().pixmap(128, 128).toImage() ==
              docxstudio::app::owlDocsApplicationIcon()
                  .pixmap(128, 128).toImage(),
          "main window is not using the packaged geometric owl artwork");
    auto* fontFamily = window.findChild<QFontComboBox*>(
        QStringLiteral("ribbon.fontFamily"));
    auto* fontSize = window.findChild<QComboBox*>(
        QStringLiteral("ribbon.fontSize"));
    check(fontFamily && fontFamily->currentFont().family() ==
              QStringLiteral("Carlito"),
          "new-document ribbon does not show the Carlito default font");
    check(fontSize && fontSize->currentText() == QStringLiteral("11"),
          "new-document ribbon does not show the 11 point default size");
    window.resize(1280, 860);
    window.show();
    QApplication::processEvents();

    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* sizeEditor = fontSize ? fontSize->lineEdit() : nullptr;
    check(canvas && sizeEditor,
          "could not set up the focused editable-field shortcut test");
    check(canvas->currentTextColor().rgba() == QColor(Qt::black).rgba(),
          "new-document current text color is not black");

    canvas->insertText(QStringLiteral("tab"));
    canvas->setFocus();
    QApplication::processEvents();
    QKeyEvent tabEvent(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(canvas, &tabEvent);
    QApplication::processEvents();
    QWidget* focused = QApplication::focusWidget();
    check(focused == canvas || canvas->isAncestorOf(focused),
          "Tab moved focus from the document into the ribbon");
    check(paragraphTexts(*canvas) == QStringList{QStringLiteral("tab\t")},
          "Tab in the document did not insert an editor tab");
    canvas->selectAll();
    canvas->insertText({});

    auto* marginPresets = window.findChild<QComboBox*>(
        QStringLiteral("ribbon.margins"));
    check(marginPresets, "Layout ribbon has no margin choices");
    const QStringList expectedMarginIds{
        QStringLiteral("normal"), QStringLiteral("narrow"),
        QStringLiteral("moderate"), QStringLiteral("wide"),
        QStringLiteral("office2003"), QStringLiteral("custom")};
    QStringList actualMarginIds;
    for (int index = 0; index < marginPresets->count(); ++index) {
        const QString id = marginPresets->itemData(index).toString();
        if (id.isEmpty()) continue;
        check(!marginPresets->itemText(index).trimmed().isEmpty(),
              "a margin choice has no visible label");
        actualMarginIds.push_back(id);
    }
    check(actualMarginIds == expectedMarginIds,
          "margin choices are missing or their stable IDs/order changed");

    const auto marginsMatch = [canvas](const std::array<double, 4>& expected) {
        constexpr double tolerance = 0.01;
        return std::abs(canvas->marginTopPoints() - expected[0]) < tolerance &&
               std::abs(canvas->marginRightPoints() - expected[1]) < tolerance &&
               std::abs(canvas->marginBottomPoints() - expected[2]) < tolerance &&
               std::abs(canvas->marginLeftPoints() - expected[3]) < tolerance;
    };
    const auto activateMargin = [&](const QString& id,
                                    const std::array<double, 4>& expected) {
        const int index = marginPresets->findData(id);
        check(index >= 0, "could not find a margin choice by its stable ID");
        marginPresets->setCurrentIndex(index);
        check(QMetaObject::invokeMethod(
                  marginPresets, "activated", Qt::DirectConnection,
                  Q_ARG(int, index)),
              "could not activate a margin choice");
        QApplication::processEvents();
        check(marginsMatch(expected),
              "a margin preset applied the wrong page measurements");
    };
    activateMargin(QStringLiteral("normal"), {72, 72, 72, 72});
    activateMargin(QStringLiteral("narrow"), {36, 36, 36, 36});
    activateMargin(QStringLiteral("moderate"), {72, 54, 72, 54});
    activateMargin(QStringLiteral("wide"), {72, 144, 72, 144});
    activateMargin(QStringLiteral("office2003"), {72, 90, 72, 90});

    bool inspectedCustomMargins = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("customMarginsDialog"));
        check(dialog && dialog->isModal(),
              "Custom Margins did not open one modal dialog");
        auto* top = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("customMargins.top"));
        auto* right = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("customMargins.right"));
        auto* bottom = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("customMargins.bottom"));
        auto* left = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("customMargins.left"));
        const auto fields = dialog->findChildren<QDoubleSpinBox*>();
        check(top && right && bottom && left && fields.size() == 4 &&
                  dialog->isAncestorOf(top) && dialog->isAncestorOf(right) &&
                  dialog->isAncestorOf(bottom) && dialog->isAncestorOf(left),
              "Custom Margins does not contain all four fields in one popup");
        top->setValue(0.4);
        right->setValue(0.7);
        bottom->setValue(0.6);
        left->setValue(0.8);
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("customMargins.buttons"));
        check(buttons && buttons->button(QDialogButtonBox::Ok),
              "Custom Margins has no shared confirmation control");
        inspectedCustomMargins = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    const int customMarginIndex =
        marginPresets->findData(QStringLiteral("custom"));
    check(customMarginIndex >= 0,
          "Custom Margins choice has no stable ID");
    marginPresets->setCurrentIndex(customMarginIndex);
    check(QMetaObject::invokeMethod(
              marginPresets, "activated", Qt::DirectConnection,
              Q_ARG(int, customMarginIndex)),
          "could not activate Custom Margins");
    check(inspectedCustomMargins &&
              marginsMatch({0.4 * 72.0, 0.7 * 72.0,
                            0.6 * 72.0, 0.8 * 72.0}),
          "Custom Margins fields did not apply together");
    activateMargin(QStringLiteral("normal"), {72, 72, 72, 72});

    canvas->insertText(QStringLiteral("document clipboard"));
    canvas->selectAll();
    sizeEditor->setText(QStringLiteral("field clipboard"));
    sizeEditor->selectAll();
    sizeEditor->setFocus();
    QApplication::processEvents();
    QApplication::clipboard()->clear();
    QKeyEvent copyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier,
                        QStringLiteral("c"));
    QApplication::sendEvent(sizeEditor, &copyEvent);
    check(QApplication::clipboard()->text() == QStringLiteral("field clipboard"),
          "document-wide shortcuts stole Ctrl+C from a focused editable field");
    QKeyEvent boldEvent(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier);
    QApplication::sendEvent(sizeEditor, &boldEvent);
    check(!canvas->snapshot().document.paragraphs().front()
               .characterFormatAt(1).bold.value_or(false),
          "document formatting shortcut fired while an editable field had focus");
    canvas->setFocus();
    QApplication::processEvents();
    QKeyEvent canvasBoldEvent(QEvent::KeyPress, Qt::Key_B,
                              Qt::ControlModifier);
    QApplication::sendEvent(canvas, &canvasBoldEvent);
    check(canvas->snapshot().document.paragraphs().front()
              .characterFormatAt(1).bold.value_or(false),
          "canvas-scoped formatting shortcut did not fire for the document");

    auto* bulletButton = window.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.paragraph.bullets"));
    check(bulletButton && bulletButton->focusPolicy() == Qt::NoFocus,
          "Bullets is missing or can steal document focus");
    canvas->selectAll();
    canvas->insertText({});
    sizeEditor->setFocus();
    bulletButton->click();
    QApplication::processEvents();
    focused = QApplication::focusWidget();
    check(focused == canvas || canvas->isAncestorOf(focused),
          "Bullets did not return keyboard focus from a ribbon field to the page");
    QKeyEvent itemEvent(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier,
                        QStringLiteral("first"));
    QApplication::sendEvent(focused, &itemEvent);
    QKeyEvent returnEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(focused, &returnEvent);
    check(paragraphTexts(*canvas) ==
              QStringList{QStringLiteral("\u2022\tfirst"),
                          QStringLiteral("\u2022\t")},
          "click Bullets, type, Enter did not create the next bullet");
    bulletButton->click();
    QApplication::processEvents();
    check(paragraphTexts(*canvas) ==
              QStringList{QStringLiteral("\u2022\tfirst"), QString()},
          "clicking Bullets on a bulleted line inserted another marker");

    int alternateFont = fontFamily->currentIndex() == 0 ? 1 : 0;
    check(alternateFont >= 0 && alternateFont < fontFamily->count(),
          "font chooser has no alternate family for focus testing");
    fontFamily->setFocus();
    fontFamily->setCurrentIndex(alternateFont);
    QApplication::processEvents();
    check(QApplication::focusWidget() == fontFamily ||
              fontFamily->isAncestorOf(QApplication::focusWidget()),
          "browsing font choices stole focus before the choice was committed");
    const QString chosenFamily = fontFamily->currentText();
    check(QMetaObject::invokeMethod(fontFamily, "textActivated",
                                    Qt::DirectConnection,
                                    Q_ARG(QString, chosenFamily)),
          "could not commit a font choice");
    QApplication::processEvents();
    focused = QApplication::focusWidget();
    check(focused == canvas || canvas->isAncestorOf(focused),
          "committing a font choice did not return focus to the page");

    sizeEditor->setFocus();
    check(QMetaObject::invokeMethod(fontSize, "textActivated",
                                    Qt::DirectConnection,
                                    Q_ARG(QString, QStringLiteral("14"))),
          "could not commit a font-size choice");
    QApplication::processEvents();
    focused = QApplication::focusWidget();
    check((focused == canvas || canvas->isAncestorOf(focused)) &&
              qFuzzyCompare(canvas->currentFontPointSize(), 14.0),
          "committing a font size did not apply it and return focus to the page");

    canvas->selectAll();
    auto* yellowHighlight = window.findChild<QAction*>(
        QStringLiteral("ribbonColor.highlight.0"));
    auto* redText = window.findChild<QAction*>(
        QStringLiteral("ribbonColor.text.2"));
    auto* noHighlight = window.findChild<QAction*>(
        QStringLiteral("ribbonColor.highlight.none"));
    check(yellowHighlight && redText && noHighlight,
          "window color palettes are missing expected immediate actions");
    yellowHighlight->trigger();
    redText->trigger();
    noHighlight->trigger();
    QApplication::processEvents();
    const auto recolored = canvas->snapshot().document.paragraphs().front()
                               .characterFormatAt(1);
    check(!recolored.highlight_argb.has_value() &&
              recolored.foreground_argb == 0xffff0000U,
          "No Highlight failed or also removed the selected font color");

    auto* moreTextColors = window.findChild<QAction*>(
        QStringLiteral("ribbonColor.text.more"));
    check(moreTextColors, "text palette has no More Colors action");
    moreTextColors->trigger();
    QApplication::processEvents();
    auto* livePicker = window.findChild<QColorDialog*>(
        QStringLiteral("textColorPicker"));
    check(livePicker && livePicker->testOption(QColorDialog::NoButtons) &&
              livePicker->testOption(QColorDialog::DontUseNativeDialog) &&
              livePicker->windowModality() == Qt::NonModal,
          "More Colors still requires a Select button or locks the document");
    livePicker->setCurrentColor(QColor(QStringLiteral("#13579b")));
    canvas->setFocus();
    canvas->insertText(QStringLiteral("x"));
    check(canvas->snapshot().document.paragraphs().back()
              .characterFormatAt(1).foreground_argb == 0xff13579bU,
          "a live color click was not applied before the picker closed");
    livePicker->close();
    QApplication::processEvents();

    moreTextColors->trigger();
    QApplication::processEvents();
    check(window.findChild<QColorDialog*>(QStringLiteral("textColorPicker")),
          "could not reopen the modeless color picker");
    auto* newAction = window.findChild<QAction*>(QStringLiteral("file.new"));
    check(newAction, "New command is missing");
    newAction->trigger();
    QApplication::processEvents();
    check(!window.findChild<QColorDialog*>(QStringLiteral("textColorPicker")),
          "switching documents left the old document's color picker active");
    const auto canvases = window.findChildren<docxstudio::app::DocumentCanvas*>();
    docxstudio::app::DocumentCanvas* newCanvas = nullptr;
    for (auto* candidate : canvases) {
        if (candidate != canvas && candidate->isVisible()) newCanvas = candidate;
    }
    check(newCanvas &&
              newCanvas->currentTextColor().rgba() == QColor(Qt::black).rgba(),
          "a new tab inherited color state from another document's picker");

    const QPixmap renderedWindow = window.grab();
    check(!renderedWindow.isNull() && renderedWindow.width() == 1280 &&
              renderedWindow.height() == 860,
          "Owl Docs main window could not be rendered offscreen");
    const QString windowRenderPath =
        qEnvironmentVariable("OWL_DOCS_UI_RENDER_PATH");
    if (!windowRenderPath.isEmpty()) {
        check(renderedWindow.save(windowRenderPath),
              "could not save the rendered Owl Docs window");
    }
    window.hide();

    auto* closingWindow = new docxstudio::app::MainWindow;
    closingWindow->show();
    QApplication::processEvents();
    auto* closingPickerAction = closingWindow->findChild<QAction*>(
        QStringLiteral("ribbonColor.text.more"));
    check(closingPickerAction, "closing-window color action is missing");
    closingPickerAction->trigger();
    QApplication::processEvents();
    auto* closingPicker = closingWindow->findChild<QColorDialog*>(
        QStringLiteral("textColorPicker"));
    check(closingPicker, "closing-window color picker did not open");
    closingPicker->setCurrentColor(QColor(QStringLiteral("#2468ac")));
    delete closingWindow;

    std::cout << "Owl Docs branding tests passed\n";
    return 0;
}
