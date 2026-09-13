#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using docxstudio::app::DocumentCanvas;
namespace core = docxstudio::core;

constexpr double kEmuPerPoint = 12700.0;
constexpr double kEmuPerInch = 72.0 * kEmuPerPoint;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> encodedPng() {
    QImage image(37, 23, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#e95420")));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "could not encode picture-dialog fixture");
    return {
        reinterpret_cast<const std::uint8_t*>(encoded.constData()),
        reinterpret_cast<const std::uint8_t*>(encoded.constData()) +
            encoded.size()};
}

core::ImageAtom selectedImage(const DocumentCanvas& canvas) {
    const auto selectedId = canvas.selectedInlineImageId();
    check(selectedId.has_value(), "picture-dialog fixture is not selected");
    const auto snapshot = canvas.snapshot();
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        for (const auto& image : paragraph.images()) {
            if (image.id == *selectedId) return image;
        }
    }
    check(false, "selected picture-dialog image is missing");
    return {};
}

void testSizeDialogPreservesExactEmus(DocumentCanvas& canvas) {
    constexpr std::int64_t kExactWidthEmu = 1'234'567;
    constexpr std::int64_t kExactHeightEmu = 2'345'679;
    check(canvas.resizeSelectedInlineImage(
              static_cast<double>(kExactWidthEmu) / kEmuPerPoint,
              static_cast<double>(kExactHeightEmu) / kEmuPerPoint),
          "could not set exact size-dialog EMU fixture");
    const auto beforeNoOp = canvas.snapshot();
    bool inspected = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureSizeDialog"));
        auto* width = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureSize.width")) : nullptr;
        auto* height = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureSize.height")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureSize.buttons")) : nullptr;
        check(dialog && dialog->isModal() && width && height && buttons,
              "Picture Size dialog is missing required controls");
        const bool exactRange = width->decimals() >= 6 &&
            height->decimals() >= 6 &&
            std::llround(width->minimum() * kEmuPerInch) <= 12700 &&
            std::llround(width->maximum() * kEmuPerInch) >=
                core::kMaximumInlineImageDimensionEmu;
        if (!exactRange) {
            std::cerr << "size dialog decimals/min/max: "
                      << width->decimals() << '/' << height->decimals()
                      << ' ' << width->minimum() << ' '
                      << width->maximum() << '\n';
        }
        check(exactRange,
              "Picture Size dialog cannot represent the core geometry range exactly");
        inspected = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageSizeDialog();
    const auto unchanged = selectedImage(canvas);
    check(inspected && canvas.snapshot().revision == beforeNoOp.revision &&
              unchanged.width_emu == kExactWidthEmu &&
              unchanged.height_emu == kExactHeightEmu,
          "accepting an untouched Picture Size dialog quantized geometry or created a revision");

    const auto beforeEdit = canvas.snapshot();
    std::int64_t expectedWidthEmu = 0;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureSizeDialog"));
        auto* width = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureSize.width"));
        auto* lock = dialog->findChild<QCheckBox*>(
            QStringLiteral("pictureSize.lockAspect"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureSize.buttons"));
        check(width && lock && buttons,
              "Picture Size edit path is missing required controls");
        lock->setChecked(false);
        width->setValue(width->value() + 0.125);
        expectedWidthEmu = static_cast<std::int64_t>(
            std::llround(width->value() * kEmuPerInch));
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageSizeDialog();
    const auto changed = selectedImage(canvas);
    check(canvas.snapshot().revision.value() ==
                  beforeEdit.revision.value() + 1 &&
              changed.width_emu == expectedWidthEmu &&
              changed.height_emu == kExactHeightEmu,
          "editing one Picture Size field altered an untouched exact dimension");
    canvas.undo();
    const auto undone = selectedImage(canvas);
    check(undone.width_emu == kExactWidthEmu &&
              undone.height_emu == kExactHeightEmu,
          "Picture Size undo did not restore exact EMU geometry");
}

void testLayoutDialogPreservesExactEmus(DocumentCanvas& canvas) {
    const core::ImageLayout exactSquare{
        core::ImagePlacement::square, 101, 202, 303, 404, true};
    check(canvas.setSelectedImageLayout(exactSquare),
          "could not set exact layout-dialog fixture");
    const auto beforeNoOp = canvas.snapshot();
    bool inspected = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureLayoutDialog"));
        auto* top = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceTop")) : nullptr;
        auto* right = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceRight")) : nullptr;
        auto* bottom = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceBottom")) : nullptr;
        auto* left = dialog ? dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceLeft")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureLayout.buttons")) : nullptr;
        check(dialog && dialog->isModal() && top && right && bottom && left &&
                  buttons,
              "Picture Layout dialog is missing required controls");
        check(top->decimals() >= 6 && right->decimals() >= 6 &&
                  bottom->decimals() >= 6 && left->decimals() >= 6 &&
                  top->isEnabled() && right->isEnabled() &&
                  bottom->isEnabled() && left->isEnabled() &&
                  top->maximum() * kEmuPerInch >=
                      static_cast<double>(
                          core::kMaximumImageWrapDistanceEmu) - 1.0,
              "anchored Picture Layout controls do not expose the exact core range");
        inspected = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageLayoutDialog();
    check(inspected && canvas.snapshot().revision == beforeNoOp.revision &&
              selectedImage(canvas).layout == exactSquare,
          "accepting an untouched Picture Layout dialog quantized values or created a revision");

    const core::ImageLayout exactInline{
        core::ImagePlacement::inline_with_text, 505, 606, 707, 808, true};
    check(canvas.setSelectedImageLayout(exactInline),
          "could not set inline distance-preservation fixture");
    const auto beforeInline = canvas.snapshot();
    bool inlineControlsInspected = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureLayoutDialog"));
        auto* top = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceTop"));
        auto* right = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceRight"));
        auto* bottom = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceBottom"));
        auto* left = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("pictureLayout.distanceLeft"));
        auto* move = dialog->findChild<QCheckBox*>(
            QStringLiteral("pictureLayout.moveWithText"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureLayout.buttons"));
        check(top && right && bottom && left && move && buttons &&
                  !top->isEnabled() && !right->isEnabled() &&
                  !bottom->isEnabled() && !left->isEnabled() &&
                  !move->isEnabled() && move->isChecked(),
              "inline-only layout values were presented as visually editable");
        inlineControlsInspected = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageLayoutDialog();
    check(inlineControlsInspected &&
              canvas.snapshot().revision == beforeInline.revision &&
              selectedImage(canvas).layout == exactInline,
          "inline Picture Layout dialog did not preserve ignored distances exactly");
}

void testAltTextDialogUsesUtf8ByteLimit(DocumentCanvas& canvas) {
    const auto beforeAscii = canvas.snapshot();
    const QString maximumAscii(
        static_cast<qsizetype>(core::kMaximumImageAccessibleNameBytes),
        QLatin1Char('a'));
    bool acceptedMaximumAscii = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureAltTextDialog"));
        auto* edit = dialog ? dialog->findChild<QLineEdit*>(
            QStringLiteral("pictureAltText.value")) : nullptr;
        auto* count = dialog ? dialog->findChild<QLabel*>(
            QStringLiteral("pictureAltText.byteCount")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureAltText.buttons")) : nullptr;
        check(dialog && dialog->isModal() && edit && count && buttons,
              "Picture Alt Text dialog is missing byte-aware controls");
        edit->setText(maximumAscii);
        check(edit->text() == maximumAscii &&
                  buttons->button(QDialogButtonBox::Ok)->isEnabled() &&
                  count->text().contains(QStringLiteral("4096")),
              "Picture Alt Text dialog truncated or rejected valid 4096-byte ASCII");
        acceptedMaximumAscii = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageAltTextDialog();
    check(acceptedMaximumAscii &&
              canvas.snapshot().revision.value() ==
                  beforeAscii.revision.value() + 1 &&
              selectedImage(canvas).accessible_name ==
                  maximumAscii.toStdString(),
          "Picture Alt Text dialog did not store the full ASCII byte limit");

    const QString oversizedAscii(
        static_cast<qsizetype>(core::kMaximumImageAccessibleNameBytes + 1U),
        QLatin1Char('b'));
    const auto beforeOversizedAscii = canvas.snapshot();
    bool rejectedOversizedAscii = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureAltTextDialog"));
        auto* edit = dialog ? dialog->findChild<QLineEdit*>(
            QStringLiteral("pictureAltText.value")) : nullptr;
        auto* count = dialog ? dialog->findChild<QLabel*>(
            QStringLiteral("pictureAltText.byteCount")) : nullptr;
        auto* buttons = dialog ? dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureAltText.buttons")) : nullptr;
        check(edit && count && buttons,
              "Picture Alt Text overflow path is missing required controls");
        edit->setText(oversizedAscii);
        const bool overflowRejected = edit->text() == oversizedAscii &&
            !buttons->button(QDialogButtonBox::Ok)->isEnabled() &&
            !count->styleSheet().isEmpty();
        if (!overflowRejected) {
            std::cerr << "ASCII overflow diagnostics: maxLength="
                      << edit->maxLength()
                      << " utf16=" << edit->text().size()
                      << " utf8=" << edit->text().toUtf8().size()
                      << " okEnabled="
                      << buttons->button(QDialogButtonBox::Ok)->isEnabled()
                      << " countStyle="
                      << count->styleSheet().toStdString() << '\n';
        }
        check(overflowRejected,
              "Picture Alt Text silently truncated an over-limit ASCII value to valid text");
        rejectedOversizedAscii = true;
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    canvas.showSelectedImageAltTextDialog();
    check(rejectedOversizedAscii &&
              canvas.snapshot().revision == beforeOversizedAscii.revision &&
              selectedImage(canvas).accessible_name ==
                  maximumAscii.toStdString(),
          "cancelling over-limit ASCII alt text changed the document");

    QString maximumEmoji;
    const QString emoji = QString::fromUtf8("\xf0\x9f\xa6\x89");
    for (std::size_t index = 0;
         index < core::kMaximumImageAccessibleNameBytes / 4U; ++index) {
        maximumEmoji += emoji;
    }
    check(maximumEmoji.toUtf8().size() ==
              static_cast<qsizetype>(
                  core::kMaximumImageAccessibleNameBytes),
          "emoji alt-text boundary fixture has the wrong UTF-8 size");
    const auto beforeEmoji = canvas.snapshot();
    bool acceptedMaximumEmoji = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureAltTextDialog"));
        auto* edit = dialog->findChild<QLineEdit*>(
            QStringLiteral("pictureAltText.value"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureAltText.buttons"));
        edit->setText(maximumEmoji);
        check(buttons->button(QDialogButtonBox::Ok)->isEnabled(),
              "Picture Alt Text dialog rejected a valid multibyte boundary");
        acceptedMaximumEmoji = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    canvas.showSelectedImageAltTextDialog();
    check(acceptedMaximumEmoji &&
              canvas.snapshot().revision.value() ==
                  beforeEmoji.revision.value() + 1 &&
              selectedImage(canvas).accessible_name.size() ==
                  core::kMaximumImageAccessibleNameBytes,
          "Picture Alt Text dialog did not store a valid multibyte boundary");

    const QString oversizedEmoji = maximumEmoji + emoji;
    const auto beforeOversized = canvas.snapshot();
    bool rejectedOversized = false;
    QTimer::singleShot(0, &canvas, [&] {
        auto* dialog = canvas.findChild<QDialog*>(
            QStringLiteral("pictureAltTextDialog"));
        auto* edit = dialog->findChild<QLineEdit*>(
            QStringLiteral("pictureAltText.value"));
        auto* count = dialog->findChild<QLabel*>(
            QStringLiteral("pictureAltText.byteCount"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("pictureAltText.buttons"));
        edit->setText(oversizedEmoji);
        check(edit->text() == oversizedEmoji &&
                  !buttons->button(QDialogButtonBox::Ok)->isEnabled() &&
                  !count->styleSheet().isEmpty(),
              "Picture Alt Text dialog did not flag an over-limit UTF-8 value");
        rejectedOversized = true;
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    canvas.showSelectedImageAltTextDialog();
    check(rejectedOversized &&
              canvas.snapshot().revision == beforeOversized.revision &&
              selectedImage(canvas).accessible_name.size() ==
                  core::kMaximumImageAccessibleNameBytes,
          "cancelling invalid Picture Alt Text changed the document");
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir applicationData;
    check(applicationData.isValid(),
          "could not isolate picture-dialog application data");
    qputenv("XDG_DATA_HOME", applicationData.path().toUtf8());
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    docxstudio::app::SpellChecker spelling;
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 600);
    canvas.show();
    check(canvas.insertInlineImage(encodedPng(), QStringLiteral("fixture")),
          "could not insert picture-dialog image");

    testSizeDialogPreservesExactEmus(canvas);
    testLayoutDialogPreservesExactEmus(canvas);
    testAltTextDialogUsesUtf8ByteLimit(canvas);

    std::cout << "picture dialog tests passed\n";
    return 0;
}
