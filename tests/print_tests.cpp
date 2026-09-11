#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/SpellChecker.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QPageLayout>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QtGui/QPageRanges>
#include <QtPrintSupport/QPrintPreviewWidget>
#include <QtPrintSupport/QPrinter>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

docxstudio::core::Document threePageDocument() {
    using namespace docxstudio::core;
    auto first = Paragraph::create(u"OWL_PRINT_PAGE_ONE_UNIQUE");
    auto second = Paragraph::create(u"OWL_PRINT_PAGE_TWO_UNIQUE");
    auto third = Paragraph::create(u"OWL_PRINT_PAGE_THREE_UNIQUE");
    check(first && second && third, "could not create print paragraphs");
    const NodeId secondId = second.value().id();
    const NodeId thirdId = third.value().id();
    std::vector<Paragraph> paragraphs;
    paragraphs.push_back(std::move(first.value()));
    paragraphs.push_back(std::move(second.value()));
    paragraphs.push_back(std::move(third.value()));
    auto document = Document::create(std::move(paragraphs));
    check(static_cast<bool>(document), "could not create print document");

    ParagraphFormatDelta pageBreak;
    pageBreak.page_break_before = PropertyDelta<bool>::set(true);
    check(static_cast<bool>(document.value().applyParagraphFormat(
              {secondId, thirdId}, pageBreak)),
          "could not create deterministic print pages");
    return std::move(document.value());
}

QString flowMarker(int index) {
    return QStringLiteral("OWL_FLOW_%1").arg(index, 3, 10, QLatin1Char('0'));
}

docxstudio::core::Document softPaginatedDocument(int paragraphCount) {
    using namespace docxstudio::core;
    std::vector<Paragraph> paragraphs;
    paragraphs.reserve(static_cast<std::size_t>(paragraphCount));
    for (int index = 0; index < paragraphCount; ++index) {
        auto paragraph = Paragraph::create(
            flowMarker(index).toStdU16String());
        check(static_cast<bool>(paragraph),
              "could not create soft-pagination paragraph");
        paragraphs.push_back(std::move(paragraph.value()));
    }
    auto document = Document::create(std::move(paragraphs));
    check(static_cast<bool>(document),
          "could not create soft-pagination document");
    return std::move(document.value());
}

std::optional<QString> runPdfTool(const QString& executable,
                                  const QStringList& arguments) {
    const QString program = QStandardPaths::findExecutable(executable);
    if (program.isEmpty()) return std::nullopt;
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(10000) ||
        process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return std::nullopt;
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

std::optional<int> pdfPageCount(const QString& path) {
    const auto information = runPdfTool(QStringLiteral("pdfinfo"), {path});
    if (!information) return std::nullopt;
    const QRegularExpression pattern(QStringLiteral("(?:^|\\n)Pages:\\s+(\\d+)"));
    const auto match = pattern.match(*information);
    if (!match.hasMatch()) return std::nullopt;
    return match.captured(1).toInt();
}

std::optional<QSizeF> pdfPageSize(const QString& path) {
    const auto information = runPdfTool(QStringLiteral("pdfinfo"), {path});
    if (!information) return std::nullopt;
    const QRegularExpression pattern(QStringLiteral(
        "(?:^|\\n)Page size:\\s+([0-9.]+) x ([0-9.]+) pts"));
    const auto match = pattern.match(*information);
    if (!match.hasMatch()) return std::nullopt;
    return QSizeF(match.captured(1).toDouble(),
                  match.captured(2).toDouble());
}

std::optional<QString> pdfText(const QString& path) {
    return runPdfTool(QStringLiteral("pdftotext"),
                      {QStringLiteral("-layout"), path, QStringLiteral("-")});
}

std::optional<QString> pdfPageText(const QString& path, int oneBasedPage) {
    return runPdfTool(
        QStringLiteral("pdftotext"),
        {QStringLiteral("-f"), QString::number(oneBasedPage),
         QStringLiteral("-l"), QString::number(oneBasedPage),
         QStringLiteral("-layout"), path, QStringLiteral("-")});
}

void checkPdfHeader(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "could not open printed PDF");
    check(file.size() > 500, "printed PDF was unexpectedly small");
    check(file.read(5) == QByteArrayLiteral("%PDF-"),
          "printer output did not contain a PDF header");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PrintTests"));

    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas canvas(spelling);
    canvas.setDocument(threePageDocument());
    canvas.setPageSizePoints(500.0, 700.0);
    canvas.setMarginsPoints(50.0, 50.0, 50.0, 50.0);
    check(canvas.pageCount() == 3, "explicit page breaks did not paginate to three pages");
    check(canvas.currentPageNumber() == 1, "new document did not begin on page one");

    QPrinter configured(QPrinter::HighResolution);
    configured.setOutputFormat(QPrinter::PdfFormat);
    QString error;
    check(canvas.configurePrinter(configured, error),
          "could not configure the printer page layout");
    const QRect fullPage = configured.pageLayout().fullRectPoints();
    check(configured.fullPage(), "print setup did not use full-page coordinates");
    check(std::abs(fullPage.width() - 500) <= 1 &&
              std::abs(fullPage.height() - 700) <= 1,
          "print setup did not inherit the editor page size");
    check(configured.pageLayout().marginsPoints() == QMarginsF(0, 0, 0, 0),
          "print setup added a second set of margins");

    docxstudio::app::DocumentCanvas landscapeCanvas(spelling);
    landscapeCanvas.setPageSizePoints(700.0, 500.0);
    QPrinter landscapePrinter(QPrinter::HighResolution);
    landscapePrinter.setOutputFormat(QPrinter::PdfFormat);
    error.clear();
    check(landscapeCanvas.configurePrinter(landscapePrinter, error),
          "could not configure a landscape print layout");
    const QRect landscapePage =
        landscapePrinter.pageLayout().fullRectPoints();
    check(std::abs(landscapePage.width() - 700) <= 1 &&
              std::abs(landscapePage.height() - 500) <= 1,
          "landscape printer geometry was transposed");

    auto tableDocument = threePageDocument();
    const auto thirdParagraphId = tableDocument.paragraphs()[2].id();
    auto table = docxstudio::core::Table::create(1, 1);
    check(static_cast<bool>(table), "could not create current-page table");
    const auto tableId = table.value().id();
    check(static_cast<bool>(tableDocument.insertTable(
              thirdParagraphId, std::move(table.value()))),
          "could not place a table on the second page");
    docxstudio::app::DocumentCanvas tableCanvas(spelling);
    tableCanvas.setDocument(std::move(tableDocument));
    check(tableCanvas.selectTable(tableId) &&
              tableCanvas.currentPageNumber() == 2,
          "current-page reporting ignored a whole-table selection");

    QTemporaryDir output;
    check(output.isValid(), "could not create print test directory");

    // Assign every marker to a page through the live canvas layout, then
    // independently extract each exported PDF page.  This proves that soft
    // pagination (not only explicit page breaks) reaches screen and PDF from
    // the same page geometry and content assignment.
    constexpr int kFlowParagraphCount = 48;
    docxstudio::app::DocumentCanvas flowCanvas(spelling);
    flowCanvas.setDocument(softPaginatedDocument(kFlowParagraphCount));
    flowCanvas.setPageSizePoints(320.0, 220.0);
    flowCanvas.setMarginsPoints(36.0, 36.0, 24.0, 24.0);
    const int flowPageCount = flowCanvas.pageCount();
    check(flowPageCount >= 3,
          "soft-pagination fixture did not span enough canvas pages");
    std::vector<int> markerPages;
    markerPages.reserve(kFlowParagraphCount);
    for (int index = 0; index < kFlowParagraphCount; ++index) {
        const QString marker = flowMarker(index);
        check(flowCanvas.findNext(marker, true),
              "canvas could not locate a soft-pagination marker");
        markerPages.push_back(flowCanvas.currentPageNumber());
    }

    const QString flowPdfPath =
        output.filePath(QStringLiteral("shared-soft-pagination.pdf"));
    error.clear();
    check(flowCanvas.exportPdf(flowPdfPath, error),
          "soft-paginated PDF export failed");
    checkPdfHeader(flowPdfPath);
    if (const auto count = pdfPageCount(flowPdfPath)) {
        check(*count == flowPageCount,
              "exported PDF page count diverged from canvas pagination");
    }
    if (!QStandardPaths::findExecutable(QStringLiteral("pdftotext")).isEmpty()) {
        for (int page = 1; page <= flowPageCount; ++page) {
            const auto pageText = pdfPageText(flowPdfPath, page);
            check(pageText.has_value(),
                  "could not extract one page of the shared-pagination PDF");
            for (int index = 0; index < kFlowParagraphCount; ++index) {
                const bool exportedOnPage =
                    pageText->contains(flowMarker(index));
                const bool canvasAssignedToPage =
                    markerPages[static_cast<std::size_t>(index)] == page;
                check(exportedOnPage == canvasAssignedToPage,
                      "screen and PDF assigned a flow marker to different pages");
            }
        }
    }

    const QString allPagesPath = output.filePath(QStringLiteral("all-pages.pdf"));
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(allPagesPath);
        printer.setResolution(144);
        check(canvas.printTo(printer, error), "all-pages print failed");
    }
    checkPdfHeader(allPagesPath);
    if (const auto count = pdfPageCount(allPagesPath)) {
        check(*count == 3, "all-pages print emitted the wrong page count");
    }
    if (const auto size = pdfPageSize(allPagesPath)) {
        check(std::abs(size->width() - 500.0) <= 0.1 &&
                  std::abs(size->height() - 700.0) <= 0.1,
              "printed PDF page geometry diverged from editor pagination");
    }
    if (const auto text = pdfText(allPagesPath)) {
        const auto first = text->indexOf(QStringLiteral("PAGE_ONE"));
        const auto second = text->indexOf(QStringLiteral("PAGE_TWO"));
        const auto third = text->indexOf(QStringLiteral("PAGE_THREE"));
        check(first >= 0 && first < second && second < third,
              "all-pages print did not preserve document page order");
    }

    const QString rangePath = output.filePath(QStringLiteral("page-range.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(rangePath);
        printer.setResolution(144);
        printer.setPrintRange(QPrinter::PageRange);
        printer.setFromTo(2, 2);
        check(canvas.printTo(printer, error), "single-page range print failed");
    }
    checkPdfHeader(rangePath);
    if (const auto count = pdfPageCount(rangePath)) {
        check(*count == 1, "page-range print emitted more than the requested page");
    }
    if (const auto text = pdfText(rangePath)) {
        check(text->contains(QStringLiteral("PAGE_TWO")) &&
                  !text->contains(QStringLiteral("PAGE_ONE")) &&
                  !text->contains(QStringLiteral("PAGE_THREE")),
              "page-range print rendered content outside the requested page");
    }

    const QString disjointPath =
        output.filePath(QStringLiteral("disjoint-range.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(disjointPath);
        printer.setResolution(144);
        QPageRanges ranges;
        ranges.addPage(1);
        ranges.addPage(3);
        printer.setPageRanges(ranges);
        printer.setPrintRange(QPrinter::PageRange);
        check(canvas.printTo(printer, error),
              "disjoint page-range print failed");
    }
    if (const auto count = pdfPageCount(disjointPath)) {
        check(*count == 2, "disjoint page range emitted the wrong page count");
    }
    if (const auto text = pdfText(disjointPath)) {
        check(text->contains(QStringLiteral("PAGE_ONE")) &&
                  text->contains(QStringLiteral("PAGE_THREE")) &&
                  !text->contains(QStringLiteral("PAGE_TWO")),
              "disjoint page range rendered an unrequested page");
    }

    check(canvas.findNext(QStringLiteral("OWL_PRINT_PAGE_TWO_UNIQUE")),
          "could not move the caret to the second print page");
    check(canvas.currentPageNumber() == 2,
          "current-page reporting did not follow the caret");
    const QString currentPath = output.filePath(QStringLiteral("current-page.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(currentPath);
        printer.setResolution(144);
        printer.setPrintRange(QPrinter::CurrentPage);
        check(canvas.printTo(printer, error), "current-page print failed");
    }
    if (const auto count = pdfPageCount(currentPath)) {
        check(*count == 1, "current-page print emitted the wrong page count");
    }
    if (const auto text = pdfText(currentPath)) {
        check(text->contains(QStringLiteral("PAGE_TWO")) &&
                  !text->contains(QStringLiteral("PAGE_ONE")),
              "current-page print did not use the caret page");
    }

    const QString reversePath = output.filePath(QStringLiteral("reverse.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(reversePath);
        printer.setResolution(144);
        printer.setPageOrder(QPrinter::LastPageFirst);
        check(canvas.printTo(printer, error), "reverse-order print failed");
    }
    if (const auto text = pdfText(reversePath)) {
        const auto first = text->indexOf(QStringLiteral("PAGE_ONE"));
        const auto second = text->indexOf(QStringLiteral("PAGE_TWO"));
        const auto third = text->indexOf(QStringLiteral("PAGE_THREE"));
        check(third >= 0 && third < second && second < first,
              "reverse-order printing ignored the printer page order");
    }

    const QString copiesPath = output.filePath(QStringLiteral("copies.pdf"));
    error.clear();
    bool softwareCopies = false;
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(copiesPath);
        printer.setResolution(144);
        printer.setCopyCount(2);
        printer.setCollateCopies(true);
        softwareCopies = !printer.supportsMultipleCopies();
        check(canvas.printTo(printer, error), "collated-copy print failed");
    }
    if (softwareCopies) {
        if (const auto count = pdfPageCount(copiesPath)) {
            check(*count == 6,
                  "software copy emulation emitted the wrong page count");
        }
        if (const auto text = pdfText(copiesPath)) {
            check(text->count(QStringLiteral("PAGE_ONE")) == 2 &&
                      text->count(QStringLiteral("PAGE_TWO")) == 2 &&
                      text->count(QStringLiteral("PAGE_THREE")) == 2,
                  "software copy emulation lost or duplicated a page");
        }
    }

    const QString invalidPath = output.filePath(QStringLiteral("invalid.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(invalidPath);
        printer.setPrintRange(QPrinter::PageRange);
        printer.setFromTo(8, 9);
        check(!canvas.printTo(printer, error) && !error.isEmpty(),
              "out-of-document print range was not rejected");
    }
    check(!QFileInfo::exists(invalidPath),
          "invalid print range created an output file");

    const QString selectionPath = output.filePath(QStringLiteral("selection.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(selectionPath);
        printer.setPrintRange(QPrinter::Selection);
        check(!canvas.printTo(printer, error) && !error.isEmpty(),
              "unsupported selection-only print was not rejected explicitly");
    }

    canvas.selectAll();
    QString summary;
    check(canvas.createReplacementPreview(
              QStringLiteral("Unaccepted replacement"), summary, error),
          "could not create print-safety preview");
    const QString previewPath = output.filePath(QStringLiteral("preview.pdf"));
    error.clear();
    {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(previewPath);
        check(!canvas.printTo(printer, error) && !error.isEmpty(),
              "print did not reject an unaccepted Codex preview");
    }
    check(!QFileInfo::exists(previewPath),
          "rejected Codex-preview print created an output file");
    canvas.discardPreview();

    QPrinter previewPrinter(QPrinter::HighResolution);
    previewPrinter.setOutputFormat(QPrinter::PdfFormat);
    error.clear();
    check(canvas.configurePrinter(previewPrinter, error),
          "could not configure print-preview geometry");
    QPrintPreviewWidget previewWidget(&previewPrinter);
    bool previewPainted = false;
    QString previewError;
    QObject::connect(
        &previewWidget, &QPrintPreviewWidget::paintRequested,
        [&canvas, &previewPainted, &previewError](QPrinter* target) {
            previewPainted = true;
            static_cast<void>(canvas.printTo(*target, previewError));
        });
    previewWidget.updatePreview();
    QApplication::processEvents();
    check(previewPainted && previewError.isEmpty(),
          "Qt print preview could not paint through the shared page renderer");
    check(previewWidget.pageCount() == canvas.pageCount(),
          "Qt print preview page count diverged from editor pagination");

    docxstudio::app::MainWindow window;
    auto* previewAction = window.findChild<QAction*>(
        QStringLiteral("file.printPreview"));
    auto* printAction = window.findChild<QAction*>(QStringLiteral("file.print"));
    check(previewAction && printAction,
          "File menu is missing print or print-preview commands");
    check(!previewAction->icon().isNull() && !printAction->icon().isNull(),
          "print commands are missing editor-style icons");
    check(previewAction->shortcut() == QKeySequence(QStringLiteral("Ctrl+F2")),
          "print-preview command is missing its shortcut");
    check(!printAction->shortcuts().isEmpty(),
          "print command is missing the platform print shortcut");

    bool previewDialogOpened = false;
    QTimer::singleShot(0, &window, [&previewDialogOpened] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QStringLiteral("printPreviewDialog")) {
                previewDialogOpened = true;
            }
            if (auto* dialog = qobject_cast<QDialog*>(widget)) {
                dialog->reject();
            }
        }
    });
    previewAction->trigger();
    check(previewDialogOpened,
          "Print Preview command did not open the standard Qt preview dialog");

    std::cout << "print tests passed\n";
    return 0;
}
