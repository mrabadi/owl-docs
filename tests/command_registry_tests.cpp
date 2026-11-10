#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/ChatDock.h"
#include "docxstudio/app/NavigationDock.h"
#include "docxstudio/app/RibbonWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPalette>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void testAvailableCommandSearch() {
    docxstudio::app::CommandRegistry commands;
    auto* available = commands.add("file.save", "Save", QKeySequence::Save,
                                   [] {});
    auto* disabled = commands.add("table.deleteRows", "Delete Rows",
                                  QKeySequence(), [] {});
    auto* hidden = commands.add("review.placeholder", "Coming soon",
                                QKeySequence(), [] {});
    disabled->setEnabled(false);
    hidden->setVisible(false);
    check(commands.search({}) == QStringList{available->objectName()},
          "command search exposes hidden or unavailable actions");
}

void testTaskSearchMetadata() {
    docxstudio::app::CommandRegistry commands;
    commands.add("file.exportPdf", "Export PDF", QKeySequence(), [] {});
    commands.describe("file.exportPdf", "File", {"publish", "share"});
    check(commands.search("publish") == QStringList{"file.exportPdf"} &&
              commands.category("file.exportPdf") == QStringLiteral("File"),
          "task keywords and command category are not discoverable");
}
}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir profile;
    check(profile.isValid(), "could not isolate workspace preferences");
    QCoreApplication::setOrganizationName("Owl Docs Workspace Tests");
    QCoreApplication::setApplicationName("Workspace");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       profile.path());
    testAvailableCommandSearch();
    testTaskSearchMetadata();
    return 0;
}
