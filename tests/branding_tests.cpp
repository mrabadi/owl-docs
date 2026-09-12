#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/OwlDocsIcon.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QCryptographicHash>
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>

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

double meanVisibleDifference(const QImage& left, const QImage& right) {
    check(left.size() == right.size(), "icon comparison size mismatch");
    const QImage lhs = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage rhs = right.convertToFormat(QImage::Format_RGBA8888);
    std::uint64_t difference = 0;
    for (int y = 0; y < lhs.height(); ++y) {
        const auto* leftLine = lhs.constScanLine(y);
        const auto* rightLine = rhs.constScanLine(y);
        for (int x = 0; x < lhs.width(); ++x) {
            const int leftAlpha = leftLine[x * 4 + 3];
            const int rightAlpha = rightLine[x * 4 + 3];
            for (int channel = 0; channel < 3; ++channel) {
                const int leftPremultiplied =
                    (static_cast<int>(leftLine[x * 4 + channel]) * leftAlpha +
                     127) /
                    255;
                const int rightPremultiplied =
                    (static_cast<int>(rightLine[x * 4 + channel]) *
                         rightAlpha +
                     127) /
                    255;
                difference += static_cast<std::uint64_t>(std::abs(
                    leftPremultiplied - rightPremultiplied));
            }
            difference += static_cast<std::uint64_t>(
                std::abs(leftAlpha - rightAlpha));
        }
    }
    return static_cast<double>(difference) /
           static_cast<double>(lhs.width() * lhs.height() * 4);
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

    const QString masterPath =
        root + QStringLiteral("/packaging/icons/owl-docs-master.png");
    const QByteArray masterBytes = readBytes(masterPath);
    check(QCryptographicHash::hash(masterBytes, QCryptographicHash::Sha256)
                  .toHex() ==
              QByteArrayLiteral(
                  "058f151ffce9299c30d8cc068d17f21b4b5c34f3701b6b76641a1fa2f58c6205"),
          "canonical Owl Docs artwork is not the user-approved raster source");
    QImage masterImage;
    check(masterImage.loadFromData(masterBytes) &&
              masterImage.size() == QSize(1254, 1254),
          "canonical Owl Docs artwork has the wrong dimensions");
    int masterTransparentPixels = 0;
    int masterOpaquePixels = 0;
    for (int y = 0; y < masterImage.height(); ++y) {
        for (int x = 0; x < masterImage.width(); ++x) {
            const int alpha = masterImage.pixelColor(x, y).alpha();
            if (alpha < 8) ++masterTransparentPixels;
            if (alpha > 247) ++masterOpaquePixels;
        }
    }
    const int masterArea = masterImage.width() * masterImage.height();
    check(masterImage.hasAlphaChannel() &&
              masterTransparentPixels > masterArea * 2 / 5 &&
              masterOpaquePixels > masterArea / 2 &&
              masterImage.pixelColor(0, 0).alpha() == 0 &&
              masterImage.pixelColor(masterImage.width() / 2,
                                     masterImage.height() / 2)
                      .alpha() > 247,
          "canonical Owl Docs artwork lost its transparent background");
    check(!QFile::exists(root + QStringLiteral("/packaging/icons/owl-docs.svg")) &&
              !QFile::exists(QStringLiteral(":/icons/owl-docs.svg")),
          "stale vector artwork is still packaged or embedded");

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
        const QImage reference = masterImage.scaled(
            QSize(size, size), Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
        check(meanVisibleDifference(sourceImage, reference) < 6.0,
              "packaged icon does not preserve the canonical composition");
        int transparentPixels = 0;
        int opaquePixels = 0;
        for (int y = 0; y < sourceImage.height(); ++y) {
            for (int x = 0; x < sourceImage.width(); ++x) {
                const int alpha = sourceImage.pixelColor(x, y).alpha();
                if (alpha < 8) ++transparentPixels;
                if (alpha > 247) ++opaquePixels;
            }
        }
        const int area = size * size;
        check(sourceImage.hasAlphaChannel() &&
                  transparentPixels > area / 4 && opaquePixels > area / 3 &&
                  sourceImage.pixelColor(0, 0).alpha() < 8 &&
                  sourceImage.pixelColor(size - 1, size - 1).alpha() < 8 &&
                  sourceImage.pixelColor(size / 2, size / 2).alpha() > 240,
              "packaged icon lost the transparent owl silhouette");
        check(applicationIcon.availableSizes().contains(QSize(size, size)),
              "application icon does not advertise a packaged launcher size");
        const QPixmap pixmap = applicationIcon.pixmap(size, size);
        check(!pixmap.isNull() && pixmap.size() == QSize(size, size),
              "application icon is missing an exact launcher size");
    }

    for (const int smallSize : {16, 24, 32}) {
        const QImage smallIcon =
            applicationIcon.pixmap(smallSize, smallSize).toImage()
                .convertToFormat(QImage::Format_ARGB32);
        int transparentPixels = 0;
        int visiblePixels = 0;
        int orangePixels = 0;
        int lightPixels = 0;
        int darkPixels = 0;
        int minimumLuma = 255;
        int maximumLuma = 0;
        std::set<QRgb> colors;
        for (int y = 0; y < smallIcon.height(); ++y) {
            for (int x = 0; x < smallIcon.width(); ++x) {
                const QColor pixel = smallIcon.pixelColor(x, y);
                if (pixel.alpha() < 8) ++transparentPixels;
                if (pixel.alpha() < 128) continue;
                ++visiblePixels;
                colors.insert(pixel.rgba());
                const int luma = qGray(pixel.rgb());
                minimumLuma = std::min(minimumLuma, luma);
                maximumLuma = std::max(maximumLuma, luma);
                if (pixel.red() > 190 && pixel.green() > 60 &&
                    pixel.green() < 170 && pixel.blue() < 120) {
                    ++orangePixels;
                }
                if (pixel.red() > 210 && pixel.green() > 130 &&
                    pixel.blue() > 100) {
                    ++lightPixels;
                }
                if (pixel.red() < 95 && pixel.green() < 45 &&
                    pixel.blue() < 95) {
                    ++darkPixels;
                }
            }
        }
        const int area = smallSize * smallSize;
        check(transparentPixels > area / 4 && visiblePixels > area / 2 &&
                  colors.size() > static_cast<std::size_t>(area / 3) &&
                  orangePixels > area / 10 && lightPixels > area / 20 &&
                  darkPixels > area / 20 &&
                  maximumLuma - minimumLuma > 180 &&
                  smallIcon.pixelColor(0, 0).alpha() < 8 &&
                  smallIcon.pixelColor(smallSize - 1, smallSize - 1)
                          .alpha() < 8 &&
                  smallIcon.pixelColor(smallSize / 2, smallSize / 2)
                          .alpha() > 240,
              "small icon lost transparency, composition, detail, or contrast");
    }
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
