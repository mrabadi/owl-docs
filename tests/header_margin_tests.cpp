#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/SpellChecker.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTemporaryDir>
#include <QScrollBar>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
QImage render(QWidget* widget) {
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
}
int ink(const QImage& image, QRect rect) {
    rect = rect.intersected(image.rect());
    int count = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const auto pixel = image.pixelColor(x,y);
            if (pixel.red() < 90 && pixel.green() < 90 && pixel.blue() < 90) ++count;
        }
    return count;
}
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace docxstudio;
    app::SpellChecker spelling;
    app::DocumentCanvas canvas(spelling);
    canvas.resize(1100, 1200);
    canvas.setEditorDefaults(QStringLiteral("DejaVu Sans"), 12, 4);
    check(canvas.setHeaderFooterText(QStringLiteral("First header line\nSecond header line\tCenter\tRight"),
        QStringLiteral("First footer line\nSecond footer line\tCenter\tRight")), "margin story fixture");
    canvas.show();
    QApplication::processEvents();
    const auto document = canvas.snapshot().document;
    const double scale = 96.0 / 72.0;
    const auto storyInk = [&](const QImage& image, bool footer) {
        const double pageLeft = (canvas.viewport()->width() - canvas.pageWidthPoints() * scale) / 2.0;
        const double top = 28 + (footer ? canvas.pageHeightPoints() - 36 - 30 : 36) * scale;
        return ink(image, QRect(static_cast<int>(pageLeft + canvas.marginLeftPoints() * scale), static_cast<int>(top),
            static_cast<int>((canvas.pageWidthPoints()-canvas.marginLeftPoints()-canvas.marginRightPoints()) * scale / 3), 42));
    };
    const auto original = render(canvas.viewport());
    check(storyInk(original,false) > 100 && storyInk(original,true) > 100, "initial header/footer glyph fixture");
    canvas.setMarginsPoints(36,36,36,36);
    QApplication::processEvents();
    const auto changed = render(canvas.viewport());
    check(canvas.snapshot().document == document, "changing margins rewrites semantic story content");
    check(storyInk(changed,false) > 100, "changing margins makes the existing header disappear");
    check(storyInk(changed,true) > 100, "changing margins makes the existing footer disappear");
    canvas.undo();
    check(canvas.marginTopPoints() == 72 && canvas.marginBottomPoints() == 72 && canvas.snapshot().document == document,
          "Undo does not restore margins and intact stories");
    for (const bool footer : {false,true}) {
        app::DocumentCanvas editing(spelling);
        editing.resize(1100,1200);
        editing.setEditorDefaults(QStringLiteral("DejaVu Sans"),12,4);
        editing.setForeground(QColor(Qt::red));
        editing.insertText(QStringLiteral("Body remains separate"));
        editing.setMarginsPoints(36,36,36,36);
        editing.show();
        editing.beginHeaderFooterEditing(footer,0,0);
        auto* editor=editing.findChild<QPlainTextEdit*>(footer ? QStringLiteral("footerStoryEditor0") : QStringLiteral("headerStoryEditor0"));
        check(editor && editor->isVisible(),"narrow story editor fixture");
        editor->insertPlainText(QStringLiteral("First line\nSecond line\nThird line"));
        check((footer ? editing.footerText() : editing.headerText()).startsWith(QStringLiteral("First line\nSecond line\nThird line")),
            "typing a multiline story with narrow margins is rejected instead of reserving body space");
        const auto cursorPosition=editor->textCursor().position();
        editing.setMarginsPoints(54,54,54,54);
        check(editor->isVisible() && editor->textCursor().position()==cursorPosition &&
            editor->toPlainText()==QStringLiteral("First line\nSecond line\nThird line"),
            "changing margins during story editing corrupts the active input or loses its caret");
        editing.setMarginsPoints(36,36,36,36);
        editing.endHeaderFooterEditing();
        QApplication::processEvents();
        check(editing.snapshot().document.findParagraph(editing.snapshot().document.paragraphs().front().id())->text()==u"Body remains separate",
            "growing a story rewrites body text");
        if (!footer) {
            const auto image=render(editing.viewport());
            int firstBody=image.height(),lastHeader=-1;
            const int left=static_cast<int>((editing.viewport()->width()-editing.pageWidthPoints()*scale)/2+36*scale);
            for (int y=50;y<300;++y) for(int x=left;x<left+160;++x) {
                const auto pixel=image.pixelColor(x,y);
                if(pixel.red()>160 && pixel.green()<80 && pixel.blue()<80) firstBody=std::min(firstBody,y);
                if(pixel.red()<90 && pixel.green()<90 && pixel.blue()<90) lastHeader=std::max(lastHeader,y);
            }
            check(firstBody<image.height() && lastHeader>=0 && firstBody>lastHeader+4,
                "growing header text overlaps the first body line");
            QTemporaryDir output;
            QString error;
            const auto pdf=output.filePath(QStringLiteral("narrow.pdf"));
            const auto beforePdf=editing.snapshot();
            check(editing.exportPdf(pdf,error),"narrow-margin PDF export");
            QProcess extractor;
            extractor.start(QStringLiteral("/usr/bin/pdftotext"),{QStringLiteral("-layout"),pdf,QStringLiteral("-")});
            check(extractor.waitForFinished(30000) && extractor.exitCode()==0,"narrow PDF extraction");
            const auto text=QString::fromUtf8(extractor.readAllStandardOutput());
            check(text.contains(QStringLiteral("First line")) && text.contains(QStringLiteral("Third line")) &&
                text.contains(QStringLiteral("Body remains separate")) && editing.snapshot().document==beforePdf.document,
                "narrow-margin PDF loses story/body content or mutates the document");
        }
    }
    app::DocumentCanvas pictures(spelling);
    pictures.setEditorDefaults(QStringLiteral("DejaVu Sans"),12,4);
    pictures.setMarginsPoints(36,36,36,36);
    pictures.beginHeaderFooterEditing(false,0,0);
    QImage logo(80,48,QImage::Format_RGB32);
    logo.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    check(buffer.open(QIODevice::WriteOnly) && logo.save(&buffer,"PNG"),"narrow story picture fixture");
    check(pictures.insertHeaderFooterImage({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        reinterpret_cast<const std::uint8_t*>(bytes.constData())+bytes.size()},QStringLiteral("Logo")),
        "narrow story picture insertion");
    check(pictures.snapshot().document.headerImages().front().height_emu>=18*12700,
        "narrow margins shrink a new header picture to an invisible one-point dot");
    std::cout << "Header/footer margin preservation passed\n";
}
