#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/AppTheme.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QAbstractButton>
#include <QComboBox>
#include <QScrollBar>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdlib>
#include <cmath>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
QImage rendering(QWidget* widget) {
    QImage result(widget->size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::white);
    QPainter painter(&result);
    widget->render(&painter);
    return result;
}
QRect ink(const QImage& image, const QRect& region) {
    QRect result;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.red() < 90 && color.green() < 90 && color.blue() < 90)
                result = result.united(QRect(x, y, 1, 1));
        }
    }
    return result;
}
void mouse(QWidget* target, QEvent::Type type, const QPointF& point) {
    QMouseEvent event(type, point, Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void storyLabelsRemainVisible() {
    using namespace docxstudio;
    for (const double margin : {72.0, 36.0}) {
        for (const int width : {1100, 420}) {
            for (const bool footer : {false, true}) {
                app::SpellChecker spelling;
                app::DocumentCanvas canvas(spelling);
                app::applyAppTheme(&canvas);
                canvas.resize(width, 700);
                canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 12, 4);
                canvas.setMarginsPoints(margin, margin, margin, margin);
                canvas.show();
                canvas.beginHeaderFooterEditing(footer, 0, 0);
                QApplication::processEvents();
                QImage unobscured(canvas.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
                unobscured.fill(Qt::white);
                {
                    QPainter painter(&unobscured);
                    canvas.viewport()->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
                }
                const auto actual = rendering(canvas.viewport());
                if (const auto directory = qEnvironmentVariable("OWL_DOCS_CAPTURE_LABELS"); !directory.isEmpty())
                    check(actual.save(QStringLiteral("%1/labels-%2pt-%3-%4.png").arg(directory).arg(margin).arg(width)
                        .arg(footer ? QStringLiteral("footer") : QStringLiteral("header"))), "cannot save story label raster evidence");
                int labelPixels = 0;
                int coveredPixels = 0;
                for (int y = 0; y < unobscured.height(); ++y) {
                    for (int x = 0; x < unobscured.width(); ++x) {
                        const auto pixel = unobscured.pixelColor(x, y);
                        if (pixel.red() >= 90 && pixel.red() < 225 && pixel.green() > pixel.red() &&
                            pixel.green() > pixel.blue() && pixel.green() - pixel.red() <= 14 &&
                            pixel.green() - pixel.blue() <= 8) {
                            ++labelPixels;
                            if (actual.pixelColor(x, y) != pixel) ++coveredPixels;
                        }
                    }
                }
                if (labelPixels < 10)
                    std::cerr << "Story label fixture: margin=" << margin << " width=" << width << " footer=" << footer
                              << " pixels=" << labelPixels << '\n';
                check(labelPixels >= 10, "on-page story label raster fixture has no visible label glyphs");
                if (coveredPixels != 0)
                    std::cerr << "Story label: margin=" << margin << " width=" << width << " footer=" << footer
                              << " covered=" << coveredPixels << '/' << labelPixels << '\n';
                check(coveredPixels == 0, "inline editor or toolbar covers the on-page Header/Footer label");
            }
        }
    }
}
void narrowMarginsDoNotOverlapBody() {
    using namespace docxstudio;
    for (const bool footer : {false, true}) {
        app::SpellChecker spelling;
        app::DocumentCanvas canvas(spelling);
        canvas.resize(1100, 700);
        canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 8, 4);
        canvas.setMarginsPoints(36, 36, 36, 36);
        canvas.show();
        canvas.beginHeaderFooterEditing(footer, 0, 0);
        auto* editor = canvas.findChild<QPlainTextEdit*>(footer ? QStringLiteral("footerStoryEditor0") : QStringLiteral("headerStoryEditor0"));
        check(editor && editor->isVisible(), "Narrow-margin inline story fixture is unavailable");
        const auto before = canvas.snapshot();
        QString failure;
        QObject::connect(&canvas, &app::DocumentCanvas::operationFailed, &canvas,
            [&failure](const QString& message) { failure = message; });
        editor->insertPlainText(QStringLiteral("x"));
        check(canvas.snapshot().revision != before.revision &&
              editor->toPlainText() == QStringLiteral("x") && failure.isEmpty() &&
              (footer ? canvas.footerText() : canvas.headerText()).startsWith(QStringLiteral("x")),
              "a new Narrow-margin story fails to reserve body space while typing");
        canvas.endHeaderFooterEditing();
        check(canvas.setHeaderFooterText(footer ? QString() : QStringLiteral("Imported"),
            footer ? QStringLiteral("Imported") : QString()), "Narrow-margin imported story fixture");
        canvas.beginHeaderFooterEditing(footer, 0, 0);
        editor->moveCursor(QTextCursor::End);
        QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(editor, &backspace);
        check(editor->toPlainText() == QStringLiteral("Importe"),
              "Narrow-margin imported story cannot be shortened");
        editor->selectAll();
        editor->insertPlainText(QString());
        check(editor->toPlainText().isEmpty() &&
              (footer ? canvas.footerText() : canvas.headerText()).trimmed().isEmpty(),
              "clearing an imported story is rejected as an empty caret line overflow");
    }
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--narrow-fit-only"))) {
        narrowMarginsDoNotOverlapBody();
        return 0;
    }
    storyLabelsRemainVisible();
    if (application.arguments().contains(QStringLiteral("--labels-only"))) return 0;
    narrowMarginsDoNotOverlapBody();
    using namespace docxstudio;
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.resize(1100, 1100);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 12, 4);
    check(canvas.setHeaderFooterText(QStringLiteral("Inline evidence\tCenter line\tRight line"),
        QStringLiteral("Footer evidence\t\tPage {PAGE} of {PAGES}")), "header/footer fixture");
    canvas.show();
    QApplication::processEvents();
    const double scale = 96.0 / 72.0;
    const double pageWidth = canvas.pageWidthPoints() * scale;
    const double pageLeft = (std::max(pageWidth + 56.0, static_cast<double>(canvas.viewport()->width())) - pageWidth) / 2.0;
    // The supported native DOCX writer emits w:header="720" and w:footer="720"
    // in twips. The shared screen/input/PDF geometry must use those same 36 pt.
    const double nativeStoryDistance = 720.0 / 20.0;
    const int storyLeft = static_cast<int>(pageLeft + canvas.marginLeftPoints() * scale);
    const int storyWidth = static_cast<int>((canvas.pageWidthPoints() - canvas.marginLeftPoints() - canvas.marginRightPoints()) * scale / 3.0);
    const auto nativeHeaderInk = ink(rendering(canvas.viewport()), QRect(storyLeft, 28, storyWidth, static_cast<int>(72 * scale)));
    check(!nativeHeaderInk.isEmpty() && nativeHeaderInk.top() >= 28 + static_cast<int>(nativeStoryDistance * scale) &&
          nativeHeaderInk.top() <= 38 + static_cast<int>(nativeStoryDistance * scale),
          "header position diverges from the supported native 720-twip page distance");
    const int nativeFooterBottom = static_cast<int>(28 + (canvas.pageHeightPoints() - nativeStoryDistance) * scale);
    const auto nativeFooterInk = ink(rendering(canvas.viewport()), QRect(storyLeft,
        static_cast<int>(28 + (canvas.pageHeightPoints() - 72) * scale), storyWidth, static_cast<int>(72 * scale)));
    check(!nativeFooterInk.isEmpty() && nativeFooterInk.bottom() <= nativeFooterBottom && nativeFooterInk.bottom() >= nativeFooterBottom - 15,
          "footer position diverges from the supported native 720-twip page distance");
    const QRect headerRegion(storyLeft, static_cast<int>(28 + nativeStoryDistance * scale), storyWidth, static_cast<int>(30 * scale));
    const auto before = ink(rendering(canvas.viewport()), headerRegion);
    check(!before.isEmpty(), "header fixture has no visible glyphs");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    auto* editor = canvas.findChild<QPlainTextEdit*>(QStringLiteral("headerStoryEditor0"));
    check(editor && editor->isVisible(), "inline header editor is unavailable");
    editor->clearFocus();
    QApplication::processEvents();
    const auto during = ink(rendering(canvas.viewport()), headerRegion);
    check(before == during, "entering header editing changes rendered text size or page position");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("First inline line\nSecond inline line")), "multiline header fixture");
    QApplication::processEvents();
    const auto multiline = ink(rendering(canvas.viewport()), headerRegion);
    check(multiline.height() >= 25, "header newlines are not rendered as distinct document lines");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    editor->moveCursor(QTextCursor::End);
    editor->setFocus();
    QApplication::processEvents();
    const auto focused = rendering(canvas.viewport());
    editor->clearFocus();
    QApplication::processEvents();
    const auto unfocused = rendering(canvas.viewport());
    int caretPixels = 0;
    for (int y = headerRegion.top(); y <= headerRegion.bottom(); ++y)
        for (int x = headerRegion.left(); x <= headerRegion.right(); ++x)
            if (focused.pixelColor(x, y).lightness() < 90 && unfocused.pixelColor(x, y).lightness() >= 90) ++caretPixels;
    check(caretPixels >= 5, "focused inline story has no caret at its rendered text position");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(QStringLiteral("Page {PAGE} of {PAGES}")), "page-field cursor fixture");
    QApplication::processEvents();
    const auto fieldInk = ink(rendering(canvas.viewport()), headerRegion);
    canvas.beginHeaderFooterEditing(false, 0, 0);
    const QPointF fieldEnd(fieldInk.right() + 3 - editor->x(), fieldInk.center().y() - editor->y());
    QMouseEvent fieldClick(QEvent::MouseButtonPress, fieldEnd, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &fieldClick);
    check(editor->textCursor().position() == editor->toPlainText().size(),
          "clicking the displayed page-field text puts the caret inside its hidden source token");
    QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QApplication::sendEvent(editor, &left);
    check(editor->textCursor().position() == editor->toPlainText().size() - 7,
          "Left moves inside a native page-count field instead of across it atomically");
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(editor, &right);
    const auto withField = canvas.snapshot().document;
    QKeyEvent erase(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(editor, &erase);
    check(editor->toPlainText() == QStringLiteral("Page {PAGE} of ") && canvas.headerText().startsWith(QStringLiteral("Page {PAGE} of \t")),
          "Backspace partially corrupts the hidden source of a displayed page-count field");
    canvas.undo();
    check(canvas.snapshot().document == withField, "Undo fails to restore a deleted native page-count field");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderFooterText({}, {}), "Word-mode fixture reset");
    canvas.insertText(QStringLiteral("Body text stays outside header editing"));
    QApplication::processEvents();
    const QRect bodyRegion(headerRegion.left(), static_cast<int>(28 + canvas.marginTopPoints() * scale), headerRegion.width() * 2, 40);
    const auto bodyBefore = ink(rendering(canvas.viewport()), bodyRegion);
    const QPointF headerPoint(headerRegion.left() + 30, headerRegion.top() + 12);
    mouse(canvas.viewport(), QEvent::MouseButtonPress, headerPoint);
    mouse(canvas.viewport(), QEvent::MouseButtonRelease, headerPoint);
    check(!canvas.isHeaderFooterEditing(), "a single margin click enters story editing instead of Word-style double click");
    mouse(canvas.viewport(), QEvent::MouseButtonDblClick, headerPoint);
    QApplication::processEvents();
    auto* close = canvas.findChild<QAbstractButton*>(QStringLiteral("story.close"));
    auto* footer = canvas.findChild<QAbstractButton*>(QStringLiteral("story.footer"));
    auto* page = canvas.findChild<QAbstractButton*>(QStringLiteral("story.insertPage"));
    auto* pages = canvas.findChild<QAbstractButton*>(QStringLiteral("story.insertPages"));
    auto* region = canvas.findChild<QComboBox*>(QStringLiteral("story.region"));
    check(canvas.isHeaderFooterEditing() && editor->hasFocus() && close && close->isVisible() && footer && page && pages && region,
          "double-click editing lacks inline focus or direct header/footer/field/Close controls");
    check(!bodyBefore.isEmpty() && ink(rendering(canvas.viewport()), bodyRegion).isEmpty(),
          "body text does not dim while editing the header/footer story");
    footer->click();
    region->setCurrentIndex(2);
    page->click(); pages->click();
    auto* footerRight = canvas.findChild<QPlainTextEdit*>(QStringLiteral("footerStoryEditor2"));
    check(footerRight && footerRight->isVisible() && footerRight->geometry().intersects(canvas.viewport()->rect()) &&
          canvas.footerText() == QStringLiteral("\t\t{PAGE}{PAGES}"), "footer navigation or native page-field insertion is not direct and visible");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(footerRight, &escape);
    check(!canvas.isHeaderFooterEditing() && !close->isVisible(), "Escape does not close story editing");
    canvas.verticalScrollBar()->setValue(0);
    canvas.beginHeaderFooterEditing(false, 0, 0);
    close->click();
    check(!canvas.isHeaderFooterEditing(), "Close Header and Footer does not leave story editing");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    mouse(canvas.viewport(), QEvent::MouseButtonPress, QPointF(bodyRegion.left() + 30, bodyRegion.top() + 10));
    check(!canvas.isHeaderFooterEditing(), "clicking body text does not leave story editing");
    canvas.selectAll();
    canvas.setFontPointSize(19);
    canvas.setFontFamily(QStringLiteral("Liberation Serif"));
    const auto bodyFormatting = canvas.snapshot().document;
    canvas.beginHeaderFooterEditing(false, 0, 0);
    check(!canvas.insertEquation(QStringLiteral("x^{2}")) && canvas.snapshot().document == bodyFormatting,
          "an unsupported story insertion silently replaces the hidden body selection");
    canvas.toggleBold(); canvas.toggleItalic(); canvas.setFontPointSize(24);
    canvas.setFontFamily(QStringLiteral("DejaVu Serif"));
    canvas.setAlignment(core::ParagraphAlignment::right);
    canvas.applyParagraphStyle(QStringLiteral("Heading1"));
    canvas.toggleBullets(); canvas.toggleNumbering();
    check(canvas.snapshot().document == bodyFormatting,
          "formatting a plain-text header silently changes the hidden body selection");
    check(canvas.currentFontFamily() == QStringLiteral("DejaVu Sans") && canvas.currentFontPointSize() == 12 &&
          !canvas.paragraphStylesAvailable(), "story mode reports the body selection font or enables unsupported paragraph styles");
    canvas.endHeaderFooterEditing();
    check(canvas.currentFontPointSize() == 19, "closing story mode loses the original body insertion format");
    auto firstPage = core::Paragraph::create(u"First page body").value();
    auto secondPage = core::Paragraph::create(u"Second page body").value();
    auto multipage = core::Document::create({firstPage, secondPage}).value();
    core::ParagraphFormatDelta breakBefore;
    breakBefore.page_break_before = core::PropertyDelta<bool>::set(true);
    check(bool(multipage.applyParagraphFormat({secondPage.id()}, breakBefore)), "multipage footer fixture");
    canvas.setDocument(multipage);
    canvas.resize(1000, 700);
    canvas.setZoomPercent(150);
    canvas.beginHeaderFooterEditing(true, 2, 1);
    QApplication::processEvents();
    const int footerScroll = canvas.verticalScrollBar()->value();
    check(canvas.pageCount() == 2 && footerScroll > 0 && footerRight->geometry().intersects(canvas.viewport()->rect()), "second-page footer is not visible at zoom");
    QKeyEvent typeFooter(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    QApplication::sendEvent(footerRight, &typeFooter);
    QApplication::processEvents();
    check(canvas.verticalScrollBar()->value() == footerScroll && footerRight->geometry().intersects(canvas.viewport()->rect()) && footerRight->hasFocus(),
          "typing in a later-page footer scrolls back to the stale body caret");
    canvas.endHeaderFooterEditing();
    canvas.setZoomPercent(100);
    canvas.resize(1100, 1100);
    canvas.verticalScrollBar()->setValue(0);
    check(canvas.setHeaderText(QStringLiteral("Valid\t\t")), "bounded header fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    QTextCursor validCursor = editor->textCursor();
    validCursor.setPosition(2); editor->setTextCursor(validCursor);
    const auto validHeader = canvas.snapshot();
    QString fitFailure;
    QObject::connect(&canvas, &app::DocumentCanvas::operationFailed, &canvas,
        [&fitFailure](const QString& message) { fitFailure = message; });
    editor->insertPlainText(QStringLiteral("\nAdditional line").repeated(40));
    check(canvas.snapshot().document == validHeader.document && canvas.snapshot().revision == validHeader.revision &&
          editor->toPlainText() == QStringLiteral("Valid") && editor->textCursor().position() == 2 &&
          fitFailure.contains(QStringLiteral("margin"), Qt::CaseInsensitive),
          "oversized header edits exceed the bounded story capacity instead of restoring the last valid source and caret");
    canvas.endHeaderFooterEditing();
    const QString oversized = QStringLiteral("Additional line\n").repeated(40).trimmed() + QStringLiteral("\t\t");
    check(canvas.setHeaderText(oversized), "imported oversized story fixture");
    canvas.clearFocus();
    QApplication::processEvents();
    const QRect belowHeader(headerRegion.left(), static_cast<int>(std::ceil(28 + 400 * scale)) + 1,
        headerRegion.width() - 12, 4);
    check(ink(rendering(canvas.viewport()), belowHeader).isEmpty(),
          "oversized imported story paints outside the same bounded region used by its inline editor");
    const auto importedOversized = canvas.snapshot();
    canvas.beginHeaderFooterEditing(false, 0, 0);
    check(canvas.snapshot().document == importedOversized.document && canvas.snapshot().revision == importedOversized.revision &&
          editor->toPlainText() == oversized.left(oversized.size() - 2) &&
          editor->toolTip().contains(QStringLiteral("margin"), Qt::CaseInsensitive),
          "oversized imported story source is changed or has no visible overflow explanation");
    editor->moveCursor(QTextCursor::End);
    QKeyEvent shorten(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
    QApplication::sendEvent(editor, &shorten);
    check(canvas.headerText().size() == oversized.size() - 1,
          "reducing existing oversized story text is incorrectly blocked");
    const auto beforeOrphan = canvas.snapshot();
    const QString beforeOrphanText = editor->toPlainText();
    editor->insertPlainText(QString(QChar::ObjectReplacementCharacter));
    check(canvas.snapshot().document == beforeOrphan.document && canvas.snapshot().revision == beforeOrphan.revision &&
          editor->toPlainText() == beforeOrphanText,
          "a rejected story source leaves the inline widget different from the document");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderFooterText(QStringLiteral("\t\t"), QStringLiteral("\t\t")), "bounded story picture fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    QImage portrait(10, 200, QImage::Format_RGB32); portrait.fill(Qt::red);
    QByteArray pictureBytes;
    QBuffer pictureBuffer(&pictureBytes); pictureBuffer.open(QIODevice::WriteOnly);
    check(portrait.save(&pictureBuffer, "PNG"), "bounded story picture encoding");
    const std::vector<std::uint8_t> portraitBytes(
        reinterpret_cast<const std::uint8_t*>(pictureBytes.constData()),
        reinterpret_cast<const std::uint8_t*>(pictureBytes.constData()) + pictureBytes.size());
    check(canvas.insertHeaderFooterImage(portraitBytes, QStringLiteral("Portrait")), "bounded story picture insert");
    check(canvas.snapshot().document.headerImages().size() == 1 &&
          canvas.snapshot().document.headerImages().front().height_emu == 150 * 12700,
          "a newly inserted header picture stores a larger size than the region used to render it");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderText(oversized), "combined story picture fit fixture");
    canvas.beginHeaderFooterEditing(false, 0, 0);
    const auto beforeCombined = canvas.snapshot();
    check(!canvas.insertHeaderFooterImage(portraitBytes, QStringLiteral("Does not fit")) &&
          canvas.snapshot().document == beforeCombined.document && canvas.snapshot().revision == beforeCombined.revision &&
          editor->toPlainText() == oversized.left(oversized.size() - 2),
          "picture insertion commits story content that cannot fit its combined text and image height");
    canvas.endHeaderFooterEditing();
    check(canvas.setHeaderFooterText(QStringLiteral("\t\tRight"), QStringLiteral("\t\tRight")), "narrow right story fixture");
    canvas.resize(420, 700);
    for (const bool footerStory : {false, true}) {
        canvas.setZoomPercent(100);
        canvas.horizontalScrollBar()->setValue(0);
        canvas.beginHeaderFooterEditing(footerStory, 2, 0);
        auto* active = canvas.findChild<QPlainTextEdit*>(footerStory ? QStringLiteral("footerStoryEditor2") : QStringLiteral("headerStoryEditor2"));
        QApplication::processEvents();
        check(active && active->geometry().left() >= 0 && active->geometry().right() < canvas.viewport()->width(),
              "entering a right header/footer region leaves its input outside a narrow viewport");
        const int savedPosition = active->textCursor().position();
        canvas.resize(1100, 700); QApplication::processEvents();
        canvas.resize(420, 700); QApplication::processEvents();
        check(active->geometry().left() >= 0 && active->geometry().right() < canvas.viewport()->width() &&
              active->textCursor().position() == savedPosition,
              "resizing into a narrow workspace hides the active story or resets its caret");
        canvas.setZoomPercent(150);
        QApplication::processEvents();
        QInputMethodQueryEvent caretQuery(Qt::ImCursorRectangle);
        QApplication::sendEvent(active, &caretQuery);
        const auto caret = caretQuery.value(Qt::ImCursorRectangle).toRectF().translated(active->pos()).toAlignedRect();
        auto* tools = canvas.findChild<QWidget*>(QStringLiteral("story.tools"));
        check(active->textCursor().position() == savedPosition && canvas.viewport()->rect().contains(caret) &&
              tools && !tools->geometry().intersects(caret),
              "narrow story caret is hidden after zoom or covered by the story controls");
        const int horizontal = canvas.horizontalScrollBar()->value();
        QKeyEvent typeRight(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        QApplication::sendEvent(active, &typeRight);
        check(active->hasFocus() && canvas.horizontalScrollBar()->value() == horizontal,
              "typing in a revealed narrow story loses its visible caret or focus");
        canvas.endHeaderFooterEditing();
    }
    canvas.resize(1100, 1100);
    canvas.setZoomPercent(100);
    canvas.horizontalScrollBar()->setValue(0);
    canvas.verticalScrollBar()->setValue(0);
    check(canvas.setHeaderFooterText(QStringLiteral("First\nSecond\tCentered\tRight"),
        QStringLiteral("Footer\t\tPage {PAGE} of {PAGES}")), "story PDF appearance fixture");
    QApplication::processEvents();
    QTemporaryDir output;
    check(output.isValid(), "story PDF output directory");
    const QString pdfPath = output.filePath(QStringLiteral("stories.pdf"));
    QString pdfError;
    const auto beforePdf = canvas.snapshot();
    const auto layoutGeneration = canvas.layoutGeneration();
    check(canvas.exportPdf(pdfPath, pdfError) && canvas.snapshot().document == beforePdf.document &&
          canvas.snapshot().revision == beforePdf.revision && canvas.layoutGeneration() == layoutGeneration,
          "PDF story rendering changes the document or its shared pagination");
    const auto rasterizer = QStandardPaths::findExecutable(QStringLiteral("pdftoppm"),
        {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    check(!rasterizer.isEmpty(), "pdftoppm is required for story appearance verification");
    QProcess raster;
    const QString rasterBase = output.filePath(QStringLiteral("stories-page"));
    raster.start(rasterizer, {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-singlefile"),
        QStringLiteral("-png"), QStringLiteral("-r"), QStringLiteral("96"), pdfPath, rasterBase});
    check(raster.waitForFinished(30000) && raster.exitCode() == 0, "story PDF rasterization failed");
    const QImage pdfPage(rasterBase + QStringLiteral(".png"));
    check(!pdfPage.isNull(), "story PDF page is unavailable");
    const auto pdfInk = ink(pdfPage, QRect(96, 48, 208, 40));
    const auto screenInk = ink(rendering(canvas.viewport()), headerRegion);
    check(!pdfInk.isEmpty() && std::abs(pdfInk.width() - screenInk.width()) <= 2 &&
          std::abs(pdfInk.height() - screenInk.height()) <= 2 &&
          std::abs(pdfInk.top() - (screenInk.top() - 28)) <= 2,
          "PDF and on-page header typography or multiline geometry diverge");
    QProcess extractor;
    extractor.start(QStandardPaths::findExecutable(QStringLiteral("pdftotext")), {QStringLiteral("-layout"), pdfPath, QStringLiteral("-")});
    check(extractor.waitForFinished(30000) && extractor.exitCode() == 0, "story PDF extraction failed");
    const QString pdfText = QString::fromUtf8(extractor.readAllStandardOutput());
    check(pdfText.contains(QStringLiteral("First")) && pdfText.contains(QStringLiteral("Second")) &&
          pdfText.contains(QStringLiteral("Centered")) && pdfText.contains(QStringLiteral("Right")) &&
          pdfText.contains(QStringLiteral("Page 1 of 2")) && pdfText.contains(QStringLiteral("Page 2 of 2")) &&
          !pdfText.contains(QStringLiteral("{PAGE}")), "PDF loses regional text or leaves native page fields unresolved");
    if (qEnvironmentVariableIsSet("OWL_DOCS_CAPTURE_STORY")) {
        output.setAutoRemove(false);
        rendering(canvas.viewport()).save(output.filePath(QStringLiteral("canvas.png")));
        std::cout << "Story appearance artifacts: " << output.path().toStdString() << '\n';
    }
    std::cout << "Header/footer UI tests passed\n";
}
