#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <cstdlib>
#include <iostream>
#include <string>

namespace app = docxstudio::app;
namespace core = docxstudio::core;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::vector<std::uint8_t> png(const QColor& color) {
    QImage image(96, 48, QImage::Format_RGB32); image.fill(color);
    QByteArray bytes; QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "image fixture encoding failed");
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size()};
}
int coloredPixels(app::DocumentCanvas& canvas, const QColor& color) {
    QApplication::processEvents();
    QImage raster(canvas.viewport()->size(), QImage::Format_RGB32); raster.fill(Qt::white);
    { QPainter painter(&raster); canvas.viewport()->render(&painter); }
    int result = 0;
    for (int y = 0; y < raster.height(); ++y) for (int x = 0; x < raster.width(); ++x) {
        const auto pixel = raster.pixelColor(x, y);
        if (std::abs(pixel.red() - color.red()) < 3 && std::abs(pixel.green() - color.green()) < 3 &&
            std::abs(pixel.blue() - color.blue()) < 3) ++result;
    }
    return result;
}
void replacementPaint(app::SpellChecker& spelling, const std::string& context) {
    std::cout << "Image payload paint context: " << context << std::endl;
    const QColor original(QStringLiteral("#e125b1")), replacement(QStringLiteral("#1bc7a9"));
    const auto oldBytes = png(original), newBytes = png(replacement);
    app::DocumentCanvas canvas(spelling);
    canvas.resize(820, 900); canvas.setZoomPercent(75);
    core::NodeId imageId;
    if (context == "body") {
        check(canvas.insertInlineImage(oldBytes, QStringLiteral("Original body image")), "body image insertion failed");
        imageId = canvas.snapshot().document.paragraphs().front().images().front().id;
    } else if (context == "cell") {
        check(canvas.insertTable(1, 1, false), "cell image table fixture failed");
        const auto tableId = canvas.snapshot().document.tables().front().id();
        check(canvas.activateTableCell(tableId, 0, 0) && canvas.insertInlineImage(oldBytes, QStringLiteral("Original cell image")), "cell image insertion failed");
        imageId = canvas.snapshot().document.findTable(tableId)->cell(0, 0)->images.front().id;
    } else {
        core::Document document; imageId = core::NodeId::generate();
        check(bool(document.insertHeaderFooterImage(context == "footer", 0,
            core::EncodedImagePayload(oldBytes), core::ImageFormat::png, "Original story image", 48 * 12700, 16 * 12700, imageId)), "story image insertion failed");
        canvas.setDocument(std::move(document));
    }
    canvas.show(); canvas.markSaved();
    const auto initial = canvas.snapshot();
    const auto* image = initial.document.findImage(imageId);
    check(image && coloredPixels(canvas, original) > 100, "original image did not paint and warm the decoded cache");
    const std::vector<core::Operation> operations{core::ReplaceImagePayload{imageId,
        core::EncodedImagePayload(newBytes), core::ImageFormat::png, image->width_emu, image->height_emu}};
    QString summary, error;
    const auto preview = [&] {
        const bool created = canvas.createOperationsPreview(canvas.snapshot().revision, operations,
            QStringLiteral("Replace image bytes"), summary, error);
        if (!created) std::cerr << error.toStdString() << '\n';
        check(created, "same-identity image payload preview failed");
        check(canvas.snapshot().document == initial.document && !canvas.isModified(), "payload preview mutated live document or dirty state");
        check(coloredPixels(canvas, replacement) > 100 && coloredPixels(canvas, original) == 0,
            "same-identity payload preview painted the cached old PNG");
    };
    preview(); canvas.discardPreview();
    check(canvas.snapshot().document == initial.document && coloredPixels(canvas, original) > 100 &&
        coloredPixels(canvas, replacement) == 0, "discard did not restore original image pixels");
    preview(); check(canvas.acceptPreview(error), "image payload preview acceptance failed");
    check(canvas.snapshot().document.findImage(imageId)->encoded_payload == core::EncodedImagePayload(newBytes) &&
        coloredPixels(canvas, replacement) > 100 && coloredPixels(canvas, original) == 0, "accepted image payload painted stale pixels");
    canvas.undo();
    check(canvas.snapshot().document == initial.document && coloredPixels(canvas, original) > 100 &&
        coloredPixels(canvas, replacement) == 0, "one Undo did not restore source payload and pixels");
    canvas.redo();
    check(coloredPixels(canvas, replacement) > 100 && coloredPixels(canvas, original) == 0, "Redo did not repaint replacement image pixels");
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv); app::SpellChecker spelling;
    for (const std::string context : {"body", "cell", "header", "footer"})
        if (argc == 1 || context == argv[1]) replacementPaint(spelling, context);
    std::cout << "Image payload preview paint tests passed\n";
}
