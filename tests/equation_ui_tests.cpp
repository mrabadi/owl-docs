#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/MathLayout.h"
#include "docxstudio/app/SpellChecker.h"
#include "docxstudio/math/latex_parser.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <zip.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

namespace {

using docxstudio::app::DocumentCanvas;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QString fromUtf16(const std::u16string& text) {
    return QString::fromUtf16(text.data(), static_cast<qsizetype>(text.size()));
}

void sendKey(DocumentCanvas& canvas, Qt::Key key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &event);
    check(event.isAccepted(), "document canvas did not accept an editing key");
}

QByteArray zipMember(const QString& path, const char* memberName) {
    int code = 0;
    const auto encodedPath = QFile::encodeName(path);
    zip_t* archive = zip_open(encodedPath.constData(), ZIP_RDONLY, &code);
    check(archive != nullptr, "could not open saved DOCX as a ZIP package");
    zip_stat_t stat{};
    zip_stat_init(&stat);
    check(zip_stat(archive, memberName, ZIP_FL_UNCHANGED, &stat) == 0,
          "saved DOCX is missing word/document.xml");
    check(stat.size <= static_cast<zip_uint64_t>(
              std::numeric_limits<qsizetype>::max()),
          "saved document.xml is too large for the test process");
    zip_file_t* member = zip_fopen(archive, memberName, ZIP_FL_UNCHANGED);
    check(member != nullptr, "could not open saved word/document.xml");
    QByteArray result(static_cast<qsizetype>(stat.size), Qt::Uninitialized);
    const auto read = zip_fread(member, result.data(), stat.size);
    check(read == static_cast<zip_int64_t>(stat.size),
          "could not read saved word/document.xml");
    check(zip_fclose(member) == 0,
          "could not close saved word/document.xml");
    zip_discard(archive);
    return result;
}

int changedPixels(const QImage& left, const QImage& right) {
    check(left.size() == right.size(),
          "render comparison images have different sizes");
    int changed = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QColor a = left.pixelColor(x, y);
            const QColor b = right.pixelColor(x, y);
            const int delta = std::abs(a.red() - b.red()) +
                              std::abs(a.green() - b.green()) +
                              std::abs(a.blue() - b.blue());
            if (delta > 48) ++changed;
        }
    }
    return changed;
}

void testSemanticEquationEditing(docxstudio::app::SpellChecker& spelling) {
    DocumentCanvas canvas(spelling);
    canvas.resize(900, 700);
    canvas.insertText(QStringLiteral("BeforeAfter"));
    for (int index = 0; index < 5; ++index) sendKey(canvas, Qt::Key_Left);

    check(canvas.insertEquation(QStringLiteral("  \\frac{a}{b}  "), false),
          "canvas rejected a valid safe-subset equation");
    auto snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().size() == 1,
          "equation insertion changed the paragraph count");
    const auto& paragraph = snapshot.document.paragraphs().front();
    check(paragraph.text() == std::u16string(u"Before\ufffcAfter"),
          "equation is not represented by exactly one U+FFFC atom");
    check(paragraph.equations().size() == 1,
          "equation metadata was not attached to the paragraph");
    const auto& equation = paragraph.equations().front();
    check(equation.id.isValid() && equation.utf16_offset == 6 &&
              equation.canonical_latex == "\\frac{a}{b}" &&
              !equation.display,
          "equation identity, offset, canonical source, or display flag is wrong");
    check(canvas.hasNonTextChanges(),
          "semantic equation insertion was not classified as a non-text edit");
    check(canvas.selection().anchor == canvas.selection().focus &&
              canvas.selection().focus.utf16_offset == 7,
          "caret did not land immediately after the inline equation atom");

    sendKey(canvas, Qt::Key_Left, Qt::ShiftModifier);
    check(canvas.selection().anchor.utf16_offset == 7 &&
              canvas.selection().focus.utf16_offset == 6,
          "Shift+Left did not select the equation as one caret unit");
    check(canvas.selectedText() == QStringLiteral("\\(\\frac{a}{b}\\)"),
          "copy/selection text did not expose a safe readable equation fallback");
    canvas.insertText(QStringLiteral("replacement"));
    snapshot = canvas.snapshot();
    check(fromUtf16(snapshot.document.paragraphs().front().text()) ==
              QStringLiteral("BeforereplacementAfter") &&
              snapshot.document.paragraphs().front().equations().empty(),
          "replacing a selected equation left an orphan atom or metadata");
    canvas.undo();
    snapshot = canvas.snapshot();
    check(snapshot.document.paragraphs().front().text() ==
              std::u16string(u"Before\ufffcAfter") &&
              snapshot.document.paragraphs().front().equations().size() == 1,
          "Undo did not restore the selected equation atom and metadata");
    canvas.redo();
    check(canvas.snapshot().document.paragraphs().front().equations().empty(),
          "Redo did not remove the replaced equation atom again");

    canvas.undo();
    sendKey(canvas, Qt::Key_Right);
    sendKey(canvas, Qt::Key_Backspace);
    snapshot = canvas.snapshot();
    check(fromUtf16(snapshot.document.paragraphs().front().text()) ==
              QStringLiteral("BeforeAfter") &&
              snapshot.document.paragraphs().front().equations().empty(),
          "Backspace did not delete an equation atomically");
    canvas.undo();
    check(canvas.snapshot().document.paragraphs().front().equations().size() == 1,
          "Undo did not restore a backspaced equation");
    canvas.redo();
    check(canvas.snapshot().document.paragraphs().front().equations().empty(),
          "Redo did not delete the restored equation");
}

void testScreenAndPdfRendering(docxstudio::app::SpellChecker& spelling) {
    // Keep a direct MathLayout reference in this executable. Besides checking
    // the renderer contract independently, this catches accidental omission
    // of the implementation object from the application library.
    const auto parsed = docxstudio::math::parseLatex("\\frac{x}{y}");
    check(static_cast<bool>(parsed),
          "could not parse direct MathLayout smoke fixture");
    docxstudio::app::MathLayout directLayout(parsed.value(), QFont{});
    check(directLayout.valid() && directLayout.metrics().width > 0.0 &&
              directLayout.metrics().height() > 0.0,
          "direct equation layout did not produce finite visible metrics");

    DocumentCanvas empty(spelling);
    DocumentCanvas equationCanvas(spelling);
    empty.resize(900, 700);
    equationCanvas.resize(900, 700);
    empty.show();
    equationCanvas.show();
    check(equationCanvas.insertEquation(
              QStringLiteral("\\frac{x_1+1}{\\sqrt{y}}"), false),
          "could not prepare rendered equation fixture");
    empty.clearFocus();
    equationCanvas.clearFocus();
    QApplication::processEvents();

    const QImage emptyImage = empty.grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    const QImage equationImage = equationCanvas.grab().toImage().convertToFormat(
        QImage::Format_ARGB32);
    check(changedPixels(emptyImage, equationImage) > 40,
          "equation did not add visible vector-rendered ink to the document canvas");
    const QString renderPath = qEnvironmentVariable(
        "OWL_DOCS_EQUATION_RENDER_PATH");
    if (!renderPath.isEmpty()) {
        check(equationImage.save(renderPath),
              "could not save the rendered equation QA image");
    }
    const auto snapshot = equationCanvas.snapshot();
    check(fromUtf16(snapshot.document.paragraphs().front().text()) ==
              QString(QChar::ObjectReplacementCharacter) &&
              !fromUtf16(snapshot.document.paragraphs().front().text())
                   .contains(QStringLiteral("frac")),
          "rendered equation was stored as literal LaTeX or a bracket marker");

    DocumentCanvas pdfCanvas(spelling);
    pdfCanvas.insertText(QStringLiteral("SearchableBefore SearchableAfter"));
    for (int index = 0; index < 16; ++index) sendKey(pdfCanvas, Qt::Key_Left);
    check(pdfCanvas.insertEquation(QStringLiteral("\\frac{p}{q}"), false),
          "could not insert the PDF equation fixture");
    QTemporaryDir output;
    check(output.isValid(), "could not create PDF test directory");
    const QString pdf = output.filePath(QStringLiteral("equation.pdf"));
    QString error;
    check(pdfCanvas.exportPdf(pdf, error) && QFileInfo(pdf).size() > 500,
          "shared display-list renderer did not export the equation PDF");
    const QString pdftotext = QStandardPaths::findExecutable(
        QStringLiteral("pdftotext"));
    if (!pdftotext.isEmpty()) {
        QProcess extractor;
        extractor.start(pdftotext, {pdf, QStringLiteral("-")});
        check(extractor.waitForStarted(5000),
              "could not start pdftotext for equation PDF");
        check(extractor.waitForFinished(10000) &&
                  extractor.exitStatus() == QProcess::NormalExit &&
                  extractor.exitCode() == 0,
              "pdftotext could not extract the equation PDF");
        const QByteArray text = extractor.readAllStandardOutput();
        check(text.contains("SearchableBefore") &&
                  text.contains("SearchableAfter"),
              "surrounding PDF text was not searchable/extractable");
        check(!text.contains("\\frac") && !text.contains("[Equation") &&
                  !text.contains("\xe3\x80\x96"),
              "literal LaTeX or a legacy equation marker leaked into the PDF");
    } else {
        std::cout << "pdftotext unavailable; equation extraction check skipped\n";
    }
}

DocumentCanvas* equationCanvasIn(docxstudio::app::MainWindow& window) {
    for (auto* canvas : window.findChildren<DocumentCanvas*>()) {
        const auto snapshot = canvas->snapshot();
        for (const auto& paragraph : snapshot.document.paragraphs()) {
            if (!paragraph.equations().empty()) return canvas;
        }
    }
    return nullptr;
}

void testDialogNativeSaveAndReopen() {
    docxstudio::app::MainWindow window;
    window.resize(1000, 760);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<DocumentCanvas*>();
    auto* insertAction = window.findChild<QAction*>(
        QStringLiteral("insert.equation"));
    check(canvas && insertAction,
          "could not reach the equation insertion command");
    canvas->insertText(QStringLiteral("DialogBefore DialogAfter"));
    sendKey(*canvas, Qt::Key_Home);
    for (int index = 0; index < 12; ++index) sendKey(*canvas, Qt::Key_Right);

    bool inspectedDialog = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("insertEquationDialog"));
        check(dialog && dialog->isModal(),
              "Insert Equation did not open one modal dialog");
        auto* latex = dialog->findChild<QLineEdit*>(
            QStringLiteral("insertEquation.latex"));
        auto* display = dialog->findChild<QCheckBox*>(
            QStringLiteral("insertEquation.display"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("insertEquation.buttons"));
        check(latex && display && buttons &&
                  latex->isAncestorOf(display) == false,
              "Insert Equation dialog is missing its LaTeX, display, or button controls");
        check(latex->window() == dialog && display->window() == dialog &&
                  buttons->window() == dialog,
              "LaTeX and display controls were split across dialogs");
        latex->setText(QStringLiteral("\\frac{a}{b}"));
        display->setChecked(true);
        inspectedDialog = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    insertAction->trigger();
    check(inspectedDialog,
          "Insert Equation dialog could not be inspected");
    auto snapshot = canvas->snapshot();
    const auto& paragraph = snapshot.document.paragraphs().front();
    check(paragraph.text() == std::u16string(u"DialogBefore\ufffc DialogAfter") &&
              paragraph.equations().size() == 1 &&
              paragraph.equations().front().canonical_latex == "\\frac{a}{b}" &&
              paragraph.equations().front().display,
          "equation dialog did not create the requested semantic display equation");

    QTemporaryDir output;
    check(output.isValid(), "could not create DOCX equation test directory");
    const QString path = output.filePath(QStringLiteral("semantic-equation.docx"));
    auto* saveAs = window.findChild<QAction*>(QStringLiteral("file.saveAs"));
    check(saveAs != nullptr, "could not reach Save As for the equation document");
    bool selectedDestination = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QFileDialog*>();
        check(dialog != nullptr, "Save As did not open a file dialog");
        dialog->selectFile(path);
        selectedDestination = true;
        check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection),
              "could not accept the equation Save As destination");
    });
    saveAs->trigger();
    check(selectedDestination && QFileInfo::exists(path),
          "semantic equation document was not saved");

    const QByteArray xml = zipMember(path, "word/document.xml");
    check(xml.contains("xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"") &&
              xml.contains("<m:oMathPara>") && xml.contains("<m:oMath>") &&
              xml.contains("<m:f>") && xml.contains("<m:num>") &&
              xml.contains("<m:den>"),
          "Save As did not emit native display OMML for the equation");
    check(!xml.contains("\\frac{a}{b}") && !xml.contains("\xef\xbf\xbc") &&
              !xml.contains("\xe3\x80\x96"),
          "Save As leaked literal LaTeX, U+FFFC, or a legacy marker into DOCX XML");

    docxstudio::app::MainWindow reopened;
    check(reopened.openPath(path),
          "Owl Docs could not reopen its native equation DOCX");
    auto* imported = equationCanvasIn(reopened);
    check(imported != nullptr,
          "reopening native OMML did not reconstruct a semantic equation atom");
    snapshot = imported->snapshot();
    const auto& importedParagraph = snapshot.document.paragraphs().front();
    check(importedParagraph.text() ==
              std::u16string(u"DialogBefore\ufffc DialogAfter") &&
              importedParagraph.equations().size() == 1 &&
              importedParagraph.equations().front().utf16_offset == 12 &&
              importedParagraph.equations().front().canonical_latex ==
                  "\\frac{a}{b}" &&
              importedParagraph.equations().front().display,
          "native OMML source, placement, or display semantics changed on reopen");
    check(!imported->isModified(),
          "reopening an equation DOCX incorrectly marked it modified");
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir applicationData;
    check(applicationData.isValid(),
          "could not isolate equation UI application data");
    qputenv("XDG_DATA_HOME", applicationData.path().toUtf8());
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    docxstudio::app::SpellChecker spelling;
    testSemanticEquationEditing(spelling);
    testScreenAndPdfRendering(spelling);
    testDialogNativeSaveAndReopen();
    std::cout << "equation UI tests passed\n";
    return 0;
}
