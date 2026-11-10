#include "docxstudio/app/AppTheme.h"

#include <QFont>
#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>
#include <QStyle>
#include <QWidget>

namespace docxstudio::app {

QColor workspaceColor() { return QColor(QStringLiteral("#f4f5f3")); }
QColor borderColor() { return QColor(QStringLiteral("#dce2da")); }
QColor accentColor() { return QColor(QStringLiteral("#245648")); }
QColor mutedTextColor() { return QColor(QStringLiteral("#65756d")); }

void applyAppTheme(QWidget* root) {
    // QApplication owns the common native style. Assigning separate styles to
    // child docks during construction can invalidate Qt's dock-layout caches.
    if (qApp && qApp->style()->objectName() != QStringLiteral("fusion"))
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette;
    palette.setColor(QPalette::Window, workspaceColor());
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#202c33")));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f6f8f3")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#202c33")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#fcfcfa")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#354c40")));
    palette.setColor(QPalette::Highlight, accentColor());
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Mid, borderColor());
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#27735b")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#929c96")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#929c96")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#929c96")));
    root->setPalette(palette);
    const auto families = QFontDatabase::families();
    for (const auto* family : {"Inter", "Noto Sans", "DejaVu Sans"}) {
        if (families.contains(QString::fromLatin1(family))) {
            root->setFont(QFont(QString::fromLatin1(family), 10));
            break;
        }
    }
    root->setStyleSheet(QStringLiteral(R"QSS(
QMenuBar { background: #fcfcfa; color: #51615c; font-size: 14px; padding: 0px 8px; border-bottom: 1px solid #e5e9e1; }
QMenuBar::item { padding: 13px 3px; border-radius: 5px; }
QMenuBar::item:selected, QMenu::item:selected { background: #e7efe6; color: #174536; }
QMenu { background: white; color: #202c33; border: 1px solid #dce2da; padding: 5px; }
QMenu::item { padding: 7px 24px 7px 10px; border-radius: 4px; }
QMenu::item:disabled { color: #929c96; }
QMenu::separator { height: 1px; background: #e5e9e1; margin: 5px; }
QWidget[objectName="workspace.header"] { background: #fcfcfa; border-bottom: 1px solid #dce2da; }
QLabel[objectName="workspace.brand"] { font-size: 17px; font-weight: 600; color: #243b30; }
QLabel[objectName="workspace.documentName"] { color: #51615c; font-size: 12px; }
QLabel[objectName="status.saveState"] { color: #65756d; padding: 0 8px; }
QWidget[objectName="workspace.writingBar"] { background: #fcfcfa; border-bottom: 1px solid #dce2da; }
QWidget[objectName="workspace.writingBar"] QToolButton { padding: 3px; border-radius: 4px; }
QWidget[objectName="workspace.writingBar"] QToolButton[popupChevron="true"] { padding-left: 7px; padding-right: 20px; }
QToolButton[popupChevron="true"]::menu-indicator { image: none; width: 0px; height: 0px; }
QTabWidget::pane { border: 0; }
QTabBar::tab { background: transparent; color: #65756d; padding: 8px 14px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: #ffffff; color: #245648; border-bottom-color: #245648; }
QTabBar::tab:hover { background: #eef1eb; }
QToolButton, QPushButton { border: 1px solid #dce2da; border-radius: 6px; padding: 6px 9px; background: #fcfcfa; color: #354c40; }
QToolButton[autoRaise="true"] { border-color: transparent; }
QToolButton:hover, QPushButton:hover { background: #eef1eb; border-color: #c7d5c7; }
QToolButton:checked, QPushButton:checked { background: #dfece3; color: #174536; border-color: #a6c1af; }
QToolButton:disabled, QPushButton:disabled { color: #929c96; border-color: transparent; }
QToolButton[role="primary"], QPushButton[role="primary"] { background: #245648; border-color: #245648; color: white; }
QToolButton[role="primary"]:hover, QPushButton[role="primary"]:hover { background: #174536; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextBrowser, QListView, QTreeView { background: white; color: #202c33; border: 1px solid #dce2da; border-radius: 6px; padding: 5px 7px; selection-background-color: #b7d6cd; selection-color: #202c33; }
QToolButton:focus, QPushButton:focus, QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QPlainTextEdit:focus, QTextBrowser:focus, QListView:focus, QTreeView:focus { border: 1px solid #267060; }
QComboBox::drop-down { border: 0; width: 18px; }
QDockWidget { color: #51615c; font-size: 11px; }
QDockWidget::title { background: #fcfdfb; padding: 10px 12px; border-bottom: 1px solid #e5e9e1; }
QMainWindow::separator { background: #e2e7df; width: 5px; height: 5px; }
QStatusBar { background: #fcfcfa; color: #65756d; border-top: 1px solid #e2e7df; }
QStatusBar::item { border: 0; }
QLabel[tone="info"], QWidget[tone="info"] { background: #edf4ec; color: #354f40; border: 1px solid #d4e1d0; border-radius: 6px; padding: 8px; }
QLabel[tone="warning"] { background: #fff4df; color: #815f21; border: 1px solid #eadcbd; border-radius: 6px; padding: 8px; }
)QSS"));
}

}  // namespace docxstudio::app
