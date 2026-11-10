#pragma once

#include "docxstudio/core/document.h"

#include <QColor>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QSpinBox;
class QTabWidget;
class QToolButton;
class QResizeEvent;
class QMenu;

namespace docxstudio::app {

class CommandRegistry;

class RibbonWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RibbonWidget(CommandRegistry& commands, QWidget* parent = nullptr);
    ~RibbonWidget() override;

    void setParagraphStyle(const QString& styleId);
    void setParagraphStyleAvailable(
        bool available, const QString& unavailableLabel = {});
    void setFontFamily(const QString& family);
    void setFontPointSize(double points);
    void setTextColor(const QColor& color);
    void setHighlightColor(const QColor& color);
    // Record a committed custom choice; caret/current-format updates do not add history.
    void rememberTextColor(const QColor& color);
    void rememberHighlightColor(const QColor& color);
    void setZoomPercent(int percent);
    // level is one-based and is clamped to the supported range of 1-10.
    void setListContext(bool visible, int level);
    // The contextual Table tab is available only while a table or table cell
    // is selected. Hiding it also disables its command-palette actions.
    void setTableContext(bool visible);
    void setTableDimensions(double columnWidthPoints, double rowMinimumHeightPoints);
    double tableBorderWidthPoints() const;
    QColor tableBorderColor() const;
    void setTableCellAlignment(std::optional<std::pair<core::ParagraphAlignment,
        core::TableCellVerticalAlignment>> alignment);
    // The contextual Picture tab is available only while one semantic image
    // atom is selected. The active wrap choice is mirrored in its menu.
    void setPictureContext(
        bool visible, core::ImagePlacement placement =
                          core::ImagePlacement::inline_with_text,
        bool editableExcalidraw = false, bool floatingLayoutSupported = true);
    void setEquationContext(bool visible, bool display = false);
    void setHeaderFooterContext(QWidget* controls);
    void setBodyFormattingEnabled(bool enabled);

signals:
    void paragraphStyleRequested(const QString& styleId);
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
    void tableColumnWidthRequested(double points);
    void tableRowMinimumHeightRequested(double points);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* makeHomeTab(CommandRegistry& commands);
    QWidget* makeInsertTab(CommandRegistry& commands);
    QWidget* makeLayoutTab(CommandRegistry& commands);
    QWidget* makeReviewTab(CommandRegistry& commands);
    QWidget* makeViewTab(CommandRegistry& commands);
    QWidget* makeListTab(CommandRegistry& commands);
    QWidget* makeTableTab(CommandRegistry& commands);
    QWidget* makePictureTab(CommandRegistry& commands);
    QWidget* makeEquationRow(CommandRegistry& commands);
    void refreshParagraphStyle();
    void updateCompactPresentation();
    void updateContextPresentation();
    void updateTablePresentation();

    QTabWidget* tabs_{};
    QComboBox* paragraphStyle_{};
    QString paragraphStyleId_{QStringLiteral("Normal")};
    QString paragraphStyleUnavailableLabel_;
    bool paragraphStyleAvailable_{true};
    QFontComboBox* fontFamily_{};
    QComboBox* fontSize_{};
    QToolButton* textColor_{};
    QToolButton* highlightColor_{};
    QToolButton* moreFormatting_{};
    QMenu* paragraphStylesMenu_{};
    QWidget* contextRow_{};
    QWidget* listPage_{};
    QWidget* tablePage_{};
    QList<QToolButton*> tableTools_;
    QToolButton* tableMore_{};
    QDoubleSpinBox* tableColumnWidth_{};
    QDoubleSpinBox* tableRowMinimumHeight_{};
    QDoubleSpinBox* tableBorderWidth_{};
    QColor tableBorderColor_{Qt::black};
    QWidget* picturePage_{};
    QWidget* equationPage_{};
    QPointer<QWidget> headerFooterPage_;
    QMetaObject::Connection headerFooterDestroyedConnection_;
    bool listContextActive_{false};
    bool tableContextActive_{false};
    bool pictureContextActive_{false};
    bool equationContextActive_{false};
    QSpinBox* zoom_{};
    QLabel* listLevel_{};
    int listTabIndex_{-1};
    int tableTabIndex_{-1};
    int pictureTabIndex_{-1};
    QAction* pictureWrapInline_{};
    QAction* pictureWrapSquare_{};
    QAction* pictureWrapTopBottom_{};
    QAction* pictureEditExcalidraw_{};
};

}  // namespace docxstudio::app
