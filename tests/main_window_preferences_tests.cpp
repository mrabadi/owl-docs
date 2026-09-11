#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorPreferences.h"
#include "docxstudio/app/FontFamilyPicker.h"
#include "docxstudio/app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using docxstudio::app::DocumentCanvas;
using docxstudio::app::EditorPreferences;
using docxstudio::app::MainWindow;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool near(double left, double right) {
    return std::abs(left - right) < 0.001;
}

void checkCanvasDefaults(const DocumentCanvas& canvas,
                         const QString& family, double points,
                         int tabWidth) {
    check(canvas.defaultFontFamily() == family,
          "canvas has the wrong default font family");
    check(near(canvas.defaultFontPointSize(), points),
          "canvas has the wrong default font size");
    check(canvas.tabWidthSpaces() == tabWidth,
          "canvas has the wrong tab width");
}

void testOptionsDialogAndPropagation(QString& selectedFamily) {
    MainWindow window;
    window.resize(1000, 760);
    window.show();
    QApplication::processEvents();

    auto* options = window.findChild<QAction*>(QStringLiteral("file.options"));
    auto* newDocument = window.findChild<QAction*>(QStringLiteral("file.new"));
    auto canvases = window.findChildren<DocumentCanvas*>();
    check(options && newDocument && canvases.size() == 1,
          "File Options or the initial document is missing");
    checkCanvasDefaults(*canvases.front(), QStringLiteral("Carlito"), 11.0, 4);

    bool cancelled = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("editorOptionsDialog"));
        check(dialog && dialog->isModal(),
              "Options did not open one modal editor-options dialog");
        auto* font = dialog->findChild<QFontComboBox*>(
            QStringLiteral("editorOptions.fontFamily"));
        auto* size = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("editorOptions.fontSize"));
        auto* tab = dialog->findChild<QSpinBox*>(
            QStringLiteral("editorOptions.tabWidth"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("editorOptions.buttons"));
        check(font && size && tab && buttons,
              "Options dialog is missing a required editor default");
        check(dynamic_cast<docxstudio::app::FontFamilyPicker*>(font),
              "Options dialog does not use the shared preview font picker");
        check(font->itemDelegate() &&
                  font->itemDelegate()->objectName() ==
                      QStringLiteral("fontFamilyNameOnlyDelegate"),
              "Options font picker still appends writing-system samples");
        check(near(size->value(), 11.0) && tab->value() == 4 &&
                  tab->minimum() == EditorPreferences::kMinimumTabWidthSpaces &&
                  tab->maximum() == EditorPreferences::kMaximumTabWidthSpaces,
              "Options dialog does not expose the built-in defaults and bounds");
        size->setValue(18.0);
        tab->setValue(9);
        cancelled = true;
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    options->trigger();
    check(cancelled, "Options cancel path was not exercised");
    checkCanvasDefaults(*canvases.front(), QStringLiteral("Carlito"), 11.0, 4);

    bool accepted = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(
            QStringLiteral("editorOptionsDialog"));
        check(dialog != nullptr, "accepted Options dialog was not found");
        auto* font = dialog->findChild<QFontComboBox*>(
            QStringLiteral("editorOptions.fontFamily"));
        auto* size = dialog->findChild<QDoubleSpinBox*>(
            QStringLiteral("editorOptions.fontSize"));
        auto* tab = dialog->findChild<QSpinBox*>(
            QStringLiteral("editorOptions.tabWidth"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("editorOptions.buttons"));
        check(font && size && tab && buttons,
              "accepted Options dialog is incomplete");
        font->setCurrentFont(QFont(QStringLiteral("DejaVu Sans")));
        selectedFamily = font->currentFont().family();
        check(!selectedFamily.isEmpty(),
              "font chooser did not resolve a selectable family");
        size->setValue(13.5);
        tab->setValue(7);
        accepted = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    options->trigger();
    check(accepted, "Options accept path was not exercised");

    canvases = window.findChildren<DocumentCanvas*>();
    check(canvases.size() == 1, "Options unexpectedly changed the tab count");
    checkCanvasDefaults(*canvases.front(), selectedFamily, 13.5, 7);
    check(!canvases.front()->isModified(),
          "changing application defaults dirtied the document");

    newDocument->trigger();
    canvases = window.findChildren<DocumentCanvas*>();
    check(canvases.size() == 2,
          "New did not create a second document for preference propagation");
    for (const auto* canvas : canvases) {
        checkCanvasDefaults(*canvas, selectedFamily, 13.5, 7);
    }
}

void testPreferencesReload(const QString& selectedFamily) {
    MainWindow window;
    const auto canvases = window.findChildren<DocumentCanvas*>();
    check(canvases.size() == 1,
          "reopened window did not create its initial document");
    checkCanvasDefaults(*canvases.front(), selectedFamily, 13.5, 7);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Owl Docs Main Window Preferences Tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QTemporaryDir settingsDirectory;
    check(settingsDirectory.isValid(),
          "could not create an isolated preferences directory");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());

    QString selectedFamily;
    testOptionsDialogAndPropagation(selectedFamily);
    QApplication::processEvents();
    testPreferencesReload(selectedFamily);

    std::cout << "main-window preference tests passed\n";
    return 0;
}
