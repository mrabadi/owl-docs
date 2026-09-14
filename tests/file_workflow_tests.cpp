#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/ooxml/docx_document.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using docxstudio::app::DocumentCanvas;
using docxstudio::app::MainWindow;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void resetSettings() {
    QSettings settings;
    settings.clear();
    settings.sync();
    check(settings.status() == QSettings::NoError,
          "could not reset isolated file-workflow settings");
}

std::filesystem::path nativePath(const QString& path) {
    const QByteArray encoded = QFile::encodeName(path);
    return std::filesystem::path(encoded.constData());
}

void createDocx(const QString& path, const std::string& text) {
    docxstudio::ooxml::NewParagraph paragraph;
    paragraph.runs.emplace_back(text, docxstudio::ooxml::BasicRunFormat{});
    const auto saved = docxstudio::ooxml::DocxDocument::writeNew(
        nativePath(path), {paragraph});
    check(saved.saved, "could not create file-workflow DOCX fixture");
}

void createTextFile(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "could not create non-DOCX ingress fixture");
    check(file.write("not a document\n") > 0,
          "could not write non-DOCX ingress fixture");
}

QTabWidget* documentTabs(MainWindow& window) {
    return window.findChild<QTabWidget*>(QStringLiteral("documentTabs"));
}

QMenu* recentMenu(MainWindow& window) {
    return window.findChild<QMenu*>(QStringLiteral("file.openRecent"));
}

QAction* menuAction(QMenu& menu, const QString& objectName) {
    for (auto* action : menu.actions()) {
        if (action->objectName() == objectName) return action;
    }
    return nullptr;
}

QList<QAction*> recentOpenActions(QMenu& menu) {
    QList<QAction*> result;
    for (auto* action : menu.actions()) {
        if (action->objectName().startsWith(QStringLiteral("recent.open."))) {
            result.push_back(action);
        }
    }
    return result;
}

QString canvasText(const DocumentCanvas& canvas) {
    const auto snapshot = canvas.snapshot();
    QString result;
    for (const auto& paragraph : snapshot.document.paragraphs()) {
        if (!result.isEmpty()) result += QLatin1Char('\n');
        const auto& text = paragraph.text();
        result += QString::fromUtf16(text.data(),
                                     static_cast<qsizetype>(text.size()));
    }
    return result;
}

QStringList storedRecentFiles() {
    QSettings settings;
    settings.sync();
    return settings.value(QStringLiteral("recentFiles")).toStringList();
}

QString canonicalPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool isVisiblyNumbered(const QString& text, int number) {
    const QRegularExpression prefix(
        QStringLiteral("^&?%1(?:\\D|$)").arg(number));
    return prefix.match(text).hasMatch();
}

class UnexpectedModalGuard final {
public:
    UnexpectedModalGuard() {
        timer_.setInterval(1);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* message = qobject_cast<QMessageBox*>(widget);
                if (!message || !message->isVisible()) continue;
                encountered_ = true;
                message->reject();
            }
        });
        timer_.start();
    }

    ~UnexpectedModalGuard() { timer_.stop(); }

    [[nodiscard]] bool encountered() const { return encountered_; }

private:
    QTimer timer_;
    bool encountered_{false};
};

void testOpenPlaceholderFailureAndCanonicalDeduplication(
    const QTemporaryDir& temporary) {
    resetSettings();

    const QString first = temporary.filePath(QStringLiteral("first.docx"));
    const QString second = temporary.filePath(QStringLiteral("second.docx"));
    const QString alias = temporary.filePath(QStringLiteral("first-alias.docx"));
    const QString invalid = temporary.filePath(QStringLiteral("notes.txt"));
    const QString corrupt = temporary.filePath(QStringLiteral("corrupt.docx"));
    createDocx(first, "First document");
    createDocx(second, "Second document");
    createTextFile(invalid);
    createTextFile(corrupt);
    check(QFile::link(first, alias),
          "could not create canonical-path symlink fixture");

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();

    auto* tabs = documentTabs(window);
    check(tabs && tabs->count() == 1,
          "main window is missing its named pristine document tab");
    auto* pristine = qobject_cast<DocumentCanvas*>(tabs->currentWidget());
    check(pristine && tabs->tabText(0) == QStringLiteral("Untitled"),
          "initial document is not a pristine Untitled placeholder");

    {
        UnexpectedModalGuard modalGuard;
        check(window.openPaths({invalid}) == 0,
              "invalid extension unexpectedly counted as an opened document");
        QApplication::processEvents();
        check(!modalGuard.encountered(),
              "invalid extension showed a blocking import error");
    }
    check(tabs->count() == 1 && tabs->currentWidget() == pristine &&
              tabs->tabText(0) == QStringLiteral("Untitled"),
          "failed open replaced or disturbed the pristine placeholder");
    check(storedRecentFiles().isEmpty(),
          "failed open was added to recent documents");

    {
        UnexpectedModalGuard modalGuard;
        check(!window.openPath(corrupt),
              "corrupt DOCX unexpectedly opened successfully");
        QApplication::processEvents();
        check(modalGuard.encountered(),
              "corrupt DOCX did not provide an explicit open error");
    }
    check(tabs->count() == 1 && tabs->currentWidget() == pristine &&
              storedRecentFiles().isEmpty(),
          "failed corrupt-DOCX open disturbed the placeholder or recents");

    check(window.openPath(first), "could not open first DOCX fixture");
    QApplication::processEvents();
    check(tabs->count() == 1 &&
              tabs->tabText(0) == QFileInfo(first).fileName(),
          "successful open did not replace the pristine Untitled placeholder");
    auto* firstCanvas = qobject_cast<DocumentCanvas*>(tabs->currentWidget());
    check(firstCanvas && canvasText(*firstCanvas) == QStringLiteral("First document"),
          "replacement tab does not contain the opened document");
    check(storedRecentFiles() == QStringList{canonicalPath(first)},
          "successful open did not add one canonical recent-file entry");

    check(window.openPath(second), "could not open second DOCX fixture");
    QApplication::processEvents();
    check(tabs->count() == 2 && tabs->currentWidget() != firstCanvas,
          "second distinct document did not receive its own tab");

    check(window.openPath(alias),
          "opening a symlink alias did not activate its existing document");
    QApplication::processEvents();
    check(tabs->count() == 2 && tabs->currentWidget() == firstCanvas,
          "canonical duplicate created a tab instead of activating the existing one");
    const auto recent = storedRecentFiles();
    check(recent.size() == 2 && recent.front() == canonicalPath(first) &&
              recent.back() == canonicalPath(second),
          "canonical duplicate was retained as a separate recent-file identity");
}

void sendDrop(MainWindow& window, const QList<QUrl>& urls,
              bool shouldAccept,
              Qt::DropActions possibleActions = Qt::CopyAction) {
    QMimeData mime;
    mime.setUrls(urls);
    const QPoint position = window.rect().center();

    QDragEnterEvent enter(position, possibleActions, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&window, &enter);
    check(enter.isAccepted() == shouldAccept,
          shouldAccept ? "valid local DOCX drag was not accepted"
                       : "invalid or remote drag was accepted");
    if (shouldAccept) {
        check(enter.dropAction() == Qt::CopyAction,
              "accepted document drag did not force a safe copy action");
    }

    QDropEvent drop(QPointF(position), possibleActions, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&window, &drop);
    check(drop.isAccepted() == shouldAccept,
          shouldAccept ? "valid local DOCX drop was not accepted"
                       : "invalid or remote drop was accepted");
    if (shouldAccept) {
        check(drop.dropAction() == Qt::CopyAction,
              "accepted document drop did not force a safe copy action");
    }
    QApplication::processEvents();
}

void testMultipleDocumentDropAndInvalidDropRejection(
    const QTemporaryDir& temporary) {
    resetSettings();

    const QString first = temporary.filePath(QStringLiteral("drop-one.docx"));
    const QString second = temporary.filePath(QStringLiteral("drop-two.DOCX"));
    const QString alias = temporary.filePath(QStringLiteral("drop-one-alias.docx"));
    const QString text = temporary.filePath(QStringLiteral("drop-notes.txt"));
    const QString corrupt =
        temporary.filePath(QStringLiteral("drop-corrupt.docx"));
    createDocx(first, "Drop one");
    createDocx(second, "Drop two");
    createTextFile(text);
    createTextFile(corrupt);
    check(QFile::link(first, alias),
          "could not create dropped canonical-path symlink fixture");

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    auto* tabs = documentTabs(window);
    check(tabs && tabs->count() == 1,
          "drop test did not start with one pristine tab");

    const QList<QUrl> mixed{
        QUrl::fromLocalFile(first),
        QUrl::fromLocalFile(first),
        QUrl::fromLocalFile(alias),
        QUrl(QStringLiteral("https://example.invalid/remote.docx")),
        QUrl::fromLocalFile(text),
        QUrl::fromLocalFile(second),
    };
    {
        UnexpectedModalGuard modalGuard;
        sendDrop(window, mixed, true);
        check(!modalGuard.encountered(),
              "mixed drop showed an error for an ignored URL");
    }

    check(tabs->count() == 2,
          "multi-DOCX drop did not open each canonical local document once");
    QSet<QString> openedText;
    for (int index = 0; index < tabs->count(); ++index) {
        auto* canvas = qobject_cast<DocumentCanvas*>(tabs->widget(index));
        check(canvas, "document tab is not a DocumentCanvas");
        openedText.insert(canvasText(*canvas));
    }
    check(openedText == QSet<QString>{QStringLiteral("Drop one"),
                                     QStringLiteral("Drop two")},
          "multi-DOCX drop opened the wrong set of documents");

    const auto recent = storedRecentFiles();
    check(recent.size() == 2 &&
              QSet<QString>(recent.cbegin(), recent.cend()) ==
                  QSet<QString>{canonicalPath(first), canonicalPath(second)},
          "drop retained invalid or duplicate recent-file entries");

    const int tabCount = tabs->count();
    const auto recentBeforeInvalidDrops = storedRecentFiles();
    {
        UnexpectedModalGuard modalGuard;
        sendDrop(
            window,
            {QUrl(QStringLiteral(
                "https://example.invalid/only-remote.docx"))},
            false);
        QUrl remoteFileUrl;
        remoteFileUrl.setScheme(QStringLiteral("file"));
        remoteFileUrl.setHost(QStringLiteral("server.invalid"));
        remoteFileUrl.setPath(first);
        sendDrop(window, {remoteFileUrl}, false);
        sendDrop(window, {QUrl::fromLocalFile(text)}, false);
        sendDrop(window, {QUrl::fromLocalFile(first)}, false,
                 Qt::MoveAction);
        check(!modalGuard.encountered(),
              "rejected remote, non-DOCX, or move-only drop showed a blocking error");
    }
    check(tabs->count() == tabCount &&
              storedRecentFiles() == recentBeforeInvalidDrops,
          "remote, non-DOCX, or move-only drop changed tabs or recents");

    QMimeData corruptMime;
    corruptMime.setUrls({QUrl::fromLocalFile(corrupt)});
    const QPoint position = window.rect().center();
    QDragEnterEvent corruptEnter(position, Qt::CopyAction, &corruptMime,
                                 Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &corruptEnter);
    check(corruptEnter.isAccepted(),
          "syntactically valid DOCX drag was rejected before safe parsing");
    {
        UnexpectedModalGuard modalGuard;
        QDropEvent corruptDrop(QPointF(position), Qt::CopyAction,
                               &corruptMime, Qt::LeftButton,
                               Qt::NoModifier);
        QApplication::sendEvent(&window, &corruptDrop);
        check(!corruptDrop.isAccepted(),
              "drop was reported successful when its DOCX could not open");
        check(modalGuard.encountered(),
              "malformed dropped DOCX did not provide an open error");
    }
    check(tabs->count() == tabCount &&
              storedRecentFiles() == recentBeforeInvalidDrops,
          "failed malformed-DOCX drop changed tabs or recents");
}

void testOpenDialogAllowsMultipleDocuments() {
    resetSettings();
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();

    auto* open = window.findChild<QAction*>(QStringLiteral("file.open"));
    check(open, "main window is missing the Open command");
    bool inspected = false;
    QTimer dialogMonitor;
    dialogMonitor.setInterval(1);
    QObject::connect(&dialogMonitor, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QFileDialog*>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            inspected = dialog->fileMode() == QFileDialog::ExistingFiles;
            dialog->reject();
        }
    });
    dialogMonitor.start();
    open->trigger();
    dialogMonitor.stop();
    check(inspected,
          "File > Open did not use a multi-document selection dialog");
}

void testSymlinkFirstOpenSavesTheTarget(const QTemporaryDir& temporary) {
    resetSettings();

    const QString target =
        temporary.filePath(QStringLiteral("symlink-save-target.bin"));
    const QString alias =
        temporary.filePath(QStringLiteral("symlink-save-alias.docx"));
    createDocx(target, "Original text");
    check(QFile::link(target, alias),
          "could not create symlink-first save fixture");

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    check(window.openPath(alias), "could not open DOCX through a symlink");
    QApplication::processEvents();

    auto* tabs = documentTabs(window);
    auto* canvas = tabs ? qobject_cast<DocumentCanvas*>(tabs->currentWidget())
                        : nullptr;
    check(canvas && tabs->count() == 1 &&
              canvasText(*canvas) == QStringLiteral("Original text"),
          "symlink-first open did not load the target document");
    check(storedRecentFiles() == QStringList{canonicalPath(target)},
          "symlink-first open did not use the target as its file identity");

    canvas->selectAll();
    canvas->insertText(QStringLiteral("Modified text"));
    auto* save = window.findChild<QAction*>(QStringLiteral("file.save"));
    check(save && canvas->isModified(),
          "could not prepare symlink-first save regression edit");
    {
        UnexpectedModalGuard modalGuard;
        save->trigger();
        QApplication::processEvents();
        check(!modalGuard.encountered(),
              "safe symlink-target save unexpectedly showed a warning");
    }
    check(!canvas->isModified(),
          "symlink-target edit was not saved");
    check(QFileInfo(alias).isSymLink() &&
              canonicalPath(alias) == canonicalPath(target),
          "saving a symlink-opened document replaced or retargeted the symlink");

    docxstudio::ooxml::Error error;
    auto saved = docxstudio::ooxml::DocxDocument::open(nativePath(target),
                                                       &error);
    check(saved && !saved->paragraphs().empty() &&
              saved->paragraphs().front().plainText() == "Modified text",
          "saving a symlink-opened document did not update its target");
}

void testRecentMenuPresentationAndMaintenance(
    const QTemporaryDir& temporary) {
    resetSettings();

    const QString northDirectory =
        temporary.filePath(QStringLiteral("north-folder"));
    const QString southDirectory =
        temporary.filePath(QStringLiteral("south-folder"));
    check(QDir().mkpath(northDirectory) && QDir().mkpath(southDirectory),
          "could not create duplicate-basename fixture directories");
    const QString north =
        QDir(northDirectory).filePath(QStringLiteral("report.docx"));
    const QString south =
        QDir(southDirectory).filePath(QStringLiteral("report.docx"));
    const QString distinct =
        temporary.filePath(QStringLiteral("summary.docx"));
    const QString staleOnOpen =
        temporary.filePath(QStringLiteral("missing-on-open.docx"));
    const QString staleOnCleanup =
        temporary.filePath(QStringLiteral("missing-on-cleanup.docx"));
    createDocx(north, "North report");
    createDocx(south, "South report");
    createDocx(distinct, "Summary");

    const QStringList seeded{QFileInfo(north).absoluteFilePath(),
                             QFileInfo(south).absoluteFilePath(),
                             QFileInfo(staleOnOpen).absoluteFilePath(),
                             QFileInfo(distinct).absoluteFilePath(),
                             QFileInfo(staleOnCleanup).absoluteFilePath()};
    {
        QSettings settings;
        settings.setValue(QStringLiteral("recentFiles"), seeded);
        settings.sync();
        check(settings.status() == QSettings::NoError,
              "could not seed recent-document settings");
    }

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
    auto* tabs = documentTabs(window);
    auto* menu = recentMenu(window);
    check(tabs && tabs->count() == 1 && menu,
          "main window is missing file-workflow menu hooks");

    const auto entries = recentOpenActions(*menu);
    check(entries.size() == seeded.size(),
          "recent menu did not present all persisted entries");
    for (int index = 0; index < entries.size(); ++index) {
        auto* action = entries[index];
        const QString expectedName =
            QStringLiteral("recent.open.%1").arg(index + 1);
        check(action->objectName() == expectedName,
              "recent entry has an unstable or incorrectly numbered object name");
        check(isVisiblyNumbered(action->text(), index + 1),
              "recent entry has no visible ordinal");
        check(action->data().toString() == seeded[index],
              "recent entry does not carry its full path as action data");
        check(action->toolTip() == seeded[index],
              "recent entry tooltip is not its full absolute path");
    }
    check(entries[0]->text() != entries[1]->text() &&
              entries[0]->text().contains(QStringLiteral("north-folder")) &&
              entries[1]->text().contains(QStringLiteral("south-folder")),
          "same-basename recent entries are not visibly disambiguated");

    {
        UnexpectedModalGuard modalGuard;
        entries[2]->trigger();
        QApplication::processEvents();
        check(!modalGuard.encountered(),
              "opening a stale recent entry showed a blocking error");
    }
    const QStringList afterStaleOpen{seeded[0], seeded[1], seeded[3],
                                     seeded[4]};
    check(tabs->count() == 1 && storedRecentFiles() == afterStaleOpen,
          "opening a stale recent entry did not remove only that entry");

    auto* removeMissing =
        menuAction(*menu, QStringLiteral("recent.removeMissing"));
    check(removeMissing && removeMissing->isEnabled(),
          "recent menu does not offer removal of stale entries");
    removeMissing->trigger();
    QApplication::processEvents();
    check(tabs->count() == 1,
          "removing stale recent entries unexpectedly opened a document");
    const QStringList afterRemoval{seeded[0], seeded[1], seeded[3]};
    check(storedRecentFiles() == afterRemoval,
          "Remove Missing did not remove only the stale recent entry");
    const auto renumbered = recentOpenActions(*menu);
    check(renumbered.size() == afterRemoval.size(),
          "recent menu was not rebuilt after removing stale entries");
    for (int index = 0; index < renumbered.size(); ++index) {
        check(renumbered[index]->objectName() ==
                  QStringLiteral("recent.open.%1").arg(index + 1) &&
                  isVisiblyNumbered(renumbered[index]->text(), index + 1),
              "recent entries were not renumbered after stale removal");
    }

    auto* refreshedRemoveMissing =
        menuAction(*menu, QStringLiteral("recent.removeMissing"));
    check(refreshedRemoveMissing && !refreshedRemoveMissing->isEnabled(),
          "Remove Missing remained enabled after stale entries were removed");
    check(QFile::remove(distinct),
          "could not make a recent document stale after menu construction");
    check(QMetaObject::invokeMethod(menu, "aboutToShow",
                                    Qt::DirectConnection),
          "could not refresh the recent menu before showing it");
    refreshedRemoveMissing =
        menuAction(*menu, QStringLiteral("recent.removeMissing"));
    check(refreshedRemoveMissing && refreshedRemoveMissing->isEnabled(),
          "recent menu did not notice a file deleted while the app was open");
    refreshedRemoveMissing->trigger();
    QApplication::processEvents();
    check(storedRecentFiles() == QStringList{seeded[0], seeded[1]},
          "refreshed Remove Missing did not remove the newly stale entry");

    check(menu->toolTipsVisible(),
          "recent menu does not expose its full-path tooltips");

    auto* clear = menuAction(*menu, QStringLiteral("recent.clear"));
    check(clear && clear->isEnabled(),
          "recent menu does not offer Clear Recent");
    clear->trigger();
    QApplication::processEvents();
    check(storedRecentFiles().isEmpty(),
          "Clear Recent did not clear persisted recent files");
    check(recentOpenActions(*menu).isEmpty(),
          "Clear Recent left document entries in the menu");
    check(tabs->count() == 1,
          "Clear Recent unexpectedly changed open documents");

    QStringList tenEntries;
    for (int index = 0; index < 10; ++index) {
        tenEntries.push_back(temporary.filePath(
            QStringLiteral("ordinal-%1.docx").arg(index + 1)));
    }
    {
        QSettings settings;
        settings.setValue(QStringLiteral("recentFiles"), tenEntries);
        settings.sync();
    }
    check(QMetaObject::invokeMethod(menu, "aboutToShow",
                                    Qt::DirectConnection),
          "could not rebuild ten-entry recent menu");
    auto* tenth = menuAction(*menu, QStringLiteral("recent.open.10"));
    check(tenth && tenth->text().startsWith(QStringLiteral("10 ")) &&
              !tenth->text().startsWith(QStringLiteral("&10")),
          "tenth recent entry reuses the first entry's mnemonic");
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir stateDirectory(
        QStringLiteral("/tmp/owl-docs-file-workflow-state-XXXXXX"));
    check(stateDirectory.isValid(),
          "could not create isolated file-workflow state directory");
    qputenv("XDG_DATA_HOME", stateDirectory.path().toUtf8());

    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Owl Docs File Workflow Tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QTemporaryDir settingsDirectory;
    QTemporaryDir fixtures;
    check(settingsDirectory.isValid() && fixtures.isValid(),
          "could not create isolated file-workflow directories");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());

    testOpenPlaceholderFailureAndCanonicalDeduplication(fixtures);
    QApplication::processEvents();
    testMultipleDocumentDropAndInvalidDropRejection(fixtures);
    QApplication::processEvents();
    testOpenDialogAllowsMultipleDocuments();
    QApplication::processEvents();
    testSymlinkFirstOpenSavesTheTarget(fixtures);
    QApplication::processEvents();
    testRecentMenuPresentationAndMaintenance(fixtures);

    std::cout << "file workflow tests passed\n";
    return 0;
}
