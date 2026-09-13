#include "docxstudio/app/RibbonWidget.h"

#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/FontFamilyPicker.h"

#include <QAction>
#include <QComboBox>
#include <QFontComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPolygonF>
#include <QPixmap>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSignalBlocker>

#include <algorithm>
#include <utility>
#include <vector>

namespace docxstudio::app {
namespace {

QString themeIconName(const QString& id) {
    if (id == QStringLiteral("edit.undo")) return QStringLiteral("edit-undo");
    if (id == QStringLiteral("edit.redo")) return QStringLiteral("edit-redo");
    if (id == QStringLiteral("edit.cut")) return QStringLiteral("edit-cut");
    if (id == QStringLiteral("edit.copy")) return QStringLiteral("edit-copy");
    if (id == QStringLiteral("edit.paste")) return QStringLiteral("edit-paste");
    if (id == QStringLiteral("insert.image")) return QStringLiteral("insert-image");
    if (id == QStringLiteral("insert.table")) return QStringLiteral("insert-table");
    if (id == QStringLiteral("review.spelling")) return QStringLiteral("tools-check-spelling");
    return {};
}

QIcon paintedCommandIcon(const QString& id, const QWidget* widget,
                         const QColor& colorSwatch = {}) {
    constexpr int extent = 24;
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor foreground = widget->palette().color(QPalette::ButtonText);
    QPen pen(foreground, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    auto drawLetter = [&](const QString& text, bool bold = false, bool italic = false,
                          int pointSize = 14) {
        QFont font = widget->font();
        font.setPointSize(pointSize);
        font.setBold(bold);
        font.setItalic(italic);
        painter.setFont(font);
        painter.drawText(QRect(1, 1, 22, 21), Qt::AlignCenter, text);
    };
    auto drawLines = [&](Qt::Alignment alignment) {
        const int widths[] = {15, 11, 16, 13};
        for (int row = 0; row < 4; ++row) {
            const int width = widths[row];
            int left = 4;
            if (alignment == Qt::AlignHCenter) left = (extent - width) / 2;
            else if (alignment == Qt::AlignRight) left = 20 - width;
            else if (alignment == Qt::AlignJustify) left = 4;
            painter.drawLine(left, 6 + row * 4,
                             alignment == Qt::AlignJustify ? 20 : left + width,
                             6 + row * 4);
        }
    };

    if (id == QStringLiteral("edit.undo") || id == QStringLiteral("edit.redo")) {
        const bool redo = id.endsWith(QStringLiteral("redo"));
        QPainterPath path;
        path.moveTo(redo ? 7 : 17, 7);
        path.cubicTo(redo ? 18 : 6, 5, redo ? 20 : 4, 11, redo ? 18 : 6, 17);
        painter.drawPath(path);
        const QPolygonF arrow = redo
            ? QPolygonF{{QPointF(16, 3)}, {QPointF(21, 7)}, {QPointF(15, 9)}}
            : QPolygonF{{QPointF(8, 3)}, {QPointF(3, 7)}, {QPointF(9, 9)}};
        painter.setBrush(foreground);
        painter.drawPolygon(arrow);
    } else if (id == QStringLiteral("edit.cut")) {
        painter.drawEllipse(QPointF(6, 17), 3, 3);
        painter.drawEllipse(QPointF(18, 17), 3, 3);
        painter.drawLine(8, 15, 17, 5);
        painter.drawLine(16, 15, 7, 5);
    } else if (id == QStringLiteral("edit.copy")) {
        painter.drawRoundedRect(QRectF(8, 7, 11, 13), 1, 1);
        painter.drawRoundedRect(QRectF(5, 4, 11, 13), 1, 1);
    } else if (id == QStringLiteral("edit.paste")) {
        painter.drawRoundedRect(QRectF(5, 5, 14, 16), 1.5, 1.5);
        painter.setBrush(widget->palette().color(QPalette::Button));
        painter.drawRoundedRect(QRectF(8, 3, 8, 5), 1.5, 1.5);
        painter.drawLine(8, 12, 16, 12);
        painter.drawLine(8, 16, 16, 16);
    } else if (id == QStringLiteral("format.bold")) {
        drawLetter(QStringLiteral("B"), true);
    } else if (id == QStringLiteral("format.italic")) {
        drawLetter(QStringLiteral("I"), false, true);
    } else if (id == QStringLiteral("format.underline")) {
        drawLetter(QStringLiteral("U"));
        painter.drawLine(5, 21, 19, 21);
    } else if (id == QStringLiteral("format.strike")) {
        drawLetter(QStringLiteral("ab"), false, false, 11);
        painter.drawLine(4, 12, 20, 12);
    } else if (id == QStringLiteral("format.superscript")) {
        drawLetter(QStringLiteral("x²"), false, false, 11);
    } else if (id == QStringLiteral("format.subscript")) {
        drawLetter(QStringLiteral("x₂"), false, false, 11);
    } else if (id == QStringLiteral("format.textColor")) {
        drawLetter(QStringLiteral("A"), true, false, 12);
        painter.setPen(QPen(colorSwatch.isValid() ? colorSwatch : Qt::black, 3));
        painter.drawLine(5, 21, 19, 21);
    } else if (id == QStringLiteral("format.highlightColor")) {
        // A broad chisel-tip marker is recognizable at small ribbon sizes and
        // does not conflate text highlighting with the font-color "A" icon.
        const QColor swatch = colorSwatch.isValid()
            ? colorSwatch
            : QColor(QStringLiteral("#f7d154"));
        painter.save();
        painter.translate(12.0, 12.0);
        painter.rotate(-38.0);
        painter.translate(-12.0, -12.0);
        painter.setBrush(widget->palette().color(QPalette::Button));
        painter.drawRoundedRect(QRectF(7.0, 2.5, 10.0, 13.0), 1.5, 1.5);
        painter.drawLine(QPointF(9.0, 6.0), QPointF(15.0, 6.0));
        painter.setBrush(swatch.alpha() == 0 ? Qt::NoBrush : swatch);
        painter.drawPolygon(QPolygonF{{QPointF(7.0, 15.0)},
                                      {QPointF(17.0, 15.0)},
                                      {QPointF(14.5, 21.5)},
                                      {QPointF(9.5, 21.5)}});
        if (swatch.alpha() == 0) {
            painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 1.8,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(7.5, 20.5), QPointF(16.5, 15.5));
        }
        painter.restore();
    } else if (id == QStringLiteral("paragraph.alignLeft")) {
        drawLines(Qt::AlignLeft);
    } else if (id == QStringLiteral("paragraph.alignCenter")) {
        drawLines(Qt::AlignHCenter);
    } else if (id == QStringLiteral("paragraph.alignRight")) {
        drawLines(Qt::AlignRight);
    } else if (id == QStringLiteral("paragraph.justify")) {
        drawLines(Qt::AlignJustify);
    } else if (id == QStringLiteral("paragraph.bullets") ||
               id == QStringLiteral("paragraph.numbering")) {
        QFont font = widget->font();
        font.setPointSize(7);
        painter.setFont(font);
        for (int row = 0; row < 3; ++row) {
            const int y = 7 + row * 6;
            if (id.endsWith(QStringLiteral("bullets"))) {
                painter.setBrush(foreground);
                painter.drawEllipse(QPointF(5, y - 1), 1.3, 1.3);
                painter.setBrush(Qt::NoBrush);
            } else {
                painter.drawText(QRect(1, y - 5, 7, 7), Qt::AlignCenter,
                                 QString::number(row + 1));
            }
            painter.drawLine(10, y - 1, 21, y - 1);
        }
    } else if (id == QStringLiteral("paragraph.decreaseIndent") ||
               id == QStringLiteral("paragraph.increaseIndent")) {
        const bool increase = id.endsWith(QStringLiteral("increaseIndent"));
        painter.drawLine(10, 6, 21, 6);
        painter.drawLine(10, 11, 18, 11);
        painter.drawLine(10, 16, 21, 16);
        painter.drawLine(10, 21, 18, 21);
        const QPolygonF arrow = increase
            ? QPolygonF{{QPointF(3, 9)}, {QPointF(8, 13)}, {QPointF(3, 17)}}
            : QPolygonF{{QPointF(8, 9)}, {QPointF(3, 13)}, {QPointF(8, 17)}};
        painter.setBrush(foreground);
        painter.drawPolygon(arrow);
    } else if (id == QStringLiteral("paragraph.listProperties")) {
        painter.setBrush(foreground);
        painter.drawEllipse(QPointF(4, 7), 1.4, 1.4);
        painter.drawEllipse(QPointF(4, 13), 1.4, 1.4);
        painter.drawEllipse(QPointF(4, 19), 1.4, 1.4);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(8, 7, 21, 7);
        painter.drawLine(8, 13, 21, 13);
        painter.drawLine(8, 19, 21, 19);
        painter.drawLine(11, 3, 11, 21);
    } else if (id == QStringLiteral("insert.table")) {
        painter.drawRect(QRect(3, 4, 18, 16));
        painter.drawLine(9, 4, 9, 20);
        painter.drawLine(15, 4, 15, 20);
        painter.drawLine(3, 9, 21, 9);
        painter.drawLine(3, 15, 21, 15);
    } else if (id.startsWith(QStringLiteral("table.insertRow")) ||
               id.startsWith(QStringLiteral("table.insertColumn"))) {
        const bool row = id.contains(QStringLiteral("Row"));
        const bool after = id.endsWith(QStringLiteral("Below")) ||
                           id.endsWith(QStringLiteral("Right"));
        painter.drawRect(QRect(3, 4, 14, 16));
        painter.drawLine(10, 4, 10, 20);
        painter.drawLine(3, 12, 17, 12);
        painter.setPen(QPen(QColor(QStringLiteral("#2e7d32")), 2.0,
                            Qt::SolidLine, Qt::RoundCap));
        if (row) {
            const int y = after ? 20 : 4;
            painter.drawLine(18, y, 23, y);
            painter.drawLine(QPointF(20.5, static_cast<double>(y) - 2.5),
                             QPointF(20.5, static_cast<double>(y) + 2.5));
        } else {
            const int x = after ? 17 : 3;
            painter.drawLine(x, 20, x, 23);
            painter.drawLine(QPointF(static_cast<double>(x) - 2.5, 21.5),
                             QPointF(static_cast<double>(x) + 2.5, 21.5));
        }
    } else if (id == QStringLiteral("table.deleteRows") ||
               id == QStringLiteral("table.deleteColumns")) {
        const bool rows = id.endsWith(QStringLiteral("Rows"));
        painter.drawRect(QRect(3, 4, 18, 16));
        painter.drawLine(9, 4, 9, 20);
        painter.drawLine(15, 4, 15, 20);
        painter.drawLine(3, 9, 21, 9);
        painter.drawLine(3, 15, 21, 15);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(198, 40, 40, 105));
        painter.drawRect(rows ? QRect(3, 9, 18, 6) : QRect(9, 4, 6, 16));
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 1.8,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(17, 3, 23, 9);
        painter.drawLine(23, 3, 17, 9);
    } else if (id == QStringLiteral("insert.image")) {
        painter.drawRoundedRect(QRectF(3, 4, 18, 16), 1, 1);
        painter.drawEllipse(QPointF(16.5, 8.5), 2, 2);
        QPainterPath mountains;
        mountains.moveTo(5, 18);
        mountains.lineTo(10, 12);
        mountains.lineTo(13, 15);
        mountains.lineTo(16, 12);
        mountains.lineTo(20, 18);
        painter.drawPath(mountains);
    } else if (id == QStringLiteral("picture.size")) {
        painter.drawRect(QRectF(5, 5, 14, 14));
        painter.drawLine(2, 2, 9, 2);
        painter.drawLine(2, 2, 2, 9);
        painter.drawLine(2, 2, 8, 8);
        painter.drawLine(15, 22, 22, 22);
        painter.drawLine(22, 15, 22, 22);
        painter.drawLine(16, 16, 22, 22);
    } else if (id == QStringLiteral("picture.altText")) {
        painter.drawRoundedRect(QRectF(3, 5, 18, 14), 1, 1);
        painter.drawEllipse(QPointF(16.5, 9), 1.8, 1.8);
        painter.drawLine(5, 17, 10, 12);
        painter.drawLine(10, 12, 14, 16);
        QFont font = widget->font();
        font.setPointSize(7);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(1, 0, 14, 9), Qt::AlignLeft, QStringLiteral("ALT"));
    } else if (id == QStringLiteral("picture.layoutOptions")) {
        painter.drawRect(QRectF(7, 7, 10, 10));
        painter.drawLine(2, 4, 22, 4);
        painter.drawLine(2, 20, 22, 20);
        painter.drawLine(2, 9, 5, 9);
        painter.drawLine(19, 9, 22, 9);
        painter.drawLine(2, 14, 5, 14);
        painter.drawLine(19, 14, 22, 14);
    } else if (id == QStringLiteral("picture.delete")) {
        painter.drawRect(QRectF(5, 6, 14, 14));
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 2.0,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(4, 4, 20, 20);
        painter.drawLine(20, 4, 4, 20);
    } else if (id.startsWith(QStringLiteral("picture.wrap"))) {
        painter.drawRect(QRectF(8, 7, 8, 10));
        painter.drawLine(2, 4, 22, 4);
        painter.drawLine(2, 20, 22, 20);
        painter.drawLine(2, 9, 6, 9);
        painter.drawLine(18, 9, 22, 9);
        painter.drawLine(2, 14, 6, 14);
        painter.drawLine(18, 14, 22, 14);
    } else if (id == QStringLiteral("insert.equation")) {
        drawLetter(QStringLiteral("∑"), false, true, 15);
    } else if (id == QStringLiteral("insert.comment") ||
               id == QStringLiteral("review.comment")) {
        painter.drawRoundedRect(QRectF(3, 4, 18, 14), 3, 3);
        painter.drawLine(8, 18, 6, 22);
        painter.drawLine(8, 18, 12, 18);
        painter.drawLine(7, 9, 17, 9);
        painter.drawLine(7, 13, 14, 13);
    } else if (id == QStringLiteral("insert.pageBreak")) {
        painter.drawRect(QRect(5, 2, 14, 7));
        painter.drawRect(QRect(5, 15, 14, 7));
        painter.setPen(QPen(foreground, 1, Qt::DashLine));
        painter.drawLine(2, 12, 22, 12);
    } else if (id == QStringLiteral("insert.textBox")) {
        painter.drawRect(QRect(3, 4, 18, 16));
        drawLetter(QStringLiteral("T"), false, false, 11);
    } else if (id == QStringLiteral("layout.columns")) {
        painter.drawRect(QRect(3, 3, 18, 18));
        painter.drawLine(12, 3, 12, 21);
        for (int y = 7; y <= 17; y += 5) {
            painter.drawLine(5, y, 10, y);
            painter.drawLine(14, y, 19, y);
        }
    } else if (id == QStringLiteral("layout.orientation")) {
        painter.drawRect(QRect(3, 7, 18, 13));
        painter.drawLine(8, 4, 16, 4);
        painter.drawLine(8, 4, 10, 2);
        painter.drawLine(16, 4, 14, 6);
    } else if (id == QStringLiteral("layout.lineSpacing") ||
               id == QStringLiteral("layout.paragraphSpacing")) {
        painter.drawLine(9, 6, 21, 6);
        painter.drawLine(9, 12, 21, 12);
        painter.drawLine(9, 18, 21, 18);
        painter.drawLine(5, 5, 5, 19);
        painter.drawLine(3, 7, 5, 5);
        painter.drawLine(7, 7, 5, 5);
        painter.drawLine(3, 17, 5, 19);
        painter.drawLine(7, 17, 5, 19);
    } else if (id == QStringLiteral("review.acceptChange")) {
        painter.setPen(QPen(QColor(QStringLiteral("#2e7d32")), 2.2,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(4, 13, 10, 19);
        painter.drawLine(10, 19, 21, 5);
    } else if (id == QStringLiteral("review.rejectChange")) {
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 2.2,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(5, 5, 19, 19);
        painter.drawLine(19, 5, 5, 19);
    } else if (id == QStringLiteral("review.spelling")) {
        drawLetter(QStringLiteral("ABC"), true, false, 8);
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 1.4,
                            Qt::DashLine));
        painter.drawLine(3, 20, 21, 20);
    } else if (id == QStringLiteral("review.trackChanges")) {
        painter.drawRect(QRect(5, 3, 14, 18));
        painter.setPen(QPen(QColor(QStringLiteral("#2e7d32")), 2));
        painter.drawLine(9, 8, 16, 8);
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 2));
        painter.drawLine(9, 14, 16, 14);
    } else if (id == QStringLiteral("review.compatibility")) {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        drawLetter(QStringLiteral("i"), true, false, 11);
    } else if (id == QStringLiteral("view.ruler")) {
        painter.drawRect(QRect(2, 7, 20, 10));
        for (int x = 5; x <= 20; x += 3)
            painter.drawLine(x, 7, x, x % 2 == 0 ? 13 : 11);
    } else if (id == QStringLiteral("view.pageWidth")) {
        painter.drawRect(QRect(5, 3, 14, 18));
        painter.drawLine(2, 12, 22, 12);
        painter.drawLine(2, 12, 6, 9);
        painter.drawLine(2, 12, 6, 15);
        painter.drawLine(22, 12, 18, 9);
        painter.drawLine(22, 12, 18, 15);
    } else if (id == QStringLiteral("view.navigation")) {
        painter.drawRect(QRect(4, 3, 16, 18));
        painter.drawLine(9, 3, 9, 21);
        painter.drawLine(11, 8, 18, 8);
        painter.drawLine(11, 13, 18, 13);
    } else {
        drawLetter(QStringLiteral("•"), true, false, 14);
    }
    painter.end();
    return QIcon(pixmap);
}

QIcon colorChoiceIcon(const QColor& color, const QWidget* widget) {
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF chip(2.5, 2.5, 13.0, 13.0);
    painter.fillRect(chip, color.isValid() ? color : Qt::white);
    painter.setPen(QPen(widget->palette().color(QPalette::Mid), 1.0));
    painter.drawRect(chip);
    if (!color.isValid()) {
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 2.0,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(3.5, 14.5), QPointF(14.5, 3.5));
    }
    return QIcon(pixmap);
}

QIcon tableStyleChoiceIcon(const QColor& header, const QColor& band,
                           const QColor& grid, const QWidget* widget) {
    QPixmap pixmap(42, 28);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRect tableRect(2, 2, 38, 24);
    painter.fillRect(tableRect, widget->palette().color(QPalette::Base));
    painter.fillRect(QRect(2, 2, 38, 8), header);
    painter.fillRect(QRect(2, 18, 38, 8), band);
    painter.setPen(QPen(grid, 1.0));
    painter.drawRect(tableRect);
    painter.drawLine(2, 10, 40, 10);
    painter.drawLine(2, 18, 40, 18);
    painter.drawLine(14, 2, 14, 26);
    painter.drawLine(27, 2, 27, 26);
    return QIcon(pixmap);
}

QIcon iconForCommand(const QString& id, const QWidget* widget) {
    const QString themeName = themeIconName(id);
    if (!themeName.isEmpty()) {
        const QIcon themed = QIcon::fromTheme(themeName);
        if (!themed.isNull()) return themed;
    }
    return paintedCommandIcon(id, widget);
}

QString actionToolTip(const QAction& action) {
    QString tip = action.text();
    if (!action.shortcut().isEmpty()) {
        tip += QStringLiteral(" (%1)").arg(action.shortcut().toString(QKeySequence::NativeText));
    }
    return tip;
}

QToolButton* buttonFor(CommandRegistry& commands, const QString& id, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("ribbonButton.%1").arg(id));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(22, 22));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    if (auto* action = commands.action(id)) {
        if (action->icon().isNull()) action->setIcon(iconForCommand(id, parent));
        if (action->toolTip().isEmpty()) action->setToolTip(actionToolTip(*action));
        button->setDefaultAction(action);
        button->setToolTip(action->toolTip());
        button->setAccessibleName(action->text());
        button->setAccessibleDescription(button->toolTip());
    } else {
        button->setText(id);
        button->setEnabled(false);
    }
    return button;
}

QFrame* divider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

void addButtons(QHBoxLayout* layout,
                CommandRegistry& commands,
                QWidget* parent,
                std::initializer_list<const char*> ids) {
    for (const auto* id : ids) {
        layout->addWidget(buttonFor(commands, QString::fromLatin1(id), parent));
    }
}

QWidget* makeTabPage(QWidget* parent, QHBoxLayout*& layout) {
    auto* page = new QWidget(parent);
    layout = new QHBoxLayout(page);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(4);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return page;
}

}  // namespace

RibbonWidget::RibbonWidget(CommandRegistry& commands, QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("ribbon.tabs"));
    tabs_->setDocumentMode(true);
    tabs_->addTab(makeHomeTab(commands), tr("Home"));
    tabs_->addTab(makeInsertTab(commands), tr("Insert"));
    tabs_->addTab(makeLayoutTab(commands), tr("Layout"));
    tabs_->addTab(makeReviewTab(commands), tr("Review"));
    tabs_->addTab(makeViewTab(commands), tr("View"));
    listTabIndex_ = tabs_->addTab(makeListTab(commands), tr("List"));
    tabs_->setTabVisible(listTabIndex_, false);
    tableTabIndex_ = tabs_->addTab(makeTableTab(commands), tr("Table"));
    pictureTabIndex_ = tabs_->addTab(makePictureTab(commands), tr("Picture"));
    outer->addWidget(tabs_);
    setTableContext(false);
    setPictureContext(false);
    setFontFamily(QStringLiteral("Carlito"));
    setFontPointSize(11.0);
    setTextColor(Qt::black);
}

QWidget* RibbonWidget::makeHomeTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    addButtons(layout, commands, page, {"edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste"});
    layout->addWidget(divider(page));

    fontFamily_ = new FontFamilyPicker(page);
    fontFamily_->setObjectName(QStringLiteral("ribbon.fontFamily"));
    fontFamily_->setAccessibleName(tr("Font family"));
    fontFamily_->setMaximumWidth(190);
    connect(fontFamily_, &QComboBox::textActivated, this,
            [this](const QString& family) { emit fontFamilyRequested(family); });
    layout->addWidget(fontFamily_);

    fontSize_ = new QComboBox(page);
    fontSize_->setObjectName(QStringLiteral("ribbon.fontSize"));
    fontSize_->setAccessibleName(tr("Font size"));
    fontSize_->setEditable(true);
    fontSize_->setMaximumWidth(66);
    fontSize_->addItems({"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "32", "36", "48", "72"});
    const auto requestFontSize = [this](const QString& value) {
        bool ok = false;
        const double points = value.toDouble(&ok);
        if (ok && points >= 1.0 && points <= 400.0) {
            emit fontPointSizeRequested(points);
        }
    };
    connect(fontSize_, &QComboBox::textActivated, this, requestFontSize);
    layout->addWidget(fontSize_);
    addButtons(layout, commands, page,
               {"format.bold", "format.italic", "format.underline", "format.strike",
                "format.superscript", "format.subscript"});

    textColor_ = new QToolButton(page);
    textColor_->setObjectName(QStringLiteral("ribbonButton.format.textColor"));
    textColor_->setText(tr("Text color"));
    textColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.textColor"), page, Qt::black));
    textColor_->setIconSize(QSize(22, 22));
    textColor_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    textColor_->setToolTip(tr("Text color"));
    textColor_->setAccessibleName(tr("Text color"));
    textColor_->setAccessibleDescription(tr("Choose the text color"));
    textColor_->setAutoRaise(true);
    textColor_->setFocusPolicy(Qt::NoFocus);
    auto* textColorMenu = new QMenu(textColor_);
    textColorMenu->setObjectName(QStringLiteral("ribbonMenu.textColor"));
    const std::vector<std::pair<QString, QColor>> textColors{
        {tr("Black"), QColor(QStringLiteral("#000000"))},
        {tr("Dark red"), QColor(QStringLiteral("#C00000"))},
        {tr("Red"), QColor(QStringLiteral("#FF0000"))},
        {tr("Orange"), QColor(QStringLiteral("#E95420"))},
        {tr("Yellow"), QColor(QStringLiteral("#FFC000"))},
        {tr("Green"), QColor(QStringLiteral("#008000"))},
        {tr("Blue"), QColor(QStringLiteral("#0070C0"))},
        {tr("Purple"), QColor(QStringLiteral("#7030A0"))},
        {tr("White"), QColor(QStringLiteral("#FFFFFF"))},
    };
    for (std::size_t index = 0; index < textColors.size(); ++index) {
        const auto& [label, color] = textColors[index];
        auto* action = textColorMenu->addAction(
            colorChoiceIcon(color, textColor_), label);
        action->setObjectName(QStringLiteral("ribbonColor.text.%1").arg(index));
        connect(action, &QAction::triggered, this,
                [this, color] { emit textColorSelected(color); });
    }
    textColorMenu->addSeparator();
    auto* moreTextColors = textColorMenu->addAction(tr("More Colors…"));
    moreTextColors->setObjectName(QStringLiteral("ribbonColor.text.more"));
    connect(moreTextColors, &QAction::triggered, this,
            &RibbonWidget::textColorRequested);
    textColor_->setMenu(textColorMenu);
    textColor_->setPopupMode(QToolButton::InstantPopup);
    layout->addWidget(textColor_);
    highlightColor_ = new QToolButton(page);
    highlightColor_->setObjectName(QStringLiteral("ribbonButton.format.highlightColor"));
    highlightColor_->setText(tr("Highlight"));
    highlightColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.highlightColor"), page,
        QColor(QStringLiteral("#f7d154"))));
    highlightColor_->setIconSize(QSize(22, 22));
    highlightColor_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    highlightColor_->setToolTip(tr("Highlight"));
    highlightColor_->setAccessibleName(tr("Highlight"));
    highlightColor_->setAccessibleDescription(tr("Choose the text highlight color"));
    highlightColor_->setAutoRaise(true);
    highlightColor_->setFocusPolicy(Qt::NoFocus);
    auto* highlightMenu = new QMenu(highlightColor_);
    highlightMenu->setObjectName(QStringLiteral("ribbonMenu.highlightColor"));
    auto* noHighlight = highlightMenu->addAction(
        colorChoiceIcon({}, highlightColor_), tr("No Highlight"));
    noHighlight->setObjectName(QStringLiteral("ribbonColor.highlight.none"));
    connect(noHighlight, &QAction::triggered, this,
            &RibbonWidget::clearHighlightRequested);
    highlightMenu->addSeparator();
    const std::vector<std::pair<QString, QColor>> highlightColors{
        {tr("Yellow"), QColor(QStringLiteral("#FFF200"))},
        {tr("Bright green"), QColor(QStringLiteral("#00FF00"))},
        {tr("Cyan"), QColor(QStringLiteral("#00FFFF"))},
        {tr("Pink"), QColor(QStringLiteral("#FF00FF"))},
        {tr("Blue"), QColor(QStringLiteral("#00A2FF"))},
        {tr("Red"), QColor(QStringLiteral("#FF0000"))},
        {tr("Orange"), QColor(QStringLiteral("#FFB347"))},
        {tr("Gray"), QColor(QStringLiteral("#D9D9D9"))},
    };
    for (std::size_t index = 0; index < highlightColors.size(); ++index) {
        const auto& [label, color] = highlightColors[index];
        auto* action = highlightMenu->addAction(
            colorChoiceIcon(color, highlightColor_), label);
        action->setObjectName(
            QStringLiteral("ribbonColor.highlight.%1").arg(index));
        connect(action, &QAction::triggered, this,
                [this, color] { emit highlightColorSelected(color); });
    }
    highlightMenu->addSeparator();
    auto* moreHighlightColors = highlightMenu->addAction(tr("More Colors…"));
    moreHighlightColors->setObjectName(
        QStringLiteral("ribbonColor.highlight.more"));
    connect(moreHighlightColors, &QAction::triggered, this,
            &RibbonWidget::highlightColorRequested);
    highlightColor_->setMenu(highlightMenu);
    highlightColor_->setPopupMode(QToolButton::InstantPopup);
    layout->addWidget(highlightColor_);
    layout->addWidget(divider(page));

    addButtons(layout, commands, page,
               {"paragraph.bullets", "paragraph.numbering", "paragraph.alignLeft",
                "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
                "paragraph.decreaseIndent", "paragraph.increaseIndent"});
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeInsertTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    addButtons(layout, commands, page,
               {"insert.pageBreak", "insert.table", "insert.image", "insert.equation",
                "insert.textBox", "insert.comment"});
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeLayoutTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);

    layout->addWidget(new QLabel(tr("Margins:"), page));
    auto* margins = new QComboBox(page);
    margins->setObjectName(QStringLiteral("ribbon.margins"));
    margins->setAccessibleName(tr("Page margins"));
    margins->addItem(tr("Normal (1\" all)"), QStringLiteral("normal"));
    margins->addItem(tr("Narrow (0.5\" all)"), QStringLiteral("narrow"));
    margins->addItem(tr("Moderate (1\" × 0.75\")"), QStringLiteral("moderate"));
    margins->addItem(tr("Wide (1\" × 2\")"), QStringLiteral("wide"));
    margins->addItem(tr("Office 2003 (1\" × 1.25\")"),
                     QStringLiteral("office2003"));
    margins->insertSeparator(margins->count());
    margins->addItem(tr("Custom Margins…"), QStringLiteral("custom"));
    connect(margins, qOverload<int>(&QComboBox::activated), this,
            [this, margins](int index) {
        emit marginPresetRequested(margins->itemData(index).toString());
    });
    layout->addWidget(margins);

    layout->addWidget(new QLabel(tr("Size:"), page));
    auto* pageSize = new QComboBox(page);
    pageSize->setObjectName(QStringLiteral("ribbon.pageSize"));
    pageSize->setAccessibleName(tr("Page size"));
    pageSize->addItems({tr("Letter"), tr("A4"), tr("Legal")});
    connect(pageSize, &QComboBox::textActivated, this,
            &RibbonWidget::pageSizeRequested);
    layout->addWidget(pageSize);
    addButtons(layout, commands, page,
               {"layout.orientation", "layout.columns", "layout.lineSpacing", "layout.paragraphSpacing"});
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeReviewTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    addButtons(layout, commands, page,
               {"review.spelling", "review.comment", "review.trackChanges",
                "review.acceptChange", "review.rejectChange", "review.compatibility"});
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeViewTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    addButtons(layout, commands, page, {"view.navigation", "view.ruler", "view.pageWidth"});
    layout->addWidget(new QLabel(tr("Zoom:"), page));
    zoom_ = new QComboBox(page);
    zoom_->setObjectName(QStringLiteral("ribbon.zoom"));
    zoom_->setAccessibleName(tr("Zoom"));
    zoom_->addItems({"50%", "75%", "100%", "125%", "150%", "200%"});
    zoom_->setCurrentText("100%");
    connect(zoom_, &QComboBox::textActivated, this, [this](QString value) {
        value.remove(QLatin1Char('%'));
        bool ok = false;
        const int percent = value.toInt(&ok);
        if (ok) {
            emit zoomRequested(percent);
        }
    });
    layout->addWidget(zoom_);
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeListTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("ribbon.listTab"));

    addButtons(layout, commands, page,
               {"paragraph.bullets", "paragraph.numbering",
                "paragraph.decreaseIndent", "paragraph.increaseIndent"});
    layout->addWidget(divider(page));

    listLevel_ = new QLabel(tr("Level 1"), page);
    listLevel_->setObjectName(QStringLiteral("ribbon.listLevel"));
    listLevel_->setAccessibleName(tr("Current list level"));
    listLevel_->setToolTip(tr("Current list level (1-10)"));
    layout->addWidget(listLevel_);

    auto* properties = new QToolButton(page);
    properties->setObjectName(
        QStringLiteral("ribbonButton.paragraph.listProperties"));
    properties->setText(tr("List Properties"));
    properties->setIcon(
        paintedCommandIcon(QStringLiteral("paragraph.listProperties"), page));
    properties->setIconSize(QSize(22, 22));
    properties->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    properties->setToolTip(
        tr("Set bullet and text indentation for levels 1-10"));
    properties->setAccessibleName(tr("List Properties"));
    properties->setAccessibleDescription(properties->toolTip());
    properties->setAutoRaise(true);
    properties->setFocusPolicy(Qt::NoFocus);
    connect(properties, &QToolButton::clicked, this,
            &RibbonWidget::listPropertiesRequested);
    layout->addWidget(properties);
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeTableTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("ribbon.tableTab"));

    auto* rowsAndColumns = new QLabel(tr("Rows and Columns"), page);
    rowsAndColumns->setObjectName(QStringLiteral("ribbon.tableRowsColumnsLabel"));
    rowsAndColumns->setAccessibleName(tr("Table rows and columns"));
    layout->addWidget(rowsAndColumns);
    addButtons(layout, commands, page,
               {"table.insertRowAbove", "table.insertRowBelow",
                "table.insertColumnLeft", "table.insertColumnRight",
                "table.deleteRows", "table.deleteColumns"});
    layout->addWidget(divider(page));

    auto* styles = new QToolButton(page);
    styles->setObjectName(QStringLiteral("ribbonButton.table.styles"));
    styles->setText(tr("Table Styles"));
    styles->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    styles->setPopupMode(QToolButton::InstantPopup);
    styles->setAutoRaise(true);
    styles->setFocusPolicy(Qt::NoFocus);
    styles->setAccessibleName(tr("Table Styles"));
    styles->setAccessibleDescription(
        tr("Choose a visual style for the selected table"));
    styles->setToolTip(styles->accessibleDescription());
    styles->setIconSize(QSize(42, 28));
    styles->setIcon(tableStyleChoiceIcon(
        QColor(QStringLiteral("#E95420")), QColor(QStringLiteral("#FBE9E1")),
        QColor(QStringLiteral("#77216F")), styles));

    struct StyleChoice {
        const char* key;
        const char* label;
        const char* header;
        const char* band;
        const char* grid;
    };
    constexpr StyleChoice choices[] = {
        {"plain", "Plain Table", "#FFFFFF", "#FFFFFF", "#B7B7B7"},
        {"grid", "Table Grid", "#FFFFFF", "#FFFFFF", "#444444"},
        {"light-gray", "Light Gray", "#D9E1F2", "#F2F2F2", "#A6A6A6"},
        {"light-blue", "Light Blue", "#5B9BD5", "#DDEBF7", "#9EADBA"},
        {"light-orange", "Light Orange", "#E95420", "#FBE9E1", "#C88A73"},
        {"medium-blue", "Medium Blue", "#2F75B5", "#D9EAF7", "#2F75B5"},
        {"medium-green", "Medium Green", "#548235", "#E2F0D9", "#548235"},
        {"medium-orange", "Medium Orange", "#C65911", "#FCE4D6", "#C65911"},
        {"aubergine", "Aubergine", "#77216F", "#EFE3EE", "#5E2750"},
        {"orange-accent", "Ubuntu Orange", "#E95420", "#FFF2ED", "#77216F"},
        {"banded-blue", "Banded Blue", "#1F4E78", "#D9EAF7", "#5B9BD5"},
        {"banded-aubergine", "Banded Aubergine", "#5E2750", "#EADDE8", "#77216F"},
        {"dark-header", "Dark Header", "#262626", "#F2F2F2", "#7F7F7F"},
    };
    auto* styleMenu = new QMenu(styles);
    styleMenu->setObjectName(QStringLiteral("ribbonMenu.table.styles"));
    for (const auto& choice : choices) {
        const QString key = QString::fromLatin1(choice.key);
        auto* action = styleMenu->addAction(
            tableStyleChoiceIcon(QColor(QString::fromLatin1(choice.header)),
                                 QColor(QString::fromLatin1(choice.band)),
                                 QColor(QString::fromLatin1(choice.grid)), styles),
            tr(choice.label));
        action->setObjectName(QStringLiteral("ribbonTableStyle.%1").arg(key));
        action->setData(key);
        action->setToolTip(tr("Apply %1 to the selected table").arg(tr(choice.label)));
        connect(action, &QAction::triggered, this,
                [this, key] { emit tableStyleRequested(key); });
    }
    styles->setMenu(styleMenu);
    layout->addWidget(styles);
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makePictureTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("ribbon.pictureTab"));

    addButtons(layout, commands, page,
               {"picture.size", "picture.altText", "picture.layoutOptions"});
    layout->addWidget(divider(page));

    auto* wrap = new QToolButton(page);
    wrap->setObjectName(QStringLiteral("ribbonButton.picture.wrap"));
    wrap->setText(tr("Wrap Text"));
    wrap->setIcon(paintedCommandIcon(QStringLiteral("picture.wrap"), page));
    wrap->setIconSize(QSize(22, 22));
    wrap->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    wrap->setPopupMode(QToolButton::InstantPopup);
    wrap->setAutoRaise(true);
    wrap->setFocusPolicy(Qt::NoFocus);
    wrap->setAccessibleName(tr("Wrap Text"));
    wrap->setAccessibleDescription(
        tr("Choose how text flows around the selected picture"));
    wrap->setToolTip(wrap->accessibleDescription());
    auto* wrapMenu = new QMenu(wrap);
    wrapMenu->setObjectName(QStringLiteral("ribbonMenu.picture.wrap"));
    pictureWrapInline_ = commands.action(QStringLiteral("picture.wrapInline"));
    pictureWrapSquare_ = commands.action(QStringLiteral("picture.wrapSquare"));
    pictureWrapTopBottom_ =
        commands.action(QStringLiteral("picture.wrapTopBottom"));
    for (auto* action : {pictureWrapInline_, pictureWrapSquare_,
                         pictureWrapTopBottom_}) {
        if (!action) continue;
        if (action->icon().isNull()) {
            action->setIcon(paintedCommandIcon(action->objectName(), page));
        }
        wrapMenu->addAction(action);
    }
    wrap->setMenu(wrapMenu);
    layout->addWidget(wrap);
    layout->addWidget(divider(page));
    addButtons(layout, commands, page, {"picture.delete"});
    layout->addStretch(1);
    return page;
}

void RibbonWidget::setFontFamily(const QString& family) {
    const QSignalBlocker blocker(fontFamily_);
    fontFamily_->setCurrentFont(QFont(family));
}

void RibbonWidget::setFontPointSize(double points) {
    const QSignalBlocker blocker(fontSize_);
    fontSize_->setCurrentText(QString::number(points, 'g', 4));
}

void RibbonWidget::setTextColor(const QColor& color) {
    if (!textColor_ || !color.isValid()) return;
    textColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.textColor"), textColor_, color));
}

void RibbonWidget::setHighlightColor(const QColor& color) {
    if (!highlightColor_) return;
    highlightColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.highlightColor"), highlightColor_,
        color.isValid() ? color : QColor(Qt::transparent)));
}

void RibbonWidget::setZoomPercent(int percent) {
    const QSignalBlocker blocker(zoom_);
    zoom_->setCurrentText(QString::number(percent) + QLatin1Char('%'));
}

void RibbonWidget::setListContext(bool visible, int level) {
    if (!tabs_ || listTabIndex_ < 0 || !listLevel_) return;
    const int clampedLevel = std::clamp(level, 1, 10);
    listLevel_->setText(tr("Level %1").arg(clampedLevel));
    listLevel_->setAccessibleDescription(
        tr("The cursor is in list level %1 of 10").arg(clampedLevel));

    if (!visible && tabs_->currentIndex() == listTabIndex_) {
        tabs_->setCurrentIndex(0);
    }
    tabs_->setTabVisible(listTabIndex_, visible);
}

void RibbonWidget::setTableContext(bool visible) {
    if (!tabs_ || tableTabIndex_ < 0) return;
    auto* page = tabs_->widget(tableTabIndex_);
    if (page) {
        const auto buttons = page->findChildren<QToolButton*>();
        for (auto* button : buttons) {
            if (auto* action = button->defaultAction()) {
                action->setEnabled(visible);
            } else {
                button->setEnabled(visible);
            }
        }
    }
    if (!visible && tabs_->currentIndex() == tableTabIndex_) {
        tabs_->setCurrentIndex(0);
    }
    tabs_->setTabVisible(tableTabIndex_, visible);
}

void RibbonWidget::setPictureContext(bool visible,
                                     core::ImagePlacement placement) {
    if (!tabs_ || pictureTabIndex_ < 0) return;
    auto* page = tabs_->widget(pictureTabIndex_);
    if (page) {
        const auto buttons = page->findChildren<QToolButton*>();
        for (auto* button : buttons) {
            if (auto* action = button->defaultAction()) {
                action->setEnabled(visible);
            } else {
                button->setEnabled(visible);
            }
        }
    }
    for (auto* action : {pictureWrapInline_, pictureWrapSquare_,
                         pictureWrapTopBottom_}) {
        if (action) action->setEnabled(visible);
    }
    if (pictureWrapInline_) {
        const QSignalBlocker blocker(pictureWrapInline_);
        pictureWrapInline_->setChecked(
            placement == core::ImagePlacement::inline_with_text);
    }
    if (pictureWrapSquare_) {
        const QSignalBlocker blocker(pictureWrapSquare_);
        pictureWrapSquare_->setChecked(
            placement == core::ImagePlacement::square);
    }
    if (pictureWrapTopBottom_) {
        const QSignalBlocker blocker(pictureWrapTopBottom_);
        pictureWrapTopBottom_->setChecked(
            placement == core::ImagePlacement::top_and_bottom);
    }
    if (!visible && tabs_->currentIndex() == pictureTabIndex_) {
        tabs_->setCurrentIndex(0);
    }
    tabs_->setTabVisible(pictureTabIndex_, visible);
}

}  // namespace docxstudio::app
