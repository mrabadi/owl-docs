#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/DocumentCanvas.h"
#include "docxstudio/app/ListPropertiesDialog.h"
#include "docxstudio/app/MainWindow.h"
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/SpellChecker.h"

#include <QApplication>
#include <QAction>
#include <QDialogButtonBox>
#include <QContextMenuEvent>
#include <QLabel>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

int tabIndex(const QTabWidget& tabs, const QString& label) {
    for (int index = 0; index < tabs.count(); ++index) {
        if (tabs.tabText(index) == label) return index;
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Owl Docs Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("Owl Docs List Properties Tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settingsDirectory;
    check(settingsDirectory.isValid(),
          "could not create isolated list-property settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());

    using docxstudio::app::ListPropertiesDialog;
    ListPropertiesDialog dialog;
    const auto defaults = dialog.properties();
    for (int index = 0; index < ListPropertiesDialog::kLevelCount; ++index) {
        const auto& level = defaults[static_cast<std::size_t>(index)];
        check(level.bulletPositionSpaces == index * 4,
              "default bullet positions are not 0, 4, ... 36 spaces");
        check(level.textGapAfterBulletSpaces == 2,
              "default text-after-bullet gap is not two spaces");
    }

    auto custom = defaults;
    custom[0] = {3, 2};
    custom[9] = {51, 7};
    dialog.setProperties(custom);
    check(dialog.properties() == custom,
          "list properties dialog did not round-trip edited values");

    auto* levelOnePosition = dialog.findChild<QSpinBox*>(
        QStringLiteral("listProperties.bulletPosition.level1"));
    auto* levelTenGap = dialog.findChild<QSpinBox*>(
        QStringLiteral("listProperties.textGap.level10"));
    check(levelOnePosition && levelTenGap,
          "list-property controls do not expose stable object names");
    check(!levelOnePosition->accessibleName().isEmpty() &&
              !levelTenGap->accessibleName().isEmpty(),
          "list-property controls are missing accessible names");

    auto* buttons = dialog.findChild<QDialogButtonBox*>(
        QStringLiteral("listProperties.buttons"));
    check(buttons && buttons->button(QDialogButtonBox::Reset),
          "list properties dialog has no Reset Defaults button");
    buttons->button(QDialogButtonBox::Reset)->click();
    check(dialog.properties() == ListPropertiesDialog::defaultProperties(),
          "Reset Defaults did not restore all ten list levels");

    auto storedDefaults = ListPropertiesDialog::defaultProperties();
    storedDefaults[0] = {6, 3};
    dialog.setDefaultProperties(storedDefaults);
    dialog.setProperties(custom);
    buttons->button(QDialogButtonBox::Reset)->click();
    check(dialog.properties() == storedDefaults,
          "Reset Defaults did not restore the current saved list defaults");

    ListPropertiesDialog defaultActionDialog;
    auto* useAsDefaults = defaultActionDialog.findChild<QPushButton*>(
        QStringLiteral("listProperties.useAsDefaults"));
    check(useAsDefaults && !useAsDefaults->accessibleName().isEmpty() &&
              !useAsDefaults->toolTip().isEmpty(),
          "list properties dialog has no accessible defaults action");
    check(!defaultActionDialog.useAsDefaultsRequested(),
          "list defaults were requested before the button was used");
    useAsDefaults->click();
    check(defaultActionDialog.useAsDefaultsRequested() &&
              defaultActionDialog.result() == QDialog::Accepted,
          "Use as Defaults did not accept and identify the requested action");

    docxstudio::app::CommandRegistry commands;
    for (const auto* id : {"paragraph.bullets", "paragraph.numbering",
                           "paragraph.decreaseIndent",
                           "paragraph.increaseIndent"}) {
        commands.add(QString::fromLatin1(id), QString::fromLatin1(id),
                     QKeySequence(), [] {});
    }
    docxstudio::app::RibbonWidget ribbon(commands);
    auto* tabs = ribbon.findChild<QTabWidget*>(QStringLiteral("ribbon.tabs"));
    check(tabs != nullptr, "ribbon tab widget is not discoverable");
    const int listTab = tabIndex(*tabs, QStringLiteral("List"));
    check(listTab >= 0 && !tabs->isTabVisible(listTab),
          "contextual List tab is visible outside a list");

    ribbon.setListContext(true, 7);
    check(tabs->isTabVisible(listTab),
          "contextual List tab did not appear inside a list");
    auto* levelLabel = ribbon.findChild<QLabel*>(
        QStringLiteral("ribbon.listLevel"));
    check(levelLabel && levelLabel->text() == QStringLiteral("Level 7"),
          "contextual List tab did not show the active list level");

    bool propertiesRequested = false;
    QObject::connect(&ribbon,
                     &docxstudio::app::RibbonWidget::listPropertiesRequested,
                     [&propertiesRequested] { propertiesRequested = true; });
    auto* propertiesButton = ribbon.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.paragraph.listProperties"));
    check(propertiesButton && propertiesButton->isEnabled() &&
              !propertiesButton->icon().isNull(),
          "contextual List tab has no usable properties button");
    propertiesButton->click();
    check(propertiesRequested,
          "List Properties ribbon button did not emit its request");

    tabs->setCurrentIndex(listTab);
    ribbon.setListContext(false, 1);
    check(!tabs->isTabVisible(listTab) && tabs->currentIndex() == 0,
          "hiding an active List tab did not return to Home");

    docxstudio::app::MainWindow window;
    window.resize(1000, 760);
    window.show();
    QApplication::processEvents();
    auto* canvas = window.findChild<docxstudio::app::DocumentCanvas*>();
    auto* windowTabs = window.findChild<QTabWidget*>(
        QStringLiteral("ribbon.tabs"));
    check(canvas && windowTabs,
          "main-window list integration has no canvas or ribbon");
    const int windowListTab = tabIndex(*windowTabs, QStringLiteral("List"));
    canvas->toggleBullets();
    canvas->insertText(QStringLiteral("item"));
    QApplication::processEvents();
    check(windowListTab >= 0 && windowTabs->isTabVisible(windowListTab),
          "entering a real list did not reveal the contextual List tab");

    bool integratedDialogAccepted = false;
    QTimer::singleShot(0, &window, [&] {
        auto* liveDialog = window.findChild<ListPropertiesDialog*>(
            QStringLiteral("listPropertiesDialog"));
        check(liveDialog != nullptr,
              "the ribbon did not open the integrated List Properties dialog");
        auto changed = liveDialog->properties();
        changed[0] = {5, 3};
        liveDialog->setProperties(changed);
        integratedDialogAccepted = true;
        auto* defaultsButton = liveDialog->findChild<QPushButton*>(
            QStringLiteral("listProperties.useAsDefaults"));
        check(defaultsButton != nullptr,
              "integrated List Properties has no defaults button");
        defaultsButton->click();
    });
    auto* integratedButton = window.findChild<QToolButton*>(
        QStringLiteral("ribbonButton.paragraph.listProperties"));
    check(integratedButton != nullptr,
          "main window has no contextual List Properties button");
    integratedButton->click();
    check(integratedDialogAccepted,
          "integrated List Properties dialog did not complete");
    const auto customized = canvas->snapshot();
    const auto& customizedParagraph =
        customized.document.paragraphs().front();
    check(customizedParagraph.format().list_layout &&
              customizedParagraph.format()
                      .list_layout->levels[0].bullet_indent_spaces == 5 &&
              customizedParagraph.format()
                      .list_layout->levels[0].text_indent_spaces == 3,
          "integrated List Properties did not update the selected list");
    check(canvas->defaultListLayout().levels[0].bullet_indent_spaces == 5 &&
              canvas->defaultListLayout().levels[0].text_indent_spaces == 3,
          "Use as Defaults did not propagate to the open document");
    check(QString::fromUtf16(
              customizedParagraph.text().data(),
              static_cast<qsizetype>(customizedParagraph.text().size()))
              .startsWith(QStringLiteral("     \u2022\t")),
          "integrated list properties did not normalize the marker prefix");

    auto* increaseLevel = window.findChild<QAction*>(
        QStringLiteral("paragraph.increaseIndent"));
    check(increaseLevel != nullptr,
          "the contextual ribbon has no increase-level command");
    increaseLevel->trigger();
    QApplication::processEvents();
    const auto leveled = canvas->snapshot();
    check(leveled.document.paragraphs().front().format().list_level ==
              std::uint8_t{1},
          "the ribbon indent control did not advance the list level");
    auto* integratedLevelLabel = window.findChild<QLabel*>(
        QStringLiteral("ribbon.listLevel"));
    check(integratedLevelLabel &&
              integratedLevelLabel->text() == QStringLiteral("Level 2"),
          "the contextual ribbon did not refresh its list-level indicator");

    canvas->toggleBullets();
    QApplication::processEvents();
    check(!windowTabs->isTabVisible(windowListTab),
          "leaving a real list did not hide the contextual List tab");

    auto* newDocument = window.findChild<QAction*>(QStringLiteral("file.new"));
    check(newDocument != nullptr,
          "the list-default integration test cannot create a document");
    newDocument->trigger();
    QApplication::processEvents();
    docxstudio::app::DocumentCanvas* newCanvas = nullptr;
    for (auto* candidate :
         window.findChildren<docxstudio::app::DocumentCanvas*>()) {
        if (candidate != canvas) newCanvas = candidate;
    }
    check(newCanvas != nullptr,
          "new document did not create a second editor canvas");
    newCanvas->toggleBullets();
    const auto newList = newCanvas->snapshot();
    check(newList.document.paragraphs().front().format().list_layout &&
              *newList.document.paragraphs().front().format().list_layout ==
                  canvas->defaultListLayout(),
          "a new list did not use the saved list defaults");

    docxstudio::app::SpellChecker spelling;
    docxstudio::app::DocumentCanvas contextCanvas(spelling);
    contextCanvas.resize(900, 500);
    contextCanvas.show();
    contextCanvas.setFocus();
    contextCanvas.toggleBullets();
    contextCanvas.insertText(QStringLiteral("first"));
    auto sendKey = [&contextCanvas](Qt::Key key,
                                    Qt::KeyboardModifiers modifiers =
                                        Qt::NoModifier) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(&contextCanvas, &event);
    };
    sendKey(Qt::Key_Return);
    contextCanvas.insertText(QStringLiteral("second"));
    sendKey(Qt::Key_Return);
    sendKey(Qt::Key_Return);
    contextCanvas.insertText(QStringLiteral("between"));
    sendKey(Qt::Key_Return);
    contextCanvas.toggleBullets();
    contextCanvas.insertText(QStringLiteral("other list"));
    QApplication::processEvents();
    QInputMethodQueryEvent cursorQuery(Qt::ImCursorRectangle);
    QApplication::sendEvent(&contextCanvas, &cursorQuery);
    const QPoint menuPoint =
        cursorQuery.value(Qt::ImCursorRectangle).toRect().center();
    sendKey(Qt::Key_Home, Qt::ControlModifier);
    bool contextActionFound = false;
    bool contextSignalReceived = false;
    docxstudio::core::ListLayout rightClickLayout;
    rightClickLayout.levels[0].bullet_indent_spaces = 7;
    rightClickLayout.levels[0].text_indent_spaces = 3;
    QObject::connect(
        &contextCanvas,
        &docxstudio::app::DocumentCanvas::listPropertiesRequested,
        [&] {
            contextSignalReceived = true;
            check(contextCanvas.setCurrentListLayout(rightClickLayout),
                  "right-click properties targeted no list");
        });
    QTimer::singleShot(0, &contextCanvas, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu != nullptr, "right-click did not open a document menu");
        QAction* action = nullptr;
        for (auto* candidate : menu->actions()) {
            if (candidate->objectName() ==
                QStringLiteral("context.listProperties")) {
                action = candidate;
                break;
            }
        }
        contextActionFound = action != nullptr;
        if (action) action->trigger();
        menu->close();
    });
    QContextMenuEvent contextEvent(
        QContextMenuEvent::Mouse, menuPoint,
        contextCanvas.viewport()->mapToGlobal(menuPoint));
    QApplication::sendEvent(contextCanvas.viewport(), &contextEvent);
    check(contextActionFound,
          "right-clicking a list did not expose List Properties for that list");
    check(contextSignalReceived,
          "the right-click List Properties action emitted no request");
    const auto contextSnapshot = contextCanvas.snapshot();
    check(contextSnapshot.document.paragraphs().front()
                  .format().list_layout == docxstudio::core::ListLayout{},
          "right-click list properties changed the stale first-list caret");
    check(contextSnapshot.document.paragraphs().back()
                  .format().list_layout == rightClickLayout,
          "right-click list properties did not affect the clicked list");
    contextCanvas.hide();
    window.hide();

    docxstudio::app::MainWindow reopenedWindow;
    auto* reopenedCanvas =
        reopenedWindow.findChild<docxstudio::app::DocumentCanvas*>();
    check(reopenedCanvas &&
              reopenedCanvas->defaultListLayout().levels[0]
                      .bullet_indent_spaces == 5 &&
              reopenedCanvas->defaultListLayout().levels[0]
                      .text_indent_spaces == 3,
          "saved list defaults did not survive an application reload");

    std::cout << "list properties UI tests passed\n";
    return 0;
}
