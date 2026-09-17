#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/EditorPreferences.h"
#include "docxstudio/app/FontFamilyPicker.h"
#include "docxstudio/app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

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

    auto* zoomSlider = window.findChild<QSlider*>(
        QStringLiteral("status.zoomSlider"));
    auto* zoomLabel = window.findChild<QLabel*>(
        QStringLiteral("status.zoomPercent"));
    auto* zoomOut = window.findChild<QToolButton*>(
        QStringLiteral("status.zoomOut"));
    auto* zoomIn = window.findChild<QToolButton*>(
        QStringLiteral("status.zoomIn"));
    auto* ribbonZoom = window.findChild<QSpinBox*>(
        QStringLiteral("ribbon.zoom"));
    check(zoomSlider && zoomLabel && zoomOut && zoomIn && ribbonZoom,
          "window is missing synchronized zoom controls");
    check(zoomSlider->minimum() == DocumentCanvas::kMinimumZoomPercent &&
              zoomSlider->maximum() == DocumentCanvas::kMaximumZoomPercent &&
              zoomSlider->value() == 100 && zoomLabel->text() == QStringLiteral("100%"),
          "status zoom control has the wrong initial range or value");
    check(ribbonZoom->minimum() == DocumentCanvas::kMinimumZoomPercent &&
              ribbonZoom->maximum() == DocumentCanvas::kMaximumZoomPercent &&
              ribbonZoom->value() == 100,
          "ribbon zoom control disagrees with the document zoom range");
    check(!zoomSlider->accessibleName().isEmpty() &&
              !zoomOut->accessibleName().isEmpty() &&
              !zoomIn->accessibleName().isEmpty(),
          "zoom controls are missing accessible names");

    const auto initialRevision = canvases.front()->snapshot().revision;
    zoomSlider->setValue(DocumentCanvas::kMaximumZoomPercent);
    QApplication::processEvents();
    check(canvases.front()->zoomPercent() ==
              DocumentCanvas::kMaximumZoomPercent &&
              zoomLabel->text() == QStringLiteral("400%") &&
              ribbonZoom->value() == DocumentCanvas::kMaximumZoomPercent &&
              !zoomIn->isEnabled(),
          "400 percent status zoom did not synchronize the active document");
    check(canvases.front()->snapshot().revision == initialRevision &&
              !canvases.front()->isModified(),
          "view zoom changed document content or dirty state");
    zoomOut->click();
    check(canvases.front()->zoomPercent() == 390 && zoomIn->isEnabled(),
          "status zoom-out button did not move by one step");
    zoomSlider->setValue(100);

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

    // Zoom is view state owned by each document tab. A new document starts at
    // 100%, and returning to the first tab restores its prior zoom controls.
    canvases.front()->setZoomPercent(175);
    newDocument->trigger();
    canvases = window.findChildren<DocumentCanvas*>();
    check(canvases.size() == 2,
          "New did not create a second document for preference propagation");
    for (const auto* canvas : canvases) {
        checkCanvasDefaults(*canvas, selectedFamily, 13.5, 7);
    }
    DocumentCanvas* active = nullptr;
    QTabWidget* documentTabs = nullptr;
    for (auto* candidate : window.findChildren<QTabWidget*>()) {
        if (auto* canvas = qobject_cast<DocumentCanvas*>(candidate->currentWidget())) {
            active = canvas;
            documentTabs = candidate;
            break;
        }
    }
    check(active && documentTabs && active != canvases.front() &&
              active->zoomPercent() == 100 && zoomSlider->value() == 100,
          "new document did not receive independent default zoom state");
    zoomSlider->setValue(400);
    check(active->zoomPercent() == 400,
          "status slider did not update the second document");
    documentTabs->setCurrentWidget(canvases.front());
    QApplication::processEvents();
    check(zoomSlider->value() == 175 && zoomLabel->text() == QStringLiteral("175%") &&
              ribbonZoom->value() == 175,
          "switching tabs did not restore the document-specific zoom controls");
}

void testPreferencesReload(const QString& selectedFamily) {
    MainWindow window;
    const auto canvases = window.findChildren<DocumentCanvas*>();
    check(canvases.size() == 1,
          "reopened window did not create its initial document");
    checkCanvasDefaults(*canvases.front(), selectedFamily, 13.5, 7);

    canvases.front()->insertText(QStringLiteral("Configured body"));
    canvases.front()->applyParagraphStyle(QStringLiteral("Heading1"));
    canvases.front()->applyParagraphStyle(QStringLiteral("Normal"));
    const auto normalFormat = canvases.front()->snapshot().document
                                  .paragraphs().front().characterFormatAt(1);
    check(normalFormat.font_family == selectedFamily.toStdString() &&
              normalFormat.font_size_half_points == 27,
          "Normal style did not resolve the saved editor font and size defaults");

    auto* stylePicker = window.findChild<QComboBox*>(
        QStringLiteral("ribbon.paragraphStyle"));
    check(stylePicker && stylePicker->isEnabled(),
          "paragraph-style picker is unavailable in a normal body paragraph");
    check(canvases.front()->insertTable(1, 1, false),
          "could not enter a table context for paragraph-style testing");
    QApplication::processEvents();
    check(!canvases.front()->paragraphStylesAvailable() &&
              !stylePicker->isEnabled() &&
              stylePicker->currentText().contains(QStringLiteral("tables")),
          "active table cell did not explicitly disable the paragraph-style picker");
    const auto tableId =
        canvases.front()->snapshot().document.tables().front().id();
    check(canvases.front()->selectTableCells(tableId, 0, 0, 0, 0),
          "could not create a rectangular table-cell selection");
    QApplication::processEvents();
    check(!canvases.front()->paragraphStylesAvailable() &&
              !stylePicker->isEnabled(),
          "rectangular table-cell selection left paragraph styles actionable");
    check(canvases.front()->selectTable(tableId),
          "could not select the whole table object");
    QApplication::processEvents();
    check(!canvases.front()->paragraphStylesAvailable() &&
              !stylePicker->isEnabled(),
          "whole-table selection left paragraph styles actionable");

    canvases.front()->undo();
    QApplication::processEvents();
    check(canvases.front()->paragraphStylesAvailable() &&
              stylePicker->isEnabled(),
          "leaving a table did not re-enable the paragraph-style picker");

    QString previewSummary;
    QString previewError;
    check(canvases.front()->createReplacementPreview(
              QStringLiteral("preview"), previewSummary, previewError),
          "could not create a preview for paragraph-style availability testing");
    QApplication::processEvents();
    check(!canvases.front()->paragraphStylesAvailable() &&
              !stylePicker->isEnabled() &&
              stylePicker->currentText().contains(QStringLiteral("preview")),
          "active preview did not explicitly disable the paragraph-style picker");
    canvases.front()->discardPreview();
    QApplication::processEvents();
    check(canvases.front()->paragraphStylesAvailable() &&
              stylePicker->isEnabled(),
          "discarding a preview did not restore paragraph-style availability");
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
