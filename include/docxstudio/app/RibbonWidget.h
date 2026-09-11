#pragma once

#include <QColor>
#include <QWidget>

class QComboBox;
class QFontComboBox;
class QLabel;
class QTabWidget;
class QToolButton;

namespace docxstudio::app {

class CommandRegistry;

class RibbonWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RibbonWidget(CommandRegistry& commands, QWidget* parent = nullptr);

    void setFontFamily(const QString& family);
    void setFontPointSize(double points);
    void setTextColor(const QColor& color);
    void setHighlightColor(const QColor& color);
    void setZoomPercent(int percent);
    // level is one-based and is clamped to the supported range of 1-10.
    void setListContext(bool visible, int level);
    // The contextual Table tab is available only while a table or table cell
    // is selected. Hiding it also disables its command-palette actions.
    void setTableContext(bool visible);

signals:
    void fontFamilyRequested(const QString& family);
    void fontPointSizeRequested(double points);
    void textColorRequested();
    void textColorSelected(const QColor& color);
    void highlightColorRequested();
    void highlightColorSelected(const QColor& color);
    void clearHighlightRequested();
    void marginPresetRequested(const QString& preset);
    void pageSizeRequested(const QString& preset);
    void zoomRequested(int percent);
    void listPropertiesRequested();
    void tableStyleRequested(const QString& styleKey);

private:
    QWidget* makeHomeTab(CommandRegistry& commands);
    QWidget* makeInsertTab(CommandRegistry& commands);
    QWidget* makeLayoutTab(CommandRegistry& commands);
    QWidget* makeReviewTab(CommandRegistry& commands);
    QWidget* makeViewTab(CommandRegistry& commands);
    QWidget* makeListTab(CommandRegistry& commands);
    QWidget* makeTableTab(CommandRegistry& commands);

    QTabWidget* tabs_{};
    QFontComboBox* fontFamily_{};
    QComboBox* fontSize_{};
    QToolButton* textColor_{};
    QToolButton* highlightColor_{};
    QComboBox* zoom_{};
    QLabel* listLevel_{};
    int listTabIndex_{-1};
    int tableTabIndex_{-1};
};

}  // namespace docxstudio::app
