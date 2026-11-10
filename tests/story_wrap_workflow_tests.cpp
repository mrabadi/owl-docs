#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QMarginsF>
#include <QMessageBox>
#include <QSettings>
#include <QSizeF>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <zip.h>
#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void measuredStoryWraps() {
    using namespace docxstudio;
    auto document = core::Document::create({core::Paragraph::create(u"Body").value()}).value();
    check(bool(document.setHeaderText(u"A regional header that wraps across more than one measured line\tCenter\tRight")),
          "regional wrap source fixture");
    const auto lines = app::DocumentCanvas::measureStoryLines(document, false,
        QSizeF(612, 792), QMarginsF(72, 108, 72, 108), QStringLiteral("DejaVu Sans"), 12, 1);
    check(lines[0] > 1 && lines[1] == 1 && lines[2] == 1,
          "shared story measurement does not distinguish wrapped and single-line regions");
    check(bool(document.setHeaderText(u"{PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES}\t\t")),
          "page-count measurement fixture");
    const auto onePage = app::DocumentCanvas::measureStoryLines(document, false,
        QSizeF(612, 792), QMarginsF(72, 108, 72, 108), QStringLiteral("DejaVu Sans"), 12, 1);
    const auto manyPages = app::DocumentCanvas::measureStoryLines(document, false,
        QSizeF(612, 792), QMarginsF(72, 108, 72, 108), QStringLiteral("DejaVu Sans"), 12, 100);
    check(onePage[0] == 1 && manyPages[0] > 1,
          "shared story measurement ignores resolved page-count field width");
}

docxstudio::app::DocumentCanvas* activeCanvas(docxstudio::app::MainWindow& window) {
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("documentTabs"));
    auto* canvas = tabs ? qobject_cast<docxstudio::app::DocumentCanvas*>(tabs->currentWidget()) : nullptr;
    check(canvas, "story workflow has no active document");
    return canvas;
}

std::string member(const QString& path, const char* name) {
    int error = 0;
    auto* archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY, &error);
    check(archive, "open story package");
    zip_stat_t info{};
    check(zip_stat(archive, name, 0, &info) == 0, "locate story package member");
    auto* file = zip_fopen(archive, name, 0);
    std::string result(static_cast<std::size_t>(info.size), '\0');
    check(file && zip_fread(file, result.data(), result.size()) == static_cast<zip_int64_t>(result.size()),
          "read story package member");
    zip_fclose(file);
    zip_discard(archive);
    return result;
}

void save(docxstudio::app::MainWindow& window, const QString& destination = {}) {
    bool warned = false;
    bool chosen = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget);
                dialog && dialog->isVisible() && !chosen && !destination.isEmpty()) {
                chosen = true;
                dialog->selectFile(destination);
                check(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection), "accept story Save destination");
            }
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible()) {
                warned = true;
                std::cerr << "Unexpected story save prompt: " << box->text().toStdString()
                          << '\n' << box->detailedText().toStdString() << '\n';
                box->reject();
            }
        }
    });
    driver.start(0);
    auto* action = window.findChild<QAction*>(QStringLiteral("file.save"));
    check(action, "story workflow has no Save action");
    action->trigger();
    driver.stop();
    check(!warned && !activeCanvas(window)->isModified() && (destination.isEmpty() || chosen),
          "story save requires conversion or leaves the document unsaved");
}

void writeFresh(const QString& path, const QString& header, const QString& footer, int pages = 1) {
    docxstudio::app::MainWindow window;
    auto* canvas = activeCanvas(window);
    canvas->insertText(QStringLiteral("Regional story body"));
    for (int page = 1; page < pages; ++page) {
        canvas->insertPageBreak();
        canvas->insertText(QStringLiteral("Body page"));
    }
    canvas->setMarginsPoints(108, 72, 108, 72);
    check(canvas->setHeaderFooterText(header, footer), "set regional story fixture");
    check(canvas->pageCount() == pages, "regional story fixture has the wrong actual page count");
    save(window, path);
}

void regionalWrapWorkflow(const QString& directory) {
    using namespace docxstudio;
    const QString header = QStringLiteral("A regional header that wraps across more than one measured line\tCenter\tPage {PAGE} of {PAGES}");
    const QString footer = QStringLiteral("Footer\tA regional footer that wraps across more than one measured line\tRight");
    check(!header.contains(QLatin1Char('\n')) && !footer.contains(QLatin1Char('\n')), "wrap fixture contains explicit line breaks");
    const auto wrappedPath = directory + QStringLiteral("/wrapped.docx");
    writeFresh(wrappedPath, header, footer);
    check(member(wrappedPath, "word/header1.xml").find("<w:tbl>") != std::string::npos &&
              member(wrappedPath, "word/footer1.xml").find("<w:tbl>") != std::string::npos,
          "naturally wrapped regional stories were saved as sequential tab paragraphs instead of native cells");
    const auto pagedPath = directory + QStringLiteral("/page-count-wrap.docx");
    const QString pagedHeader = QStringLiteral("{PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES} {PAGES}\t\t");
    writeFresh(pagedPath, pagedHeader, {}, 100);
    check(member(pagedPath, "word/header1.xml").find("<w:tbl>") != std::string::npos,
          "live export ignores resolved PAGES width when choosing native regional layout");
    const auto shortPath = directory + QStringLiteral("/single-line.docx");
    writeFresh(shortPath, QStringLiteral("An old regional header\tCenter\tRight"), QStringLiteral("Left\tCenter\tRight"));
    const auto originalHeader = member(shortPath, "word/header1.xml");
    check(originalHeader.find("<w:tbl>") == std::string::npos, "single-line source fixture unexpectedly uses a table");
    QSettings preferences;
    preferences.setValue(QStringLiteral("editor/defaultFontPointSize"), 18.0);
    preferences.sync();
    {
        app::MainWindow reopened;
        check(reopened.openPath(pagedPath), "reopen page-count-driven regional wrap");
        auto* canvas = activeCanvas(reopened);
        if (canvas->pageCount() != 100 || canvas->headerText() != pagedHeader)
            std::cerr << "Reopened pages=" << canvas->pageCount() << "; header="
                      << canvas->headerText().toStdString() << "; expected=" << pagedHeader.toStdString() << '\n';
        check(canvas->pageCount() == 100 && canvas->headerText() == pagedHeader,
              "reopen loses the actual pagination or source of the page-count story");
        canvas->setMarginsPoints(109, 72, 109, 72);
        save(reopened);
        check(member(pagedPath, "word/header1.xml").find("<w:tbl>") != std::string::npos,
              "baseline export treats a hundred-page PAGES story as a single-page story");
    }
    {
        app::MainWindow reopened;
        check(reopened.openPath(wrappedPath), "reopen naturally wrapped story package");
        auto* canvas = activeCanvas(reopened);
        check(canvas->headerText() == header && canvas->footerText() == footer && canvas->defaultFontPointSize() == 12,
              "reopen changes regional source text or source font defaults");
        canvas->setMarginsPoints(109, 72, 109, 72);
        save(reopened);
        const auto package = ooxml::DocxDocument::open(wrappedPath.toStdString());
        check(package && package->headerText() == header.toStdString() && package->footerText() == footer.toStdString() &&
                  member(wrappedPath, "word/header1.xml").find("<w:tbl>") != std::string::npos &&
                  member(wrappedPath, "word/footer1.xml").find("<w:tbl>") != std::string::npos,
              "layout save after reopen loses native regional table text or representation");
    }
    {
        app::MainWindow reopened;
        check(reopened.openPath(shortPath), "reopen older single-line source form");
        auto* canvas = activeCanvas(reopened);
        check(canvas->defaultFontPointSize() == 12, "current preferences override single-line source font defaults");
        canvas->setMarginsPoints(109, 72, 109, 72);
        save(reopened);
        check(member(shortPath, "word/header1.xml") == originalHeader,
              "source-default wrap measurement rewrites an untouched older single-line story part");
    }
    const auto legacyPath = directory + QStringLiteral("/older-wrapped-paragraph.docx");
    ooxml::NewParagraph paragraph;
    paragraph.runs.push_back({"Original body", {}});
    ooxml::NewDocumentBody legacy;
    legacy.blocks.push_back(paragraph);
    legacy.header_text = header.toStdString();
    ooxml::DocumentDefaults defaults;
    defaults.font_family = "DejaVu Sans";
    defaults.font_size_half_points = 24;
    check(bool(ooxml::DocxDocument::writeNew(legacyPath.toStdString(), legacy, {}, defaults)), "create older sequential story source");
    const auto legacyHeader = member(legacyPath, "word/header1.xml");
    check(legacyHeader.find("<w:tbl>") == std::string::npos, "legacy wrapped source form was not preserved by default");
    {
        app::MainWindow reopened;
        check(reopened.openPath(legacyPath), "open older wrapped sequential source");
        auto* canvas = activeCanvas(reopened);
        canvas->selectAll();
        canvas->insertText(QStringLiteral("Safely patched body"));
        save(reopened);
        check(member(legacyPath, "word/header1.xml") == legacyHeader,
              "safe body-text patch rewrites the retained older story representation");
    }
}
}
int main(int argc, char** argv) {
    QTemporaryDir directory;
    check(directory.isValid(), "create isolated story workflow directory");
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OwlDocsTests"));
    QCoreApplication::setApplicationName(QStringLiteral("StoryWrapWorkflow"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings preferences;
    preferences.setValue(QStringLiteral("editor/defaultFontFamily"), QStringLiteral("DejaVu Sans"));
    preferences.setValue(QStringLiteral("editor/defaultFontPointSize"), 12.0);
    preferences.sync();
    measuredStoryWraps();
    regionalWrapWorkflow(directory.path());
    return 0;
}
