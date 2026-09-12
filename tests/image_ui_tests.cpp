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
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

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

    QTemporaryDir pdfOutput;
    check(pdfOutput.isValid(), "could not create picture PDF test directory");
    const QString pdfPath =
        pdfOutput.filePath(QStringLiteral("inline-picture.pdf"));
    QString pdfError;
    check(canvas.exportPdf(pdfPath, pdfError) &&
              QFileInfo(pdfPath).size() > 500,
          "shared page renderer did not export the inline picture PDF");
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
    check(!renderedPdf.isNull() && redBounds(renderedPdf).isValid(),
          "the exported PDF omitted or recolored the inline picture");
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

    sendKey(*canvas, Qt::Key_Right);
    check(canvas->insertInlineImage(png, QStringLiteral("second.png")),
          "could not insert adjacent image before DOCX save");
    sendKey(*canvas, Qt::Key_Right);
    check(canvas->insertEquation(QStringLiteral("x^2")),
          "could not insert equation after adjacent images before DOCX save");

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
    check(package && package->paragraphs().size() == 1,
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
    check(reopenedCanvas && reopenedImages.size() == 2 &&
              reopenedImages.front().encoded_payload.size() == png.size() &&
              reopenedCanvas->snapshot().document.paragraphs().front()
                      .equations().size() == 1 &&
              !reopenedCanvas->isModified(),
          "desktop reopen did not reconstruct mixed semantic inline objects");
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
    testClipboardValidation(spelling);
    testMixedObjectOrdering(spelling);
    testImageBudgetEvictionKeepsCanvasHistoryUsable(spelling);
    testDialogSaveAndReopen();
    std::cout << "image UI tests passed\n";
    return 0;
}
