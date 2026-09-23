#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RecoveryCodec.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using docxstudio::app::DocumentCanvas;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> encodedPng(const QColor& color,
                                     int width = 40, int height = 24) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not encode PNG fixture");
    return {
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size()};
}

std::vector<std::uint8_t> encodedJpeg(const QColor& color,
                                      int width = 40, int height = 24) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPEG"),
          "could not encode JPEG fixture");
    return {
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size()};
}

QByteArray asByteArray(const std::vector<std::uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<qsizetype>(bytes.size()));
}

std::vector<docxstudio::core::ImageAtom> imageAtoms(
    const DocumentCanvas& canvas) {
    std::vector<docxstudio::core::ImageAtom> result;
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        result.insert(result.end(), paragraph.images().begin(),
                      paragraph.images().end());
    }
    return result;
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void clickViewport(DocumentCanvas& canvas, const QPoint& position) {
    const QPoint global = canvas.viewport()->mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                      QPointF(position), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position),
                        QPointF(position), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas.viewport(), &release);
}

QRect inputMethodCursorRect(DocumentCanvas& canvas) {
    QInputMethodQueryEvent event(Qt::ImCursorRectangle);
    QApplication::sendEvent(&canvas, &event);
    return event.value(Qt::ImCursorRectangle).toRect();
}

void pressViewport(DocumentCanvas& canvas, const QPoint& position,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPoint global = canvas.viewport()->mapToGlobal(position);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                      QPointF(position), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, modifiers);
    QApplication::sendEvent(canvas.viewport(), &press);
}

void moveViewport(DocumentCanvas& canvas, const QPoint& position,
                  Qt::MouseButtons buttons = Qt::NoButton,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPoint global = canvas.viewport()->mapToGlobal(position);
    QMouseEvent move(QEvent::MouseMove, QPointF(position), QPointF(position),
                     QPointF(global), Qt::NoButton, buttons, modifiers);
    QApplication::sendEvent(canvas.viewport(), &move);
}

void releaseViewport(DocumentCanvas& canvas, const QPoint& position,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPoint global = canvas.viewport()->mapToGlobal(position);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position),
                        QPointF(position), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, modifiers);
    QApplication::sendEvent(canvas.viewport(), &release);
}

void dragViewport(DocumentCanvas& canvas, const QPoint& start,
                  const QPoint& end,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    pressViewport(canvas, start, modifiers);
    moveViewport(canvas, end, Qt::LeftButton, modifiers);
    releaseViewport(canvas, end, modifiers);
}

QRect redBounds(const QImage& image) {
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 230 || color.green() > 30 ||
                color.blue() > 30) {
                continue;
            }
            result = result.isValid() ? result.united(QRect(x, y, 1, 1))
                                      : QRect(x, y, 1, 1);
        }
    }
    return result;
}

QRect colorBounds(const QImage& image, const QColor& target,
                  int tolerance = 20) {
    QRect result;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (std::abs(color.red() - target.red()) > tolerance ||
                std::abs(color.green() - target.green()) > tolerance ||
                std::abs(color.blue() - target.blue()) > tolerance) {
                continue;
            }
            result = result.isValid() ? result.united(QRect(x, y, 1, 1))
                                      : QRect(x, y, 1, 1);
        }
    }
    return result;
}

QImage rasterizeFirstPdfPage(const QString& pdfPath,
                             const QString& outputRoot) {
    const QString pdftoppm = QStandardPaths::findExecutable(
        QStringLiteral("pdftoppm"),
        {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    check(!pdftoppm.isEmpty(),
          "pdftoppm is required for picture-layout PDF tests");
    QProcess rasterizer;
    rasterizer.start(
        pdftoppm,
        {QStringLiteral("-f"), QStringLiteral("1"),
         QStringLiteral("-singlefile"), QStringLiteral("-png"),
         QStringLiteral("-r"), QStringLiteral("96"), pdfPath,
         outputRoot});
    const bool started = rasterizer.waitForStarted(5000);
    const bool finished = started && rasterizer.waitForFinished(15000);
    if (!finished || rasterizer.exitStatus() != QProcess::NormalExit ||
        rasterizer.exitCode() != 0) {
        std::cerr << rasterizer.readAllStandardError().constData() << '\n';
    }
    check(finished && rasterizer.exitStatus() == QProcess::NormalExit &&
              rasterizer.exitCode() == 0,
          "pdftoppm could not rasterize picture-layout PDF");
    const QImage rendered(outputRoot + QStringLiteral(".png"));
    check(!rendered.isNull(),
          "pdftoppm did not produce a picture-layout raster");
    return rendered;
}

void testCanvasObjectLifecycle(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 520);
    canvas.show();
    canvas.setFocus();
    canvas.insertText(QStringLiteral("BeforeAfter"));
    for (int index = 0; index < 5; ++index) sendKey(canvas, Qt::Key_Left);

    const auto png = encodedPng(Qt::red);
    check(canvas.insertInlineImage(png, QStringLiteral("red fixture.png")),
          "canvas rejected a valid PNG picture");
    auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().front().text() ==
              std::u16string(u"Before\ufffcAfter"),
          "core snapshot did not contain the inline-object replacement character");
    check(snapshot.document.paragraphs().front().images().size() == 1 &&
              canvas.selectedInlineImageId().has_value(),
          "inserted picture was not retained and selected");
    const auto imageId = *canvas.selectedInlineImageId();
    auto image = snapshot.document.paragraphs().front().images().front();
    check(image.utf16_offset == 6 &&
              image.format == docxstudio::core::ImageFormat::png &&
              image.encoded_payload.size() == png.size() &&
              image.accessible_name == "red fixture.png",
          "inserted picture lost its text anchor or source payload");
    check(canvas.selection() == docxstudio::core::Range{
              {snapshot.document.paragraphs().front().id(), 6},
              {snapshot.document.paragraphs().front().id(), 7}},
          "insertion did not select exactly the semantic image atom");

    constexpr double kEmuPerPoint = 12700.0;
    const double originalWidth =
        static_cast<double>(image.width_emu) / kEmuPerPoint;
    const double originalHeight =
        static_cast<double>(image.height_emu) / kEmuPerPoint;
    const auto beforeResizeRevision = snapshot.revision;
    check(canvas.resizeSelectedInlineImage(
              originalWidth * 2.0, originalHeight * 2.0),
          "picture resize command failed");
    snapshot = canvas.snapshot();
    check(snapshot.revision.value() == beforeResizeRevision.value() + 1,
          "picture resize did not increment the document revision");
    image = snapshot.document.paragraphs().front().images().front();
    check(std::abs(static_cast<double>(image.width_emu) / kEmuPerPoint -
                       originalWidth * 2.0) < 0.01,
          "picture resize did not update semantic geometry");
    const auto resizedPayload = image.encoded_payload.bytes();
    check(resizedPayload.size() == png.size() &&
              std::equal(resizedPayload.begin(), resizedPayload.end(),
                         png.begin()),
          "picture resize resampled or rewrote the original image payload");

    QString previewSummary;
    QString previewError;
    const auto paragraphId = snapshot.document.paragraphs().front().id();
    check(!canvas.createOperationsPreview(
              beforeResizeRevision,
              {docxstudio::core::InsertText{
                  {paragraphId, 0}, u"stale", std::nullopt}},
              QStringLiteral("stale preview"), previewSummary, previewError) &&
              previewError.contains(QStringLiteral("revision"),
                                    Qt::CaseInsensitive),
          "resize did not make an older Codex preview revision stale");

    canvas.undo();
    image = imageAtoms(canvas).front();
    check(std::abs(static_cast<double>(image.width_emu) / kEmuPerPoint -
                       originalWidth) < 0.01,
          "Undo did not restore picture dimensions");
    canvas.redo();
    image = imageAtoms(canvas).front();
    check(std::abs(static_cast<double>(image.width_emu) / kEmuPerPoint -
                       originalWidth * 2.0) < 0.01,
          "Redo did not restore resized picture dimensions");

    check(canvas.selectInlineImage(imageId),
          "could not reselect semantic image for navigation test");
    sendKey(canvas, Qt::Key_Left);
    check(canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.utf16_offset == 6,
          "Left did not collapse an image selection before the atom");
    check(canvas.selectInlineImage(imageId),
          "could not reselect semantic image for right-navigation test");
    sendKey(canvas, Qt::Key_Right);
    check(canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.utf16_offset == 7,
          "Right did not collapse an image selection after the atom");
    QApplication::processEvents();
    const QImage rendered = canvas.viewport()->grab().toImage();
    const QRect red = redBounds(rendered);
    check(red.isValid(), "inline picture was not rendered on the page");
    const int screenPageCount = canvas.pageCount();
    const std::uint64_t screenLayoutGeneration = canvas.layoutGeneration();

    QTemporaryDir pdfOutput;
    check(pdfOutput.isValid(), "could not create picture PDF test directory");
    const QString pdfPath =
        pdfOutput.filePath(QStringLiteral("inline-picture.pdf"));
    QString pdfError;
    check(canvas.exportPdf(pdfPath, pdfError) &&
              QFileInfo(pdfPath).size() > 500,
          "shared page renderer did not export the inline picture PDF");
    check(canvas.pageCount() == screenPageCount &&
              canvas.layoutGeneration() == screenLayoutGeneration,
          "PDF export repaginated or changed the screen layout result");
    const QString pdftoppm = QStandardPaths::findExecutable(
        QStringLiteral("pdftoppm"),
        {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    check(!pdftoppm.isEmpty(),
          "pdftoppm is required for inline-picture PDF conformance tests");
    const QString renderedPdfRoot =
        pdfOutput.filePath(QStringLiteral("inline-picture-page"));
    QProcess rasterizer;
    rasterizer.start(
        pdftoppm,
        {QStringLiteral("-f"), QStringLiteral("1"),
         QStringLiteral("-singlefile"), QStringLiteral("-png"),
         QStringLiteral("-r"), QStringLiteral("96"), pdfPath,
         renderedPdfRoot});
    const bool rasterStarted = rasterizer.waitForStarted(5000);
    const bool rasterFinished = rasterStarted &&
        rasterizer.waitForFinished(15000);
    if (!rasterFinished || rasterizer.exitStatus() != QProcess::NormalExit ||
        rasterizer.exitCode() != 0) {
        std::cerr << rasterizer.readAllStandardError().constData() << '\n';
    }
    check(rasterFinished &&
              rasterizer.exitStatus() == QProcess::NormalExit &&
              rasterizer.exitCode() == 0,
          "pdftoppm could not rasterize the inline-picture PDF");
    const QImage renderedPdf(renderedPdfRoot + QStringLiteral(".png"));
    const QRect pdfRed = redBounds(renderedPdf);
    check(!renderedPdf.isNull() && pdfRed.isValid(),
          "the exported PDF omitted or recolored the inline picture");
    check(std::abs(pdfRed.width() - red.width()) <= 3 &&
              std::abs(pdfRed.height() - red.height()) <= 3,
          "screen and PDF used different laid-out picture bounds");
    const QString pdftotext = QStandardPaths::findExecutable(
        QStringLiteral("pdftotext"),
        {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    check(!pdftotext.isEmpty(),
          "pdftotext is required for inline-picture PDF conformance tests");
    QProcess extractor;
    extractor.start(pdftotext, {pdfPath, QStringLiteral("-")});
    check(extractor.waitForStarted(5000) &&
              extractor.waitForFinished(10000) &&
              extractor.exitStatus() == QProcess::NormalExit &&
              extractor.exitCode() == 0,
          "pdftotext could not inspect the inline-picture PDF");
    const QByteArray pdfText = extractor.readAllStandardOutput();
    check(pdfText.contains("Before") && pdfText.contains("After"),
          "text around the exported inline picture was not searchable");

    clickViewport(canvas, red.center());
    snapshot = canvas.snapshot();
    check(canvas.selectedInlineImageId() == imageId &&
              canvas.selection() == docxstudio::core::Range{
                  {snapshot.document.paragraphs().front().id(), 6},
                  {snapshot.document.paragraphs().front().id(), 7}},
          "clicking a picture did not select the picture object");

    canvas.copy();
    const QMimeData* copied = QApplication::clipboard()->mimeData();
    check(copied &&
              copied->data(QStringLiteral("image/png")) == asByteArray(png) &&
              copied->hasFormat(QStringLiteral(
                  "application/x-owl-docs-inline-image-v1")),
          "copy did not expose the original raw PNG plus native metadata");

    sendKey(canvas, Qt::Key_Delete);
    check(imageAtoms(canvas).empty() &&
              canvas.snapshot().document.paragraphs().front().text() ==
                  u"BeforeAfter",
          "Delete did not remove the selected picture");
    canvas.undo();
    check(imageAtoms(canvas).size() == 1,
          "Undo did not atomically restore the deleted picture");

    check(canvas.selectInlineImage(imageId),
          "could not select restored picture for Backspace test");
    sendKey(canvas, Qt::Key_Right);
    sendKey(canvas, Qt::Key_Backspace);
    check(imageAtoms(canvas).empty(),
          "Backspace after a picture did not delete its semantic atom");
    canvas.undo();
    check(canvas.selectInlineImage(imageId),
          "could not select restored picture for forward Delete test");
    sendKey(canvas, Qt::Key_Left);
    sendKey(canvas, Qt::Key_Delete);
    check(imageAtoms(canvas).empty(),
          "Delete before a picture did not delete its semantic atom");
    canvas.undo();

    check(canvas.selectInlineImage(imageId),
          "could not select restored picture for replacement test");
    canvas.insertText(QStringLiteral("X"));
    check(imageAtoms(canvas).empty() &&
              canvas.snapshot().document.paragraphs().front().text() ==
                  u"BeforeXAfter",
          "typing over a picture did not replace the semantic atom");
    canvas.undo();

    sendKey(canvas, Qt::Key_A, Qt::ControlModifier);
    sendKey(canvas, Qt::Key_Delete);
    check(imageAtoms(canvas).empty() &&
              canvas.snapshot().document.paragraphs().front().text().empty(),
          "Ctrl+A followed by Delete did not remove text and image atoms");
    canvas.undo();

    const auto beforeCut = imageAtoms(canvas).front();
    check(canvas.selectInlineImage(beforeCut.id),
          "could not select restored picture for clipboard round-trip");
    canvas.cut();
    check(imageAtoms(canvas).empty(), "Cut did not remove the picture atom");
    canvas.paste();
    const auto pasted = imageAtoms(canvas);
    check(pasted.size() == 1 &&
              pasted.front().encoded_payload == beforeCut.encoded_payload &&
              pasted.front().accessible_name == beforeCut.accessible_name &&
              pasted.front().width_emu == beforeCut.width_emu &&
              pasted.front().height_emu == beforeCut.height_emu,
          "native clipboard round-trip lost image bytes, name, or geometry");

    std::string error;
    const auto recoverySource = canvas.snapshot().document;
    const auto encoded = docxstudio::app::RecoveryCodec::encode(
        {recoverySource, {}}, error);
    check(encoded.has_value(),
          "recovery codec rejected a valid inline picture");
    const auto decoded = docxstudio::app::RecoveryCodec::decode(*encoded, error);
    check(decoded && decoded->document == recoverySource &&
              decoded->document.paragraphs().front().images().size() == 1,
          "recovery codec did not preserve inline-picture source and geometry");
}

void testPicturePropertiesAndClipboardVersions(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas source(spelling);
    const auto png = encodedPng(Qt::red, 52, 31);
    check(source.insertInlineImage(png, QStringLiteral("Original alt text")),
          "could not insert picture-properties fixture");
    const auto imageId = *source.selectedInlineImageId();
    check(source.resizeSelectedInlineImage(123.0, 73.0),
          "could not configure clipboard geometry fixture");

    const docxstudio::core::ImageLayout squareLayout{
        docxstudio::core::ImagePlacement::square,
        1100, 2200, 3300, 4400, false};
    auto beforeLayout = source.snapshot();
    check(source.setSelectedImageLayout(squareLayout),
          "could not set picture layout");
    auto afterLayout = source.snapshot();
    check(afterLayout.revision.value() == beforeLayout.revision.value() + 1 &&
              imageAtoms(source).front().layout == squareLayout,
          "one picture-layout action was not exactly one semantic revision");
    source.undo();
    check(imageAtoms(source).front().layout ==
              docxstudio::core::ImageLayout{},
          "Undo did not restore the previous picture layout");
    source.redo();
    check(imageAtoms(source).front().layout == squareLayout,
          "Redo did not restore the picture layout");
    const auto beforeNoOpLayout = source.snapshot();
    check(source.setSelectedImageLayout(squareLayout) &&
              source.snapshot().revision == beforeNoOpLayout.revision,
          "reapplying an identical picture layout created a revision");

    const QString revisedAltText = QStringLiteral("Quarterly owl diagram");
    const auto beforeAltText = source.snapshot();
    check(source.setSelectedImageAccessibleName(revisedAltText),
          "could not set picture alt text");
    const auto afterAltText = source.snapshot();
    check(afterAltText.revision.value() ==
                  beforeAltText.revision.value() + 1 &&
              imageAtoms(source).front().accessible_name ==
                  revisedAltText.toStdString(),
          "one picture alt-text action was not exactly one semantic revision");
    source.undo();
    check(imageAtoms(source).front().accessible_name == "Original alt text" &&
              imageAtoms(source).front().layout == squareLayout,
          "Undo of alt text also changed picture layout or failed to restore the name");
    source.redo();
    check(imageAtoms(source).front().accessible_name ==
              revisedAltText.toStdString(),
          "Redo did not restore picture alt text");
    const auto beforeNoOpAltText = source.snapshot();
    check(source.setSelectedImageAccessibleName(revisedAltText) &&
              source.snapshot().revision == beforeNoOpAltText.revision,
          "reapplying identical picture alt text created a revision");

    check(source.selectInlineImage(imageId),
          "could not select picture for native clipboard test");
    source.copy();
    const QMimeData* copied = QApplication::clipboard()->mimeData();
    check(copied && copied->hasFormat(QStringLiteral(
                        "application/x-owl-docs-inline-image-v2")) &&
              copied->hasFormat(QStringLiteral(
                  "application/x-owl-docs-inline-image-v1")),
          "picture copy did not publish both current and legacy native formats");
    const QByteArray currentPayload = copied->data(QStringLiteral(
        "application/x-owl-docs-inline-image-v2"));
    const QByteArray legacyPayload = copied->data(QStringLiteral(
        "application/x-owl-docs-inline-image-v1"));
    check(currentPayload.startsWith(QByteArrayLiteral("OWLDIMG2")) &&
              legacyPayload.startsWith(QByteArrayLiteral("OWLDIMG1")),
          "native clipboard payloads used the wrong protocol magic");
    const auto expected = imageAtoms(source).front();

    auto* currentMime = new QMimeData;
    currentMime->setData(
        QStringLiteral("application/x-owl-docs-inline-image-v2"),
        currentPayload);
    QApplication::clipboard()->setMimeData(currentMime);
    DocumentCanvas currentPaste(spelling);
    const auto beforeCurrentPaste = currentPaste.snapshot();
    currentPaste.paste();
    const auto currentImages = imageAtoms(currentPaste);
    check(currentPaste.snapshot().revision.value() ==
                  beforeCurrentPaste.revision.value() + 1 &&
              currentImages.size() == 1 &&
              currentImages.front().encoded_payload ==
                  expected.encoded_payload &&
              currentImages.front().format == expected.format &&
              currentImages.front().width_emu == expected.width_emu &&
              currentImages.front().height_emu == expected.height_emu &&
              currentImages.front().accessible_name ==
                  expected.accessible_name &&
              currentImages.front().layout == expected.layout,
          "native v2 clipboard paste lost bytes, format, geometry, alt text, or layout");

    auto* legacyMime = new QMimeData;
    legacyMime->setData(
        QStringLiteral("application/x-owl-docs-inline-image-v1"),
        legacyPayload);
    QApplication::clipboard()->setMimeData(legacyMime);
    DocumentCanvas legacyPaste(spelling);
    const auto beforeLegacyPaste = legacyPaste.snapshot();
    legacyPaste.paste();
    const auto legacyImages = imageAtoms(legacyPaste);
    check(legacyPaste.snapshot().revision.value() ==
                  beforeLegacyPaste.revision.value() + 1 &&
              legacyImages.size() == 1 &&
              legacyImages.front().encoded_payload ==
                  expected.encoded_payload &&
              legacyImages.front().format == expected.format &&
              legacyImages.front().width_emu == expected.width_emu &&
              legacyImages.front().height_emu == expected.height_emu &&
              legacyImages.front().accessible_name ==
                  expected.accessible_name &&
              legacyImages.front().layout ==
                  docxstudio::core::ImageLayout{},
          "legacy v1 clipboard paste was rejected or did not use default layout semantics");
}

void testCanonicalAnchorOriginsAndSharedPdfGeometry(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(1000, 700);
    canvas.show();
    canvas.setFocus();
    canvas.insertText(QStringLiteral("Prefix "));
    const auto png = encodedPng(Qt::red, 64, 40);
    check(canvas.insertInlineImage(png, QStringLiteral("origin fixture")),
          "could not insert canonical-anchor fixture");
    const auto imageId = *canvas.selectedInlineImageId();

    docxstudio::core::ImageLayout moving{
        docxstudio::core::ImagePlacement::square, 0, 0, 0, 0, true};
    check(canvas.setSelectedImageLayout(moving),
          "could not configure moving canonical anchor");
    sendKey(canvas, Qt::Key_Right);
    QApplication::processEvents();
    const QRect movingScreen = colorBounds(
        canvas.viewport()->grab().toImage(), Qt::red);
    check(movingScreen.isValid(),
          "moving canonical anchor did not render on the canvas");

    // Wrap distances expand the text exclusion only. They are not wp:posOffset
    // and therefore must not translate or rescale the picture rectangle.
    constexpr std::int64_t kEighteenPointsEmu = 18 * 12700;
    moving.distance_top_emu = kEighteenPointsEmu;
    moving.distance_right_emu = kEighteenPointsEmu;
    moving.distance_bottom_emu = kEighteenPointsEmu;
    moving.distance_left_emu = kEighteenPointsEmu;
    check(canvas.selectInlineImage(imageId) &&
              canvas.setSelectedImageLayout(moving),
          "could not configure canonical wrap distances");
    sendKey(canvas, Qt::Key_Right);
    QApplication::processEvents();
    const QRect spacedScreen = colorBounds(
        canvas.viewport()->grab().toImage(), Qt::red);
    check(spacedScreen.isValid() &&
              std::abs(spacedScreen.left() - movingScreen.left()) <= 1 &&
              std::abs(spacedScreen.top() - movingScreen.top()) <= 1 &&
              std::abs(spacedScreen.width() - movingScreen.width()) <= 1 &&
              std::abs(spacedScreen.height() - movingScreen.height()) <= 1,
          "wrap distances incorrectly changed the canvas picture rectangle");

    QTemporaryDir output;
    check(output.isValid(),
          "could not create canonical-anchor PDF directory");
    const QString movingPdfPath =
        output.filePath(QStringLiteral("moving-anchor.pdf"));
    QString error;
    check(canvas.exportPdf(movingPdfPath, error),
          "could not export moving-anchor PDF");
    const QRect movingPdf = colorBounds(
        rasterizeFirstPdfPage(
            movingPdfPath,
            output.filePath(QStringLiteral("moving-anchor-page"))),
        Qt::red);
    check(movingPdf.isValid(),
          "moving anchor was absent from the shared PDF renderer");

    auto fixed = moving;
    fixed.move_with_text = false;
    check(canvas.selectInlineImage(imageId) &&
              canvas.setSelectedImageLayout(fixed),
          "could not configure fixed canonical anchor");
    sendKey(canvas, Qt::Key_Right);
    QApplication::processEvents();
    const QRect fixedScreen = colorBounds(
        canvas.viewport()->grab().toImage(), Qt::red);
    check(fixedScreen.isValid(),
          "fixed canonical anchor did not render on the canvas");
    const QString fixedPdfPath =
        output.filePath(QStringLiteral("fixed-anchor.pdf"));
    error.clear();
    check(canvas.exportPdf(fixedPdfPath, error),
          "could not export fixed-anchor PDF");
    const QRect fixedPdf = colorBounds(
        rasterizeFirstPdfPage(
            fixedPdfPath,
            output.filePath(QStringLiteral("fixed-anchor-page"))),
        Qt::red);
    check(fixedPdf.isValid() && fixedPdf.left() <= 1 && fixedPdf.top() <= 1,
          "fixed page/page zero-offset anchor was not at the PDF page origin");

    const QPoint screenDelta = movingScreen.topLeft() -
                               fixedScreen.topLeft();
    const QPoint pdfDelta = movingPdf.topLeft() - fixedPdf.topLeft();
    check(screenDelta.x() > 90 && screenDelta.y() > 90 &&
              std::abs(screenDelta.x() - pdfDelta.x()) <= 3 &&
              std::abs(screenDelta.y() - pdfDelta.y()) <= 3 &&
              std::abs(movingPdf.width() - fixedPdf.width()) <= 1 &&
              std::abs(movingPdf.height() - fixedPdf.height()) <= 1,
          "canvas/PDF disagreed on character/paragraph versus page/page anchor origins");
    canvas.hide();
}

void testMixedAnchorNonOverlapAndTopmostHitTarget(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas mixed(spelling);
    mixed.resize(900, 620);
    mixed.show();
    mixed.setFocus();
    const auto redPng = encodedPng(Qt::red, 80, 48);
    const auto greenPng = encodedPng(Qt::green, 80, 48);
    check(mixed.insertInlineImage(redPng, QStringLiteral("square")),
          "could not insert mixed square anchor");
    const auto squareId = *mixed.selectedInlineImageId();
    check(mixed.setSelectedImageLayout({
              docxstudio::core::ImagePlacement::square,
              0, 0, 0, 0, true}),
          "could not configure mixed square anchor");
    sendKey(mixed, Qt::Key_Right);
    check(mixed.insertInlineImage(greenPng, QStringLiteral("top-bottom")),
          "could not insert mixed top/bottom anchor");
    check(mixed.setSelectedImageLayout({
              docxstudio::core::ImagePlacement::top_and_bottom,
              0, 0, 0, 0, true}),
          "could not configure mixed top/bottom anchor");
    sendKey(mixed, Qt::Key_Right);
    QApplication::processEvents();
    const QImage mixedRaster = mixed.viewport()->grab().toImage();
    const QRect red = colorBounds(mixedRaster, Qt::red);
    const QRect green = colorBounds(mixedRaster, Qt::green);
    check(red.isValid() && green.isValid() &&
              green.top() >= red.bottom() - 1 &&
              !red.intersects(green),
          "square followed by top/bottom anchors overlapped on the canvas");
    check(mixed.selectInlineImage(squareId),
          "mixed anchor selection was lost after layout");
    mixed.hide();

    // Fixed anchors in separate paragraphs intentionally share the canonical
    // page/page zero origin. Rendering paints the later paragraph last, so a
    // click in the overlap must select that visually topmost picture.
    DocumentCanvas overlap(spelling);
    overlap.resize(900, 620);
    overlap.show();
    overlap.setFocus();
    const auto bluePng = encodedPng(Qt::blue, 80, 48);
    check(overlap.insertInlineImage(redPng, QStringLiteral("back")),
          "could not insert backmost fixed anchor");
    check(overlap.setSelectedImageLayout({
              docxstudio::core::ImagePlacement::square,
              0, 0, 0, 0, false}),
          "could not configure backmost fixed anchor");
    sendKey(overlap, Qt::Key_Right);
    sendKey(overlap, Qt::Key_Return);
    check(overlap.insertInlineImage(bluePng, QStringLiteral("front")),
          "could not insert topmost fixed anchor");
    const auto frontId = *overlap.selectedInlineImageId();
    check(overlap.setSelectedImageLayout({
              docxstudio::core::ImagePlacement::square,
              0, 0, 0, 0, false}),
          "could not configure topmost fixed anchor");
    sendKey(overlap, Qt::Key_Right);
    QApplication::processEvents();
    const QRect blue = colorBounds(
        overlap.viewport()->grab().toImage(), Qt::blue);
    check(blue.isValid(),
          "could not locate topmost fixed anchor for hit testing");
    clickViewport(overlap, blue.center());
    check(overlap.selectedInlineImageId() == frontId,
          "overlapping picture hit-test did not follow reverse paint order");
    overlap.hide();
}

void testAnchoredMultipageFlowIsPageLocal(
    docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(1000, 1000);
    canvas.setPageSizePoints(320.0, 220.0);
    canvas.setMarginsPoints(24.0, 24.0, 24.0, 24.0);
    canvas.show();
    canvas.setFocus();

    // The fitted image occupies almost the full first-page content height. A
    // hard-line sequence then crosses onto page two while its square exclusion
    // is still active on page one.
    const auto png = encodedPng(Qt::red, 120, 230);
    check(canvas.insertInlineImage(png, QStringLiteral("page-one square")),
          "could not insert multipage square fixture");
    check(canvas.setSelectedImageLayout({
              docxstudio::core::ImagePlacement::square,
              0, 0, 0, 0, true}),
          "could not configure multipage square fixture");
    sendKey(canvas, Qt::Key_Right);

    QString flow;
    constexpr int kLineCount = 18;
    for (int index = 0; index < kLineCount; ++index) {
        if (index > 0) flow += QChar::LineSeparator;
        flow += QStringLiteral("L%1").arg(index, 2, 10, QLatin1Char('0'));
    }
    flow += QStringLiteral("\nFOLLOW");
    canvas.insertText(flow);
    QApplication::processEvents();

    std::vector<int> pages;
    std::vector<int> cursorXs;
    std::vector<int> cursorYs;
    pages.reserve(kLineCount);
    cursorXs.reserve(kLineCount);
    cursorYs.reserve(kLineCount);
    for (int index = 0; index < kLineCount; ++index) {
        const QString marker = QStringLiteral("L%1").arg(
            index, 2, 10, QLatin1Char('0'));
        check(canvas.findNext(marker, true),
              "could not locate a multipage hard-line marker");
        const QRect cursor = inputMethodCursorRect(canvas);
        pages.push_back(canvas.currentPageNumber());
        cursorXs.push_back(cursor.x());
        cursorYs.push_back(cursor.y());
    }

    std::optional<std::size_t> firstSecondPage;
    for (std::size_t index = 0; index + 1U < pages.size(); ++index) {
        if (pages[index] == 2 && pages[index + 1U] == 2) {
            firstSecondPage = index;
            break;
        }
    }
    check(firstSecondPage.has_value(),
          "multipage fixture did not leave adjacent markers on page two");
    check(std::abs(cursorXs[*firstSecondPage] -
                   cursorXs[*firstSecondPage + 1U]) <= 2,
          "first page-two line retained the page-one square-wrap offset");

    const int lastMarkerPage = pages.back();
    const int lastMarkerY = cursorYs.back();
    check(canvas.findNext(QStringLiteral("FOLLOW"), true),
          "could not locate paragraph after multipage anchor fixture");
    const QRect followingCursor = inputMethodCursorRect(canvas);
    if (!(canvas.currentPageNumber() == lastMarkerPage &&
          followingCursor.y() > lastMarkerY &&
          followingCursor.y() - lastMarkerY < 50)) {
        std::cerr << "multipage geometry: lastPage=" << lastMarkerPage
                  << " followingPage=" << canvas.currentPageNumber()
                  << " lastY=" << lastMarkerY
                  << " followingY=" << followingCursor.y() << '\n';
    }
    check(canvas.currentPageNumber() == lastMarkerPage &&
              followingCursor.y() > lastMarkerY &&
              followingCursor.y() - lastMarkerY < 50,
          "a prior-page picture bottom displaced the following paragraph on the current page");
    canvas.hide();
}

void testResizeHandleDragIsOneUndo(
    docxstudio::app::SpellChecker& spelling) {
    enum class ResizeExpectation {
        width_only,
        height_only,
        locked_corner,
        free_corner,
    };
    const auto png = encodedPng(Qt::red, 80, 48);
    const auto exerciseHandle =
        [&spelling, &png](const auto& handlePoint, const QPoint& delta,
                          Qt::CursorShape cursor,
                          ResizeExpectation expectation,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            DocumentCanvas canvas(spelling);
            canvas.resize(900, 520);
            canvas.show();
            canvas.setFocus();
            check(canvas.insertInlineImage(
                      png, QStringLiteral("resize handle fixture")),
                  "could not insert resize-handle fixture");
            const auto imageId = *canvas.selectedInlineImageId();
            const auto original = imageAtoms(canvas).front();

            // Capture the undecorated raster bounds, then reselect the image
            // so the tested point is a real painted and hit-tested handle.
            sendKey(canvas, Qt::Key_Right);
            QApplication::processEvents();
            const QRect imageBounds =
                redBounds(canvas.viewport()->grab().toImage());
            check(imageBounds.isValid(),
                  "could not locate rendered resize-handle fixture");
            check(canvas.selectInlineImage(imageId),
                  "could not reselect resize-handle fixture");
            QApplication::processEvents();

            const QPoint start = handlePoint(imageBounds);
            moveViewport(canvas, start);
            check(canvas.viewport()->cursor().shape() == cursor,
                  "exposed picture handle did not advertise its resize cursor");

            int documentChangeSignals = 0;
            QObject::connect(
                &canvas, &DocumentCanvas::documentChanged,
                [&documentChangeSignals](qulonglong) {
                    ++documentChangeSignals;
                });
            const auto beforeDrag = canvas.snapshot();
            dragViewport(canvas, start, start + delta, modifiers);
            QApplication::processEvents();
            const auto afterDrag = canvas.snapshot();
            const auto resized = imageAtoms(canvas).front();
            check(afterDrag.revision.value() ==
                          beforeDrag.revision.value() + 1 &&
                      documentChangeSignals == 1,
                  "one handle drag was not exactly one semantic transaction");

            switch (expectation) {
                case ResizeExpectation::width_only:
                    check(resized.width_emu > original.width_emu &&
                              resized.height_emu == original.height_emu,
                          "right handle changed more than picture width");
                    break;
                case ResizeExpectation::height_only:
                    check(resized.width_emu == original.width_emu &&
                              resized.height_emu > original.height_emu,
                          "bottom handle changed more than picture height");
                    break;
                case ResizeExpectation::locked_corner:
                case ResizeExpectation::free_corner: {
                    check(resized.width_emu > original.width_emu &&
                              resized.height_emu > original.height_emu,
                          "bottom-right handle did not change both dimensions");
                    const double originalRatio =
                        static_cast<double>(original.width_emu) /
                        static_cast<double>(original.height_emu);
                    const double resizedRatio =
                        static_cast<double>(resized.width_emu) /
                        static_cast<double>(resized.height_emu);
                    if (expectation == ResizeExpectation::locked_corner) {
                        check(std::abs(originalRatio - resizedRatio) < 0.002,
                              "bottom-right handle did not preserve aspect ratio by default");
                    } else {
                        check(std::abs(originalRatio - resizedRatio) > 0.05,
                              "Shift did not allow a free-aspect corner resize");
                    }
                    break;
                }
            }

            canvas.undo();
            const auto undone = imageAtoms(canvas).front();
            check(undone.width_emu == original.width_emu &&
                      undone.height_emu == original.height_emu,
                  "one Undo did not restore pre-drag picture geometry");
            canvas.redo();
            const auto redone = imageAtoms(canvas).front();
            check(redone.width_emu == resized.width_emu &&
                      redone.height_emu == resized.height_emu,
                  "Redo did not restore handle-drag picture geometry");
            canvas.hide();
        };

    exerciseHandle(
        [](const QRect& bounds) {
            return QPoint(bounds.right() + 1, bounds.center().y());
        },
        QPoint(32, 0), Qt::SizeHorCursor,
        ResizeExpectation::width_only);
    exerciseHandle(
        [](const QRect& bounds) {
            return QPoint(bounds.center().x(), bounds.bottom() + 1);
        },
        QPoint(0, 20), Qt::SizeVerCursor,
        ResizeExpectation::height_only);
    exerciseHandle(
        [](const QRect& bounds) {
            return bounds.bottomRight() + QPoint(1, 1);
        },
        QPoint(32, 19), Qt::SizeFDiagCursor,
        ResizeExpectation::locked_corner);
    exerciseHandle(
        [](const QRect& bounds) {
            return bounds.bottomRight() + QPoint(1, 1);
        },
        QPoint(36, 4), Qt::SizeFDiagCursor,
        ResizeExpectation::free_corner, Qt::ShiftModifier);

    DocumentCanvas cancellation(spelling);
    cancellation.resize(900, 520);
    cancellation.show();
    cancellation.setFocus();
    check(cancellation.insertInlineImage(
              png, QStringLiteral("resize cancellation fixture")),
          "could not insert resize-cancellation fixture");
    const auto imageId = *cancellation.selectedInlineImageId();
    const auto original = imageAtoms(cancellation).front();
    sendKey(cancellation, Qt::Key_Right);
    QApplication::processEvents();
    const QRect imageBounds =
        redBounds(cancellation.viewport()->grab().toImage());
    check(imageBounds.isValid(),
          "could not locate resize-cancellation fixture");
    check(cancellation.selectInlineImage(imageId),
          "could not reselect resize-cancellation fixture");
    QApplication::processEvents();
    const QRect selectedImageBounds =
        redBounds(cancellation.viewport()->grab().toImage());
    check(selectedImageBounds.isValid(),
          "could not locate selected resize-cancellation fixture");

    const auto isResizeCursor = [](Qt::CursorShape cursor) {
        return cursor == Qt::SizeHorCursor ||
               cursor == Qt::SizeVerCursor ||
               cursor == Qt::SizeFDiagCursor ||
               cursor == Qt::SizeBDiagCursor;
    };
    const std::array<QPoint, 5> removedHandlePoints{{
        imageBounds.topLeft(),
        QPoint(imageBounds.center().x(), imageBounds.top()),
        QPoint(imageBounds.right() + 1, imageBounds.top()),
        QPoint(imageBounds.left(), imageBounds.center().y()),
        QPoint(imageBounds.left(), imageBounds.bottom() + 1),
    }};
    for (const QPoint& point : removedHandlePoints) {
        moveViewport(cancellation, point);
        check(!isResizeCursor(cancellation.viewport()->cursor().shape()),
              "an uncommittable top/left picture handle remained hit-testable");
    }

    int documentChangeSignals = 0;
    int operationFailureSignals = 0;
    QObject::connect(
        &cancellation, &DocumentCanvas::documentChanged,
        [&documentChangeSignals](qulonglong) { ++documentChangeSignals; });
    QObject::connect(
        &cancellation, &DocumentCanvas::operationFailed,
        [&operationFailureSignals](const QString&) {
            ++operationFailureSignals;
        });
    const auto beforeUnsupportedDrag = cancellation.snapshot();
    dragViewport(cancellation, imageBounds.topLeft(),
                 imageBounds.topLeft() - QPoint(24, 16));
    check(cancellation.snapshot().revision ==
                  beforeUnsupportedDrag.revision &&
              imageAtoms(cancellation).front().width_emu ==
                  original.width_emu &&
              imageAtoms(cancellation).front().height_emu ==
                  original.height_emu,
          "dragging a removed top/left hotspot changed picture geometry");

    const QPoint bottomRight = imageBounds.bottomRight() + QPoint(1, 1);
    const QPoint previewEnd = bottomRight + QPoint(40, 24);
    const auto beforeCancel = cancellation.snapshot();
    pressViewport(cancellation, bottomRight);
    moveViewport(cancellation, previewEnd, Qt::LeftButton);
    QApplication::processEvents();
    const QRect previewBounds =
        redBounds(cancellation.viewport()->grab().toImage());
    check(previewBounds.isValid() &&
              previewBounds.width() > imageBounds.width() &&
              previewBounds.topLeft() == selectedImageBounds.topLeft() &&
              cancellation.snapshot().revision == beforeCancel.revision &&
              documentChangeSignals == 0,
          "resize preview moved its fixed origin or mutated the document before release");
    sendKey(cancellation, Qt::Key_Escape);
    QApplication::processEvents();
    const QRect cancelledBounds =
        redBounds(cancellation.viewport()->grab().toImage());
    releaseViewport(cancellation, previewEnd);
    check(cancelledBounds.isValid() &&
              cancelledBounds.width() < previewBounds.width() &&
              cancellation.viewport()->cursor().shape() == Qt::ArrowCursor &&
              cancellation.snapshot().revision == beforeCancel.revision &&
              documentChangeSignals == 0 &&
              imageAtoms(cancellation).front().width_emu ==
                  original.width_emu &&
              imageAtoms(cancellation).front().height_emu ==
                  original.height_emu,
          "Escape did not cancel resize preview without a transaction");

    const auto beforeNoOp = cancellation.snapshot();
    dragViewport(cancellation, bottomRight, bottomRight);
    constexpr double kEmuPerPoint = 12700.0;
    const double originalWidthPoints =
        static_cast<double>(original.width_emu) / kEmuPerPoint;
    const double originalHeightPoints =
        static_cast<double>(original.height_emu) / kEmuPerPoint;
    check(cancellation.resizeSelectedInlineImage(
              originalWidthPoints, originalHeightPoints) &&
              cancellation.snapshot().revision == beforeNoOp.revision &&
              documentChangeSignals == 0,
          "no-op handle/API resize created a semantic transaction");

    const auto beforeFailure = cancellation.snapshot();
    check(!cancellation.resizeSelectedInlineImage(
              0.0, originalHeightPoints) &&
              cancellation.snapshot().revision == beforeFailure.revision &&
              documentChangeSignals == 0 &&
              operationFailureSignals == 1,
          "invalid picture resize did not fail without changing the document");
    cancellation.hide();
}

void testClipboardValidation(docxstudio::app::SpellChecker& spelling) {
    const auto png = encodedPng(Qt::red);
    auto* rawPng = new QMimeData;
    rawPng->setData(QStringLiteral("image/png"), asByteArray(png));
    QApplication::clipboard()->setMimeData(rawPng);
    DocumentCanvas rawPngCanvas(spelling);
    rawPngCanvas.paste();
    const auto rawPngAtoms = imageAtoms(rawPngCanvas);
    check(rawPngAtoms.size() == 1 &&
              rawPngAtoms.front().format ==
                  docxstudio::core::ImageFormat::png &&
              rawPngAtoms.front().encoded_payload.bytes().size() == png.size(),
          "raw image/png clipboard paste did not preserve PNG bytes");

    DocumentCanvas jpegCanvas(spelling);
    const auto jpeg = encodedJpeg(Qt::blue);
    check(jpegCanvas.insertInlineImage(jpeg, QStringLiteral("blue.jpg")),
          "canvas rejected a valid JPEG picture");
    jpegCanvas.copy();
    const QMimeData* copied = QApplication::clipboard()->mimeData();
    check(copied &&
              copied->data(QStringLiteral("image/jpeg")) == asByteArray(jpeg),
          "copy did not preserve the original raw JPEG bytes");
    jpegCanvas.cut();
    jpegCanvas.paste();
    const auto jpegAtoms = imageAtoms(jpegCanvas);
    check(jpegAtoms.size() == 1 &&
              jpegAtoms.front().format ==
                  docxstudio::core::ImageFormat::jpeg &&
              jpegAtoms.front().encoded_payload.bytes().size() == jpeg.size(),
          "raw JPEG/native clipboard round-trip changed the image format");

    auto* rawJpeg = new QMimeData;
    rawJpeg->setData(QStringLiteral("image/jpeg"), asByteArray(jpeg));
    QApplication::clipboard()->setMimeData(rawJpeg);
    DocumentCanvas rawJpegCanvas(spelling);
    rawJpegCanvas.paste();
    const auto rawJpegAtoms = imageAtoms(rawJpegCanvas);
    check(rawJpegAtoms.size() == 1 &&
              rawJpegAtoms.front().format ==
                  docxstudio::core::ImageFormat::jpeg &&
              rawJpegAtoms.front().encoded_payload.bytes().size() == jpeg.size(),
          "raw image/jpeg clipboard paste did not preserve JPEG bytes");

    DocumentCanvas rejected(spelling);
    const auto unchanged = rejected.snapshot();
    auto* malformed = new QMimeData;
    malformed->setData(
        QStringLiteral("application/x-owl-docs-inline-image-v1"),
        QByteArrayLiteral("not-an-owl-image"));
    QApplication::clipboard()->setMimeData(malformed);
    rejected.paste();
    auto afterRejected = rejected.snapshot();
    check(afterRejected.revision == unchanged.revision &&
              afterRejected.document == unchanged.document,
          "malformed native clipboard data mutated the document");

    auto* oversized = new QMimeData;
    oversized->setData(
        QStringLiteral("image/png"),
        QByteArray(static_cast<qsizetype>(
                       docxstudio::core::kMaximumEncodedImageBytes + 1U),
                   '\0'));
    QApplication::clipboard()->setMimeData(oversized);
    rejected.paste();
    afterRejected = rejected.snapshot();
    check(afterRejected.revision == unchanged.revision &&
              afterRejected.document == unchanged.document,
          "oversize raw clipboard data mutated the document");

    auto* overDimension = new QMimeData;
    QImage tooWide(16'385, 1, QImage::Format_ARGB32);
    tooWide.fill(Qt::green);
    overDimension->setImageData(tooWide);
    QApplication::clipboard()->setMimeData(overDimension);
    rejected.paste();
    afterRejected = rejected.snapshot();
    check(afterRejected.revision == unchanged.revision &&
              afterRejected.document == unchanged.document,
          "over-dimensional decoded clipboard image mutated the document");
}

void testHeaderFooterImagePasteZoomHistoryAndRecovery(
    docxstudio::app::SpellChecker& spelling) {
    using namespace docxstudio;
    DocumentCanvas canvas(spelling);
    canvas.resize(1000, 760);
    canvas.show();
    canvas.beginHeaderFooterEditing(false, 1, 0);
    QApplication::processEvents();
    auto* center = canvas.findChild<QPlainTextEdit*>(
        QStringLiteral("headerStoryEditor1"));
    check(center && center->isVisible(),
          "header center editor was not available for picture paste");
    const double font100 = center->font().pointSizeF();
    const QRect geometry100 = center->geometry();
    canvas.setZoomPercent(200);
    QApplication::processEvents();
    check(std::abs(center->font().pointSizeF() - font100 * 2.0) < 0.05 &&
              center->geometry().width() > geometry100.width() * 1.8 &&
              center->geometry().height() > geometry100.height() * 1.8,
          "header/footer editor font and geometry did not scale with zoom");

    auto* mime = new QMimeData;
    QImage clipboardImage(80, 40, QImage::Format_ARGB32_Premultiplied);
    clipboardImage.fill(QColor(12, 170, 73));
    mime->setImageData(clipboardImage);
    QApplication::clipboard()->setMimeData(mime);
    center->setFocus();
    canvas.paste();
    QApplication::processEvents();
    auto snapshot = canvas.snapshot();
    check(snapshot.document.headerImages().size() == 1 &&
              snapshot.document.footerImages().empty() &&
              snapshot.document.headerText().size() == 3 &&
              snapshot.document.headerText()[0] == u'\t' &&
              snapshot.document.headerText()[1] ==
                  core::kInlineObjectReplacementCharacter &&
              snapshot.document.headerText()[2] == u'\t',
          "pasting into the center header did not create a semantic story image");
    const auto& image = snapshot.document.headerImages().front();
    check(std::abs(static_cast<double>(image.width_emu) /
                       static_cast<double>(image.height_emu) -
                   2.0) < 0.001,
          "header picture paste did not preserve the source aspect ratio");
    canvas.undo();
    check(canvas.snapshot().document.headerImages().empty(),
          "Undo did not remove a pasted header picture");
    canvas.redo();
    snapshot = canvas.snapshot();
    check(snapshot.document.headerImages().size() == 1,
          "Redo did not restore a pasted header picture");

    std::string error;
    const auto encoded = app::RecoveryCodec::encode(
        {snapshot.document, {}}, error);
    check(encoded.has_value(),
          "recovery could not encode a header picture");
    const auto recovered = app::RecoveryCodec::decode(*encoded, error);
    check(recovered && recovered->document.headerImages().size() == 1 &&
              recovered->document.headerText() ==
                  snapshot.document.headerText() &&
              recovered->document.headerImages().front().encoded_payload ==
                  snapshot.document.headerImages().front().encoded_payload &&
              recovered->document.headerImages().front().width_emu ==
                  snapshot.document.headerImages().front().width_emu &&
              recovered->document.headerImages().front().height_emu ==
                  snapshot.document.headerImages().front().height_emu,
          "header picture did not survive recovery round-trip");

    const auto pasteIntoStoryRegion = [&](bool footer, int region) {
        canvas.beginHeaderFooterEditing(footer, region, 0);
        QApplication::processEvents();
        auto* target = canvas.findChild<QPlainTextEdit*>(
            (footer ? QStringLiteral("footerStoryEditor")
                    : QStringLiteral("headerStoryEditor")) +
            QString::number(region));
        check(target && target->isVisible(),
              "requested header/footer picture region was unavailable");
        target->setFocus();
        target->moveCursor(QTextCursor::End);
        target->paste();
        QApplication::processEvents();
    };
    pasteIntoStoryRegion(false, 0);
    pasteIntoStoryRegion(false, 2);
    pasteIntoStoryRegion(true, 0);
    pasteIntoStoryRegion(true, 1);
    pasteIntoStoryRegion(true, 2);
    snapshot = canvas.snapshot();
    check(snapshot.document.headerImages().size() == 3 &&
              snapshot.document.footerImages().size() == 3 &&
              snapshot.document.headerText() == u"\ufffc\t\ufffc\t\ufffc" &&
              snapshot.document.footerText() == u"\ufffc\t\ufffc\t\ufffc",
          "picture paste did not work in every header/footer alignment region");
    canvas.hide();
}

void testMixedObjectOrdering(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.insertText(QStringLiteral("AC"));
    sendKey(canvas, Qt::Key_Left);
    const auto firstPng = encodedPng(Qt::red, 8, 8);
    const auto secondPng = encodedPng(Qt::green, 9, 7);
    check(canvas.insertInlineImage(firstPng, QStringLiteral("first")),
          "could not insert first adjacent image");
    sendKey(canvas, Qt::Key_Right);
    check(canvas.insertInlineImage(secondPng, QStringLiteral("second")),
          "could not insert second adjacent image");
    sendKey(canvas, Qt::Key_Right);
    check(canvas.insertEquation(QStringLiteral("x^2")),
          "could not insert equation after adjacent images");

    const auto snapshot = canvas.snapshot();
    const auto& paragraph = snapshot.document.paragraphs().front();
    check(paragraph.text() == u"A\ufffc\ufffc\ufffcC" &&
              paragraph.images().size() == 2 &&
              paragraph.images()[0].utf16_offset == 1 &&
              paragraph.images()[1].utf16_offset == 2 &&
              paragraph.equations().size() == 1 &&
              paragraph.equations().front().utf16_offset == 3,
          "adjacent images and equation did not retain semantic run order");
}

void testInlineObjectTypingOverrideProvenance(
    docxstudio::app::SpellChecker& spelling) {
    using namespace docxstudio::core;
    CharacterFormatDelta exactDefaults;
    exactDefaults.foreground_argb =
        PropertyDelta<std::uint32_t>::set(0xff000000U);
    exactDefaults.bold = PropertyDelta<bool>::set(false);
    exactDefaults.highlight_argb =
        PropertyDelta<std::uint32_t>::clear();

    {
        DocumentCanvas equation(spelling);
        equation.insertText(QStringLiteral("A"));
        const auto before = equation.snapshot();
        equation.applyCharacterFormat(exactDefaults);
        check(equation.snapshot().revision == before.revision &&
                  !equation.snapshot().document.paragraphs().front()
                       .styleProvenance(),
              "collapsed equation formatting created an invisible edit");
        int changeSignals = 0;
        const auto connection = QObject::connect(
            &equation, &DocumentCanvas::documentChanged,
            [&](qulonglong) { ++changeSignals; });
        check(equation.insertEquation(QStringLiteral("x^2")),
              "could not insert provenance-aware equation");
        QObject::disconnect(connection);

        auto snapshot = equation.snapshot();
        const auto& paragraph = snapshot.document.paragraphs().front();
        const auto mask = paragraph.styleOverrideMaskAt(2);
        const auto format = paragraph.characterFormatAt(2);
        check(snapshot.revision.value() == before.revision.value() + 1 &&
                  changeSignals == 1 && paragraph.text() == u"A\ufffc" &&
                  paragraph.equations().size() == 1 &&
                  paragraph.styleId() ==
                      std::optional<std::string>{"Normal"} &&
                  paragraph.styleProvenance() &&
                  format.foreground_argb == 0xff000000U &&
                  format.bold == false && !format.highlight_argb &&
                  mask.foreground_argb && mask.bold &&
                  mask.highlight_argb,
              "equation insertion lost atomic equal-value provenance");
        equation.undo();
        snapshot = equation.snapshot();
        check(snapshot.document == before.document &&
                  !snapshot.document.paragraphs().front().styleId() &&
                  !snapshot.document.paragraphs().front().styleProvenance(),
              "one Undo did not remove the equation and lazy provenance");
        equation.redo();
        equation.applyParagraphStyle(QStringLiteral("Heading1"));
        equation.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = equation.snapshot();
        const auto& transitioned = snapshot.document.paragraphs().front();
        check(transitioned.characterFormatAt(2).foreground_argb ==
                  0xff000000U &&
                  transitioned.characterFormatAt(2).bold == false &&
                  transitioned.styleOverrideMaskAt(2).foreground_argb &&
                  transitioned.styleOverrideMaskAt(2).bold &&
                  transitioned.styleOverrideMaskAt(2).highlight_argb,
              "equation direct intent did not survive style transitions");
    }

    {
        DocumentCanvas image(spelling);
        image.insertText(QStringLiteral("A"));
        image.applyParagraphStyle(QStringLiteral("Heading1"));
        image.applyParagraphStyle(QStringLiteral("Normal"));
        const auto before = image.snapshot();
        check(static_cast<bool>(
                  before.document.paragraphs().front().styleProvenance()),
              "image fixture did not establish Normal provenance");
        image.applyCharacterFormat(exactDefaults);
        check(image.snapshot().revision == before.revision,
              "collapsed image formatting created an invisible edit");
        int changeSignals = 0;
        const auto connection = QObject::connect(
            &image, &DocumentCanvas::documentChanged,
            [&](qulonglong) { ++changeSignals; });
        check(image.insertInlineImage(encodedPng(Qt::magenta, 8, 8),
                                      QStringLiteral("override fixture")),
              "could not insert provenance-aware image");
        QObject::disconnect(connection);

        auto snapshot = image.snapshot();
        const auto& paragraph = snapshot.document.paragraphs().front();
        const auto mask = paragraph.styleOverrideMaskAt(2);
        const auto format = paragraph.characterFormatAt(2);
        check(snapshot.revision.value() == before.revision.value() + 1 &&
                  changeSignals == 1 && paragraph.text() == u"A\ufffc" &&
                  paragraph.images().size() == 1 &&
                  format.foreground_argb == 0xff000000U &&
                  format.bold == false && !format.highlight_argb &&
                  mask.foreground_argb && mask.bold &&
                  mask.highlight_argb,
              "image insertion lost atomic equal-value provenance");
        image.undo();
        check(image.snapshot().document == before.document,
              "one Undo did not remove the image formatting transaction");
        image.redo();
        image.applyParagraphStyle(QStringLiteral("Heading1"));
        image.applyParagraphStyle(QStringLiteral("Heading2"));
        snapshot = image.snapshot();
        const auto& transitioned = snapshot.document.paragraphs().front();
        check(transitioned.characterFormatAt(2).foreground_argb ==
                  0xff000000U &&
                  transitioned.characterFormatAt(2).bold == false &&
                  transitioned.styleOverrideMaskAt(2).foreground_argb &&
                  transitioned.styleOverrideMaskAt(2).bold &&
                  transitioned.styleOverrideMaskAt(2).highlight_argb,
              "image direct intent did not survive style transitions");
    }
}

void testImageBudgetEvictionKeepsCanvasHistoryUsable(
    docxstudio::app::SpellChecker& spelling) {
    docxstudio::core::DocumentSessionLimits limits;
    limits.maximum_history_entries = 32;
    limits.maximum_retained_history_image_bytes = 0;
    DocumentCanvas canvas(spelling, limits);
    canvas.insertText(QStringLiteral("A"));
    const auto png = encodedPng(Qt::red, 8, 8);
    check(canvas.insertInlineImage(png, QStringLiteral("budget.png")),
          "could not insert image-budget fixture");
    check(canvas.deleteSelectedInlineImage() && imageAtoms(canvas).empty(),
          "could not delete image-budget fixture");

    // Once the image becomes inactive, the zero-byte retained-media budget
    // evicts the complete document-history prefix that owns it. The canvas
    // must discard its corresponding cursor entries instead of getting stuck
    // on a core history_empty result.
    canvas.undo();
    check(imageAtoms(canvas).empty() &&
              canvas.snapshot().document.paragraphs().front().text() == u"A",
          "canvas exposed history already evicted by the image budget");
    canvas.insertText(QStringLiteral("B"));
    canvas.undo();
    check(canvas.snapshot().document.paragraphs().front().text() == u"A",
          "image-budget eviction permanently blocked a later undo");
    canvas.redo();
    check(canvas.snapshot().document.paragraphs().front().text() == u"AB",
          "image-budget eviction permanently blocked a later redo");
}

void testDialogSaveAndReopen() {
    QTemporaryDir temporary;
    check(temporary.isValid(), "could not create image UI test directory");
    const QString source = temporary.filePath(QStringLiteral("source.png"));
    const auto png = encodedPng(Qt::red, 64, 32);
    QFile sourceFile(source);
    check(sourceFile.open(QIODevice::WriteOnly) &&
              sourceFile.write(
                  reinterpret_cast<const char*>(png.data()),
                  static_cast<qint64>(png.size())) ==
                  static_cast<qint64>(png.size()),
          "could not write source image fixture");
    sourceFile.close();

    docxstudio::app::MainWindow window;
    window.resize(1000, 720);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<DocumentCanvas*>();
    auto* insert = window.findChild<QAction*>(QStringLiteral("insert.image"));
    check(canvas && insert, "could not reach Picture command");
    canvas->insertText(QStringLiteral("BeforeAfter"));
    for (int index = 0; index < 5; ++index) sendKey(*canvas, Qt::Key_Left);

    const QString oversizedSource =
        temporary.filePath(QStringLiteral("oversized.png"));
    QFile oversizedFile(oversizedSource);
    check(oversizedFile.open(QIODevice::WriteOnly) &&
              oversizedFile.resize(static_cast<qint64>(
                  docxstudio::core::kMaximumEncodedImageBytes + 1U)),
          "could not create oversize file-read fixture");
    oversizedFile.close();
    bool selectedOversizedSource = false;
    bool closedOversizeWarning = false;
    QTimer warningCloser(&window);
    QObject::connect(&warningCloser, &QTimer::timeout, &window, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget);
                message && message->isVisible()) {
                closedOversizeWarning = true;
                message->accept();
            }
        }
    });
    warningCloser.start(1);
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "oversize Picture command did not open a file dialog");
        dialog->selectFile(oversizedSource);
        selectedOversizedSource = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept oversize picture dialog");
    });
    insert->trigger();
    warningCloser.stop();
    check(selectedOversizedSource && closedOversizeWarning &&
              imageAtoms(*canvas).empty(),
          "Picture command did not reject a file larger than 16 MiB before decode");

    bool selectedSource = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "Picture command did not open a file dialog");
        const QString filters = dialog->nameFilters().join(QLatin1Char(' '));
        check(filters.contains(QStringLiteral("*.png")) &&
                  filters.contains(QStringLiteral("*.jpg")) &&
                  filters.contains(QStringLiteral("*.jpeg")) &&
                  !filters.contains(QStringLiteral("*.bmp")) &&
                  !filters.contains(QStringLiteral("*.webp")),
              "Picture dialog did not restrict authoring to PNG/JPEG");
        dialog->selectFile(source);
        selectedSource = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept source picture dialog");
    });
    insert->trigger();
    const auto insertedFromFile = imageAtoms(*canvas);
    check(selectedSource && insertedFromFile.size() == 1 &&
              insertedFromFile.front().accessible_name == "source.png" &&
              insertedFromFile.front().accessible_name.find(
                  temporary.path().toStdString()) == std::string::npos,
          "Picture command did not insert a real inline image");
    const docxstudio::core::ImageLayout squareLayout{
        docxstudio::core::ImagePlacement::square,
        101, 202, 303, 404, false};
    check(canvas->setSelectedImageLayout(squareLayout) &&
              canvas->setSelectedImageAccessibleName(
                  QStringLiteral("Quarterly owl diagram")),
          "could not configure square wrapping and alt text before Save As");

    sendKey(*canvas, Qt::Key_Right);
    check(canvas->insertInlineImage(png, QStringLiteral("second.png")),
          "could not insert adjacent image before DOCX save");
    const docxstudio::core::ImageLayout topBottomLayout{
        docxstudio::core::ImagePlacement::top_and_bottom,
        505, 606, 707, 808, true};
    check(canvas->setSelectedImageLayout(topBottomLayout) &&
              canvas->setSelectedImageAccessibleName(
                  QStringLiteral("Supporting owl figure")),
          "could not configure top/bottom wrapping and alt text before Save As");
    sendKey(*canvas, Qt::Key_Right);
    check(canvas->insertEquation(QStringLiteral("x^2")),
          "could not insert equation after adjacent images before DOCX save");

    canvas->beginHeaderFooterEditing(false, 1, 0);
    QApplication::processEvents();
    auto* headerCenter = canvas->findChild<QPlainTextEdit*>(
        QStringLiteral("headerStoryEditor1"));
    check(headerCenter && headerCenter->isVisible(),
          "could not reach the center header before DOCX save");
    headerCenter->setFocus();
    bool selectedHeaderSource = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr,
              "header Picture command did not open a file dialog");
        dialog->selectFile(source);
        selectedHeaderSource = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept header picture dialog");
    });
    insert->trigger();
    canvas->endHeaderFooterEditing();
    check(selectedHeaderSource &&
              canvas->snapshot().document.headerImages().size() == 1,
          "Picture command did not insert into the active header region");

    const QString saved = temporary.filePath(QStringLiteral("image.docx"));
    auto* saveAs = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr, "could not reach Save As");
    bool selectedDestination = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "Save As did not open a file dialog");
        dialog->selectFile(saved);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept image DOCX destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(saved) &&
              !canvas->isModified(),
          "image document was not saved as a clean DOCX");

    docxstudio::ooxml::Error openError;
    auto package = docxstudio::ooxml::DocxDocument::open(
        std::filesystem::path(QFile::encodeName(saved).constData()),
        &openError);
    check(package && package->paragraphs().size() == 1 &&
              package->headerImages().size() == 1 &&
              package->headerImages().front().image.bytes == png &&
              package->headerImages().front().image.width_emu > 0 &&
              package->headerImages().front().image.height_emu > 0,
          "saved image DOCX did not reopen in the OOXML engine");
    bool foundImage = false;
    int imageCount = 0;
    std::string ordered;
    for (const auto& run : package->paragraphs().front().runs) {
        for (const auto& fragment : run.fragments) {
            if (fragment.kind ==
                    docxstudio::ooxml::FragmentKind::inline_image &&
                fragment.inline_image) {
                foundImage = true;
                ++imageCount;
                ordered += "<image>";
                check(fragment.inline_image->bytes.size() == png.size() &&
                          fragment.inline_image->width_emu > 0 &&
                          fragment.inline_image->height_emu > 0,
                      "native DrawingML lost picture bytes or extents");
                const auto& importedImage = *fragment.inline_image;
                if (imageCount == 1) {
                    check(importedImage.accessible_name ==
                                  "Quarterly owl diagram" &&
                              importedImage.layout ==
                                  docxstudio::ooxml::ImageLayout{
                                      docxstudio::ooxml::ImagePlacement::square,
                                      101, 202, 303, 404, false},
                          "native DrawingML lost square wrapping, distances, fixed placement, or alt text");
                } else if (imageCount == 2) {
                    check(importedImage.accessible_name ==
                                  "Supporting owl figure" &&
                              importedImage.layout ==
                                  docxstudio::ooxml::ImageLayout{
                                      docxstudio::ooxml::ImagePlacement::top_and_bottom,
                                      505, 606, 707, 808, true},
                          "native DrawingML lost top/bottom wrapping, distances, moving placement, or alt text");
                }
            } else if (fragment.kind ==
                       docxstudio::ooxml::FragmentKind::text) {
                ordered += fragment.text;
            } else if (fragment.kind ==
                           docxstudio::ooxml::FragmentKind::equation &&
                       fragment.equation) {
                ordered += "<equation>";
            }
        }
    }
    check(foundImage && imageCount == 2 &&
              ordered == "Before<image><image><equation>After",
          "saved DrawingML/OMML did not preserve mixed inline run order");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(saved),
          "desktop shell could not reopen its authored picture DOCX");
    DocumentCanvas* reopenedCanvas = nullptr;
    for (auto* candidate : reopened.findChildren<DocumentCanvas*>()) {
        if (!imageAtoms(*candidate).empty()) {
            reopenedCanvas = candidate;
            break;
        }
    }
    const auto reopenedImages = reopenedCanvas
        ? imageAtoms(*reopenedCanvas)
        : std::vector<docxstudio::core::ImageAtom>{};
    check(reopenedCanvas != nullptr,
          "desktop reopen did not provide a populated document canvas");
    const auto reopenedSnapshot = reopenedCanvas->snapshot();
    const auto reopenedHeaderBytes =
        reopenedSnapshot.document.headerImages().empty()
        ? std::span<const std::uint8_t>{}
        : reopenedSnapshot.document.headerImages().front()
              .encoded_payload.bytes();
    check(reopenedImages.size() == 2 &&
              reopenedImages.front().encoded_payload.size() == png.size() &&
              reopenedImages[0].accessible_name ==
                  "Quarterly owl diagram" &&
              reopenedImages[0].layout == squareLayout &&
              reopenedImages[1].accessible_name ==
                  "Supporting owl figure" &&
              reopenedImages[1].layout == topBottomLayout &&
              reopenedSnapshot.document.headerImages().size() == 1 &&
              std::equal(reopenedHeaderBytes.begin(), reopenedHeaderBytes.end(),
                         png.begin(), png.end()) &&
              reopenedSnapshot.document.paragraphs().front()
                      .equations().size() == 1 &&
              !reopenedCanvas->isModified(),
          "desktop reopen did not reconstruct mixed semantic objects, story pictures, picture layout, or alt text");
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir applicationData;
    check(applicationData.isValid(),
          "could not isolate image UI application data");
    qputenv("XDG_DATA_HOME", applicationData.path().toUtf8());
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    docxstudio::app::SpellChecker spelling;
    testCanvasObjectLifecycle(spelling);
    testPicturePropertiesAndClipboardVersions(spelling);
    testCanonicalAnchorOriginsAndSharedPdfGeometry(spelling);
    testMixedAnchorNonOverlapAndTopmostHitTarget(spelling);
    testAnchoredMultipageFlowIsPageLocal(spelling);
    testResizeHandleDragIsOneUndo(spelling);
    testClipboardValidation(spelling);
    testHeaderFooterImagePasteZoomHistoryAndRecovery(spelling);
    testMixedObjectOrdering(spelling);
    testInlineObjectTypingOverrideProvenance(spelling);
    testImageBudgetEvictionKeepsCanvasHistoryUsable(spelling);
    testDialogSaveAndReopen();
    std::cout << "image UI tests passed\n";
    return 0;
}
