#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <zip.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace docxstudio;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
std::string member(const std::filesystem::path& path, const char* name) {
    int error=0;
    auto* archive=zip_open(path.c_str(),ZIP_RDONLY,&error);
    check(archive,"open saved package");
    zip_stat_t stat{};
    check(zip_stat(archive,name,0,&stat)==0,"locate native member");
    auto* file=zip_fopen(archive,name,0);
    std::string result(static_cast<std::size_t>(stat.size),'\0');
    check(file && zip_fread(file,result.data(),result.size())==static_cast<zip_int64_t>(result.size()),"read native member");
    zip_fclose(file); zip_discard(archive);
    return result;
}
core::CharacterFormat effectiveAppearance(core::CharacterFormat format) {
    format.bold=format.bold.value_or(false);
    format.italic=format.italic.value_or(false);
    format.underline=format.underline.value_or(core::UnderlineStyle::none);
    format.strike=format.strike.value_or(false);
    format.foreground_argb=format.foreground_argb.value_or(0xff000000U);
    format.baseline=format.baseline.value_or(core::BaselinePosition::normal);
    return format;
}
core::ParagraphFormat effectiveAppearance(core::ParagraphFormat format) {
    format.alignment=format.alignment.value_or(core::ParagraphAlignment::left);
    format.left_indent_emu=format.left_indent_emu.value_or(0);
    format.right_indent_emu=format.right_indent_emu.value_or(0);
    format.first_line_indent_emu=format.first_line_indent_emu.value_or(0);
    format.space_before_emu=format.space_before_emu.value_or(0);
    format.space_after_emu=format.space_after_emu.value_or(0);
    format.line_spacing_emu=format.line_spacing_emu.value_or(152400);
    format.line_spacing_rule=format.line_spacing_rule.value_or(core::LineSpacingRule::automatic);
    format.keep_with_next=format.keep_with_next.value_or(false);
    format.keep_lines=format.keep_lines.value_or(false);
    format.page_break_before=format.page_break_before.value_or(false);
    return format;
}
}
int main(int argc, char** argv) {
    QTemporaryDir stateDirectory;
    qputenv("XDG_DATA_HOME",stateDirectory.path().toUtf8());
    QApplication application(argc,argv);
    QCoreApplication::setOrganizationName("OwlDocsTests");
    QCoreApplication::setApplicationName("LegacyStyleWorkflow");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir directory;
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    QSettings preferences;
    preferences.setValue(QStringLiteral("editor/defaultFontFamily"),QStringLiteral("DejaVu Serif"));
    preferences.setValue(QStringLiteral("editor/defaultFontPointSize"),18.0);
    preferences.sync();
    for (const auto* file : {"owldocs-0.6.0-desktop-styles.docx","owldocs-0.6.0-academic.docx",
                             "owldocs-0.6.1-desktop-styles.docx","owldocs-0.6.1-academic.docx"}) {
        const auto source=std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"tests/fixtures"/file;
        const auto target=directory.filePath(QString::fromLatin1(file));
        check(QFile::copy(QString::fromStdString(source.string()),target),"copy immutable old fixture");
        app::MainWindow window;
        check(window.openPath(target),"open 0.6.0 document");
        auto* canvas=window.findChild<app::DocumentCanvas*>();
        check(canvas,"find imported canvas");
        const auto sourcePackage=ooxml::DocxDocument::open(source);
        check(sourcePackage && canvas->defaultFontFamily()==QString::fromStdString(sourcePackage->sourceDefaults().font_family) &&
              canvas->defaultFontPointSize()==sourcePackage->sourceDefaults().font_size_half_points/2.0,
              "current insertion preferences replaced imported display font defaults");
        const auto imported=canvas->snapshot();
        if (std::string_view(file).find("academic")!=std::string_view::npos) {
            check(imported.document.notes().size()==1 && imported.document.tablesOfContents().size()==1,
                  "old academic snapshot failed native proof after style defaults changed");
        } else {
            const auto& title=imported.document.paragraphs()[2];
            check(title.characterFormatAt(0).foreground_argb==
                      (std::string_view(file).find("0.6.1")!=std::string_view::npos ? 0xff000000U : 0xff77216fU),
                  "old Title appearance changed on desktop import");
        }
        canvas->setMarginsPoints(canvas->marginTopPoints()+1.0,canvas->marginRightPoints(),
                                 canvas->marginBottomPoints(),canvas->marginLeftPoints());
        check(canvas->isModified(),"fixture layout edit did not mark document modified");
        bool warned=false;
        QTimer dialogs;
        QObject::connect(&dialogs,&QTimer::timeout,&window,[&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                warned=true;
                std::cerr << "Unexpected save prompt: " << box->text().toStdString()
                          << '\n' << box->detailedText().toStdString() << '\n';
                box->done(QMessageBox::No);
            }
        });
        dialogs.start(0);
        auto* save=window.findChild<QAction*>(QStringLiteral("file.save"));
        check(save,"find Save action");
        save->trigger();
        dialogs.stop();
        check(!warned && !canvas->isModified(),"old native document prompted for conversion on ordinary save");
        check(member(source,"word/styles.xml")==member(target.toStdString(),"word/styles.xml"),
              "ordinary save changed 0.6.0 style definitions");
        app::MainWindow reopened;
        check(reopened.openPath(target),"reopen old document after layout save");
        auto* reopenedCanvas=reopened.findChild<app::DocumentCanvas*>();
        check(reopenedCanvas,"find reopened canvas");
        const auto roundtrip=reopenedCanvas->snapshot();
        if (!imported.document.notes().empty()) {
            check(roundtrip.document==imported.document,
                  "old academic identities or semantics changed after save/reopen");
        } else {
            check(roundtrip.document.paragraphs().size()==imported.document.paragraphs().size(),
                  "old rich paragraph count changed after save/reopen");
            for (std::size_t index=0;index<14;++index) {
                const auto& before=imported.document.paragraphs()[index];
                const auto& after=roundtrip.document.paragraphs()[index];
                check(before.text()==after.text() && before.format()==after.format() &&
                      before.characterFormatAt(0)==after.characterFormatAt(0),
                      "old paragraph text or appearance changed after save/reopen");
            }
            canvas->applyParagraphStyle(QStringLiteral("Heading2"));
            const auto modernHeading=canvas->snapshot().document.paragraphs().front();
            check(modernHeading.styleId()==std::optional<std::string>{"Heading2"} &&
                  modernHeading.characterFormatAt(0).font_size_half_points==32 &&
                  modernHeading.characterFormatAt(0).foreground_argb==0xff2f6b57U,
                  "applying Heading2 did not use the modern appearance");
            warned=false;
            dialogs.start(0); save->trigger(); dialogs.stop();
            check(!warned && !canvas->isModified(),
                  "applying a modern heading to an old document prompted for conversion");
            check(member(source,"word/styles.xml")==member(target.toStdString(),"word/styles.xml"),
                  "applying a modern heading replaced the old style catalog");
            app::MainWindow modernReopened;
            check(modernReopened.openPath(target),"reopen legacy document with modern heading");
            const auto* modernCanvas=modernReopened.findChild<app::DocumentCanvas*>();
            check(modernCanvas &&
                  effectiveAppearance(modernCanvas->snapshot().document.paragraphs().front().characterFormatAt(0))==effectiveAppearance(modernHeading.characterFormatAt(0)) &&
                  effectiveAppearance(modernCanvas->snapshot().document.paragraphs().front().format())==effectiveAppearance(modernHeading.format()),
                  "modern heading appearance was lost against the retained legacy catalog");
        }
    }
    for (const auto* recoveryFixture:{"owldocs-0.6.0-academic.docx","owldocs-0.6.1-academic.docx"}) {
    const auto golden=std::filesystem::path(OWL_DOCS_SOURCE_DIR)/"tests/fixtures"/recoveryFixture;
    const auto missing=directory.filePath(QString::fromLatin1(recoveryFixture)+QStringLiteral(".removed.docx"));
    check(QFile::copy(QString::fromStdString(golden.string()),missing),"copy old recovery source");
    core::Document expectedRecovery;
    {
        app::MainWindow original;
        check(original.openPath(missing),"open old document for recovery checkpoint");
        auto* canvas=original.findChild<app::DocumentCanvas*>();
        check(canvas,"find checkpoint canvas");
        canvas->setMarginsPoints(canvas->marginTopPoints()+1.0,canvas->marginRightPoints(),
                                 canvas->marginBottomPoints(),canvas->marginLeftPoints());
        expectedRecovery=canvas->snapshot().document;
        bool checkpointed=false;
        for (auto* timer : original.findChildren<QTimer*>()) {
            if (timer->interval()==1500)
                checkpointed=QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection);
        }
        check(checkpointed,"checkpoint old native source");
    }
    check(QFile::remove(missing),"remove original old source before recovery");
    app::MainWindow recovered;
    bool recoveryWarning=false;
    const auto recoveredPath=directory.filePath(QString::fromLatin1(recoveryFixture)+QStringLiteral(".recovered.docx"));
    QTimer recoveryDriver;
    QObject::connect(&recoveryDriver,&QTimer::timeout,&recovered,[&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog=qobject_cast<QFileDialog*>(widget);dialog && dialog->isVisible()) {
                dialog->selectFile(recoveredPath);
                QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
            }
            if (auto* message=qobject_cast<QMessageBox*>(widget);message && message->isVisible()) {
                if (message->windowTitle()==QStringLiteral("Recover documents?")) {
                    for (auto* button : message->buttons()) if (button->text()==QStringLiteral("Restore")) button->click();
                } else {
                    recoveryWarning=true;
                    std::cerr<<"Unexpected old-source recovery prompt: "<<message->text().toStdString()<<'\n'
                             <<message->detailedText().toStdString()<<'\n';
                    message->reject();
                }
            }
        }
    });
    recoveryDriver.start(0);
    QApplication::processEvents();
    auto* tabs=recovered.findChild<QTabWidget*>(QStringLiteral("documentTabs"));
    auto* recoveredCanvas=tabs ? qobject_cast<app::DocumentCanvas*>(tabs->currentWidget()) : nullptr;
    check(recoveredCanvas && recoveredCanvas->snapshot().document==expectedRecovery,
          "missing-source recovery lost old academic identities");
    const auto originalPackage=ooxml::DocxDocument::open(golden);
    check(originalPackage && recoveredCanvas->defaultFontFamily()==QString::fromStdString(originalPackage->sourceDefaults().font_family) &&
          recoveredCanvas->defaultFontPointSize()==originalPackage->sourceDefaults().font_size_half_points/2.0,
          "current insertion preferences replaced recovered display font defaults");
    recovered.findChild<QAction*>(QStringLiteral("file.saveAs"))->trigger();
    recoveryDriver.stop();
    check(!recoveryWarning && !recoveredCanvas->isModified(),"old source recovery prompted for conversion");
    check(member(golden,"word/styles.xml")==member(recoveredPath.toStdString(),"word/styles.xml"),
          "recovery changed the old document style catalog");
    const auto recoveredPackage=ooxml::DocxDocument::open(recoveredPath.toStdString());
    check(recoveredPackage && (std::string_view(recoveryFixture).find("0.6.1")!=std::string_view::npos
              ? recoveredPackage->sourceDefaults().neutral_style_defaults
              : recoveredPackage->sourceDefaults().legacy_style_defaults) && recoveredPackage->academicRecovery(),
          "recovery save lost old style profile or verified academic registry");
    }
    std::cout << "Legacy style desktop workflow tests passed\n";
}
