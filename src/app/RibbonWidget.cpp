#include "docxstudio/app/ColorPaletteWidget.h"
#include <QWidgetAction>
#include <QTimer>
#include "docxstudio/app/RibbonWidget.h"
#include "docxstudio/app/AppTheme.h"

#include "docxstudio/app/CommandRegistry.h"
#include "docxstudio/app/FontFamilyPicker.h"

#include <QAction>
#include <QComboBox>
#include <QFontComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDoubleSpinBox>
#include <QColorDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QFontDialog>
#include <QResizeEvent>
#include <QTabBar>
#include <QPolygonF>
#include <QPixmap>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QSpinBox>

#include <algorithm>
#include <utility>
#include <vector>

namespace docxstudio::app {
namespace {

class CommandButton : public QToolButton {
public:
    explicit CommandButton(QWidget* parent = nullptr) : QToolButton(parent) {
        // Mouse clicks retain document typing focus; keyboard traversal remains available.
        setFocusPolicy(Qt::TabFocus);
    }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (menu() && popupMode() == QToolButton::InstantPopup) showMenu();
            else click();
            event->accept();
            return;
        }
        QToolButton::keyPressEvent(event);
    }
};

class MenuButton final : public CommandButton {
public:
    explicit MenuButton(QWidget* parent = nullptr) : CommandButton(parent) {
        setProperty("popupChevron", true);
    }

    QSize sizeHint() const override {
        QSize size = QToolButton::sizeHint();
        size.setWidth(std::max(size.width(), fontMetrics().horizontalAdvance(text()) + 28));
        return size;
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                           QPalette::ButtonText),
                            1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal center = height() / 2.0;
        painter.drawPolyline(QPolygonF{QPointF(width() - 15.0, center - 2.0),
                                       QPointF(width() - 11.0, center + 2.0),
                                       QPointF(width() - 7.0, center - 2.0)});
    }
};

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
    } else if (id == QStringLiteral("table.distributeColumns") || id == QStringLiteral("table.distributeRows")) {
        const bool columns = id.endsWith(QStringLiteral("Columns"));
        painter.drawRect(QRectF(3, 3, 18, 18));
        for (int position : {9, 15}) {
            if (columns) painter.drawLine(position, 3, position, 21);
            else painter.drawLine(3, position, 21, position);
        }
        painter.setPen(QPen(accentColor(), 1.6));
        if (columns) {
            painter.drawLine(5, 12, 19, 12);
            painter.drawPolyline(QPolygonF{{QPointF(8, 9)}, {QPointF(5, 12)}, {QPointF(8, 15)}});
            painter.drawPolyline(QPolygonF{{QPointF(16, 9)}, {QPointF(19, 12)}, {QPointF(16, 15)}});
        } else {
            painter.drawLine(12, 5, 12, 19);
            painter.drawPolyline(QPolygonF{{QPointF(9, 8)}, {QPointF(12, 5)}, {QPointF(15, 8)}});
            painter.drawPolyline(QPolygonF{{QPointF(9, 16)}, {QPointF(12, 19)}, {QPointF(15, 16)}});
        }
    } else if (id == QStringLiteral("table.borders") || id == QStringLiteral("table.borderColor")) {
        painter.setPen(QPen(id.endsWith(QStringLiteral("Color")) && colorSwatch.isValid() ? colorSwatch : foreground, 2));
        painter.drawRect(QRectF(3, 3, 18, 18));
        painter.setPen(QPen(foreground, 1, Qt::DashLine));
        painter.drawLine(3, 12, 21, 12);
        painter.drawLine(12, 3, 12, 21);
    } else if (id.startsWith(QStringLiteral("table.align."))) {
        const auto placement = id.mid(QStringLiteral("table.align.").size());
        painter.setPen(QPen(foreground, 1.1));
        painter.drawRect(QRectF(2.5, 2.5, 19, 19));
        const int top = placement.startsWith(QStringLiteral("top")) ? 5
            : placement.startsWith(QStringLiteral("middle")) ? 9 : 13;
        for (int row = 0; row < 3; ++row) {
            const int lineWidth = row == 1 ? 8 : 12;
            const int left = placement.endsWith(QStringLiteral("Left")) ? 5
                : placement.endsWith(QStringLiteral("Right")) ? 19 - lineWidth
                : (24 - lineWidth) / 2;
            painter.drawLine(left, top + row * 3, left + lineWidth, top + row * 3);
        }
    } else if (id == QStringLiteral("table.selectAll")) {
        painter.drawRect(QRectF(4, 4, 16, 16));
        painter.drawLine(4, 12, 20, 12);
        painter.drawLine(12, 4, 12, 20);
        painter.setPen(QPen(foreground, 2.5));
        for (const auto& point : {QPoint(3, 3), QPoint(21, 3), QPoint(3, 21), QPoint(21, 21)})
            painter.drawPoint(point);
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
    } else if (id == QStringLiteral("insert.excalidraw") ||
               id == QStringLiteral("picture.editExcalidraw")) {
        // This is Excalidraw's own mark, bundled as a transparent raster so
        // the ribbon does not depend on an SVG runtime plugin.
        const QPixmap logo(QStringLiteral(":/icons/excalidraw-logo-96.png"));
        if (!logo.isNull()) {
            painter.drawPixmap(QRect(1, 1, 22, 22), logo);
        }
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
        if (id == QStringLiteral("picture.wrapInline")) {
            painter.drawLine(1, 17, 6, 17);
            painter.drawLine(18, 17, 23, 17);
        } else {
            painter.drawLine(2, 4, 22, 4);
            painter.drawLine(2, 20, 22, 20);
            if (id == QStringLiteral("picture.wrapSquare")) {
                painter.drawLine(2, 9, 6, 9);
                painter.drawLine(18, 9, 22, 9);
                painter.drawLine(2, 14, 6, 14);
                painter.drawLine(18, 14, 22, 14);
            }
        }
    } else if (id == QStringLiteral("equation.edit")) {
        drawLetter(QStringLiteral("∑"), false, true, 13);
        painter.drawLine(15, 20, 22, 13);
        painter.drawLine(14, 21, 17, 20);
    } else if (id == QStringLiteral("equation.inline")) {
        painter.drawLine(1, 12, 6, 12);
        painter.drawLine(18, 12, 23, 12);
        drawLetter(QStringLiteral("x"), false, true, 12);
    } else if (id == QStringLiteral("equation.display")) {
        painter.drawLine(2, 3, 22, 3);
        painter.drawLine(2, 21, 22, 21);
        drawLetter(QStringLiteral("∑"), false, true, 12);
    } else if (id == QStringLiteral("equation.delete")) {
        drawLetter(QStringLiteral("x"), false, true, 12);
        painter.setPen(QPen(QColor(QStringLiteral("#c62828")), 1.8));
        painter.drawLine(3, 3, 21, 21);
    } else if (id == QStringLiteral("insert.equation")) {
        drawLetter(QStringLiteral("∑"), false, true, 15);
    } else if (id == QStringLiteral("insert.comment") ||
               id == QStringLiteral("review.comment")) {
        painter.drawRoundedRect(QRectF(3, 4, 18, 14), 3, 3);
        painter.drawLine(8, 18, 6, 22);
        painter.drawLine(8, 18, 12, 18);
        painter.drawLine(7, 9, 17, 9);
        painter.drawLine(7, 13, 14, 13);
    } else if (id == QStringLiteral("insert.headerFooter")) {
        painter.drawRect(QRect(5, 2, 14, 20));
        painter.setBrush(QColor(QStringLiteral("#e95420")));
        painter.setPen(Qt::NoPen);
        painter.drawRect(QRect(7, 4, 10, 3));
        painter.drawRect(QRect(7, 17, 10, 3));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(foreground, 1.3));
        painter.drawLine(8, 10, 16, 10);
        painter.drawLine(8, 13, 14, 13);
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
    auto* button = new CommandButton(parent);
    button->setObjectName(QStringLiteral("ribbonButton.%1").arg(id));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(22, 22));
    button->setFixedSize(30, 30);
    button->setAutoRaise(true);

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
    page->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout = new QHBoxLayout(page);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(4);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return page;
}

class ContextFlowLayout final : public QLayout {
public:
    explicit ContextFlowLayout(QWidget* parent) : QLayout(parent) {
        setContentsMargins(8, 4, 8, 4);
        setSpacing(4);
        setSizeConstraint(QLayout::SetNoConstraint);
    }
    ~ContextFlowLayout() override { while (auto* item = takeAt(0)) delete item; }
    void addItem(QLayoutItem* item) override { items_.push_back(item); invalidate(); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override {
        return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
    }
    QLayoutItem* takeAt(int index) override {
        auto* item = itemAt(index);
        if (item) items_.erase(items_.begin() + index);
        return item;
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override { return QSize(0, heightForWidth(10000)); }
    QSize sizeHint() const override { return QSize(800, heightForWidth(800)); }
    void setGeometry(const QRect& rect) override { QLayout::setGeometry(rect); arrange(rect, true); }
private:
    int arrange(const QRect& rect, bool apply) const {
        const auto margins = contentsMargins();
        const int available = std::max(1, rect.width() - margins.left() - margins.right());
        int y = rect.top() + margins.top();
        int used = 0;
        int rowHeight = 0;
        std::vector<std::pair<QLayoutItem*, QSize>> row;
        const auto finishRow = [&] {
            if (row.empty()) return;
            int x = rect.left() + margins.left();
            for (const auto& [item, size] : row) {
                if (apply) item->setGeometry(QRect(QPoint(x, y + (rowHeight - size.height()) / 2), size));
                x += size.width() + spacing();
            }
            y += rowHeight + spacing();
            row.clear(); used = 0; rowHeight = 0;
        };
        for (auto* item : items_) {
            if (item->isEmpty()) continue;
            auto size = item->sizeHint();
            size.setWidth(std::min(size.width(), available));
            if (!row.empty() && used + spacing() + size.width() > available) finishRow();
            used += (row.empty() ? 0 : spacing()) + size.width();
            rowHeight = std::max(rowHeight, size.height());
            row.emplace_back(item, size);
        }
        finishRow();
        return y - rect.top() - (y > rect.top() + margins.top() ? spacing() : 0) + margins.bottom();
    }
    std::vector<QLayoutItem*> items_;
};

void makeContextFlow(QWidget* page) {
    auto* previous = page->layout();
    std::vector<QWidget*> controls;
    while (auto* item = previous->takeAt(0)) {
        if (item->widget()) controls.push_back(item->widget());
        delete item;
    }
    delete previous;
    auto* flow = new ContextFlowLayout(page);
    for (auto* control : controls) flow->addWidget(control);
}

}  // namespace

RibbonWidget::RibbonWidget(CommandRegistry& commands, QWidget* parent) : QWidget(parent) {
    applyAppTheme(this);
    setObjectName(QStringLiteral("workspace.writingBar"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Maximum);
    auto* outer = new QVBoxLayout(this);
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    tabs_ = new QTabWidget(this);
    tabs_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    tabs_->setFixedHeight(52);
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
    // The internal pages retain their stable control identities, while the
    // visible workspace exposes tasks through labelled menus instead of tabs.
    tabs_->tabBar()->hide();
    contextRow_ = new QWidget(this);
    contextRow_->setObjectName(QStringLiteral("workspace.contextRow"));
    contextRow_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* contexts = new QVBoxLayout(contextRow_);
    contexts->setContentsMargins(0, 0, 0, 0);
    contexts->setSpacing(0);
    const auto moveContextPage = [&](int index, bool wrap = true) {
        auto* actual = tabs_->widget(index);
        const QString title = tabs_->tabText(index);
        tabs_->removeTab(index);
        tabs_->insertTab(index, new QWidget(tabs_), title);
        contexts->addWidget(actual);
        if (wrap) makeContextFlow(actual);
        actual->hide();
        return actual;
    };
    listPage_ = moveContextPage(listTabIndex_);
    tablePage_ = moveContextPage(tableTabIndex_, false);
    picturePage_ = moveContextPage(pictureTabIndex_);
    equationPage_ = makeEquationRow(commands);
    contexts->addWidget(equationPage_);
    makeContextFlow(equationPage_);
    equationPage_->hide();
    outer->addWidget(tabs_);
    outer->addWidget(contextRow_);
    refreshParagraphStyle();
    setTableContext(false);
    setPictureContext(false);
    setEquationContext(false);
    setListContext(false, 1);
    setFontFamily(QStringLiteral("Helvetica"));
    setFontPointSize(11.0);
    setTextColor(Qt::black);
    updateCompactPresentation();
}

RibbonWidget::~RibbonWidget() {
    // Adopted story controls can be destroyed by QWidget's base destructor,
    // after this class's members and sibling pages have already been released.
    disconnect(headerFooterDestroyedConnection_);
}

QWidget* RibbonWidget::makeHomeTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("workspace.homeControls"));
    // Keep secondary command controls available to integrations and tests;
    // their visible entry points live in menus and the document header.
    for (const auto* id : {"edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste"})
        buttonFor(commands, QString::fromLatin1(id), page)->hide();

    paragraphStyle_ = new QComboBox(page);
    paragraphStyle_->setObjectName(QStringLiteral("ribbon.paragraphStyle"));
    paragraphStyle_->setAccessibleName(tr("Paragraph style"));
    paragraphStyle_->setToolTip(tr("Paragraph style"));
    paragraphStyle_->setMinimumContentsLength(11);
    paragraphStyle_->setMaximumWidth(140);
    for (const auto& style : core::builtInParagraphStyles()) {
        const QString id = QString::fromUtf8(
            style.id.data(), static_cast<qsizetype>(style.id.size()));
        const QString name = QString::fromUtf8(
            style.display_name.data(),
            static_cast<qsizetype>(style.display_name.size()));
        paragraphStyle_->addItem(name, id);
        const int index = paragraphStyle_->count() - 1;
        QFont previewFont = paragraphStyle_->font();
        previewFont.setBold(style.character_format.bold.value_or(false));
        previewFont.setItalic(style.character_format.italic.value_or(false));
        if (style.character_format.font_size_half_points) {
            // Preview the hierarchy without allowing a 28-point Title row to
            // make the compact ribbon popup awkwardly tall.
            previewFont.setPointSizeF(std::clamp(
                *style.character_format.font_size_half_points / 2.0,
                9.0, 14.0));
        }
        paragraphStyle_->setItemData(index, previewFont, Qt::FontRole);
    }
    connect(paragraphStyle_, qOverload<int>(&QComboBox::activated), this,
            [this](int index) {
        if (index < 0 ||
            paragraphStyle_->itemData(index, Qt::UserRole + 1).toBool()) {
            return;
        }
        const QString id = paragraphStyle_->itemData(index).toString();
        if (!id.isEmpty()) emit paragraphStyleRequested(id);
    });
    layout->addWidget(paragraphStyle_);

    fontFamily_ = new FontFamilyPicker(page);
    fontFamily_->setObjectName(QStringLiteral("ribbon.fontFamily"));
    fontFamily_->setAccessibleName(tr("Font family"));
    fontFamily_->setMaximumWidth(150);
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

    const auto colorButton=[this,page,layout](bool highlight) {
        auto* button=new CommandButton(page);
        const auto command=highlight ? QStringLiteral("format.highlightColor") : QStringLiteral("format.textColor");
        button->setObjectName(QStringLiteral("ribbonButton.")+command);
        button->setText(highlight ? tr("Highlight") : tr("Text color"));
        button->setIcon(paintedCommandIcon(command,page,highlight ? QColor(QStringLiteral("#f7d154")) : QColor(Qt::black)));
        button->setIconSize(QSize(22,22));button->setFixedSize(30,30);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);button->setAutoRaise(true);
        button->setToolTip(button->text());button->setAccessibleName(button->text());
        button->setAccessibleDescription(highlight ? tr("Choose the text highlight color") : tr("Choose the text color"));
        auto* menu=new QMenu(button->text(),button);
        menu->setObjectName(highlight ? QStringLiteral("ribbonMenu.highlightColor") : QStringLiteral("ribbonMenu.textColor"));
        auto* grid=new ColorPaletteWidget(highlight ? ColorPaletteWidget::Usage::Highlight : ColorPaletteWidget::Usage::Text,menu);
        auto* action=new QWidgetAction(menu);action->setDefaultWidget(grid);menu->addAction(action);
        connect(menu,&QMenu::aboutToShow,grid,[grid] {
            QTimer::singleShot(0,grid,[grid] { if (grid->isVisible()) grid->setFocus(Qt::PopupFocusReason); });
        });
        connect(grid,&ColorPaletteWidget::colorSelected,this,[this,menu,highlight](const QColor& color) {
            menu->hide();
            if (highlight) emit highlightColorSelected(color);else emit textColorSelected(color);
        });
        connect(grid,&ColorPaletteWidget::defaultRequested,this,[this,menu,highlight] {
            menu->hide();
            if (highlight) emit clearHighlightRequested();else emit textColorSelected({});
        });
        connect(grid,&ColorPaletteWidget::customColorRequested,this,[this,menu,highlight] {
            menu->hide();
            if (highlight) emit highlightColorRequested();else emit textColorRequested();
        });
        button->setMenu(menu);button->setPopupMode(QToolButton::InstantPopup);
        layout->addWidget(button);return button;
    };
    textColor_=colorButton(false);
    highlightColor_=colorButton(true);
    layout->addWidget(divider(page));

    addButtons(layout, commands, page,
               {"paragraph.bullets", "paragraph.numbering", "paragraph.alignLeft",
                "paragraph.alignCenter", "paragraph.alignRight", "paragraph.justify",
                "paragraph.decreaseIndent", "paragraph.increaseIndent"});
    moreFormatting_ = new MenuButton(page);
    moreFormatting_->setObjectName(QStringLiteral("workspace.moreFormatting"));
    moreFormatting_->setText(tr("More"));
    moreFormatting_->setAccessibleName(tr("More formatting"));
    moreFormatting_->setToolTip(tr("More formatting, font and paragraph options"));
    moreFormatting_->setPopupMode(QToolButton::InstantPopup);
    auto* more = new QMenu(moreFormatting_);
    moreFormatting_->setMenu(more);
    auto* font = new QAction(tr("Font…"), this);
    font->setObjectName(QStringLiteral("workspace.fontDialog"));
    connect(font, &QAction::triggered, this, [this] {
        bool accepted = false;
        QFont initial(fontFamily_->currentText());
        initial.setPointSizeF(fontSize_->currentText().toDouble());
        const QFont selected = QFontDialog::getFont(&accepted, initial, this, tr("Font"));
        if (accepted) {
            emit fontFamilyRequested(selected.family());
            emit fontPointSizeRequested(selected.pointSizeF());
        }
    });
    auto* styles = new QMenu(tr("Paragraph style"), this);
    paragraphStylesMenu_ = styles;
    for (const auto& style : core::builtInParagraphStyles()) {
        const QString id = QString::fromUtf8(style.id.data(),
            static_cast<qsizetype>(style.id.size()));
        connect(styles->addAction(QString::fromUtf8(style.display_name.data(),
                static_cast<qsizetype>(style.display_name.size()))),
            &QAction::triggered, this, [this, id] { emit paragraphStyleRequested(id); });
    }
    textColor_->menu()->setTitle(tr("Text color"));
    highlightColor_->menu()->setTitle(tr("Highlight"));
    layout->addWidget(moreFormatting_);
    layout->addStretch(1);
    return page;
}

void RibbonWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateCompactPresentation();
    updateContextPresentation();
}

void RibbonWidget::updateCompactPresentation() {
    auto* home = findChild<QWidget*>(QStringLiteral("workspace.homeControls"));
    if (!home || !moreFormatting_) return;
    auto* row = qobject_cast<QHBoxLayout*>(home->layout());
    if (!row) return;
    const auto primary = [&](const char* id) {
        return home->findChild<QToolButton*>(QStringLiteral("ribbonButton.%1").arg(id));
    };
    for (int index = 0; index < row->count(); ++index) {
        auto* widget = row->itemAt(index)->widget();
        if (!widget || widget == moreFormatting_) continue;
        auto* button = qobject_cast<QToolButton*>(widget);
        if (button && button->defaultAction())
            button->setVisible(button->defaultAction()->isVisible());
        else if (button && button != textColor_ && button != highlightColor_)
            button->hide();
        else widget->show();
    }
    // Only the visible list row replaces primary indentation controls.
    const bool expandedList = listContextActive_ && !tableContextActive_ && !pictureContextActive_ && !equationContextActive_ && !headerFooterPage_;
    if (expandedList) for (const auto* id : {"paragraph.decreaseIndent", "paragraph.increaseIndent"})
        if (auto* button = primary(id)) button->hide();
    moreFormatting_->hide();
    auto* more = moreFormatting_->menu();
    more->clear();
    const auto requiredWidth = [&] {
        const auto margins = row->contentsMargins();
        int required = margins.left() + margins.right();
        int visible = 0;
        for (int index = 0; index < row->count(); ++index) {
            auto* widget = row->itemAt(index)->widget();
            if (!widget || widget->isHidden()) continue;
            required += std::clamp(widget->sizeHint().width(), widget->minimumWidth(), widget->maximumWidth());
            ++visible;
        }
        return required + std::max(0, visible - 1) * row->spacing();
    };
    if (requiredWidth() > width()) {
        moreFormatting_->show();
        const std::vector<QWidget*> candidates{fontFamily_, primary("format.strike"), primary("format.superscript"),
            primary("format.subscript"), primary("paragraph.decreaseIndent"), primary("paragraph.increaseIndent"),
            highlightColor_, paragraphStyle_, fontSize_, primary("format.bold"), primary("format.italic"),
            primary("format.underline")};
        for (auto* widget : candidates) {
            if (requiredWidth() <= width()) break;
            if (widget) widget->hide();
        }
    }
    if (fontFamily_->isHidden() || fontSize_->isHidden())
        more->addAction(findChild<QAction*>(QStringLiteral("workspace.fontDialog")));
    if (paragraphStyle_->isHidden()) more->addMenu(paragraphStylesMenu_);
    if (textColor_->isHidden()) more->addMenu(textColor_->menu());
    if (highlightColor_->isHidden()) more->addMenu(highlightColor_->menu());
    for (int index = 0; index < row->count(); ++index) {
        auto* button = qobject_cast<QToolButton*>(row->itemAt(index)->widget());
        if (!button || !button->isHidden() || !button->defaultAction() || !button->defaultAction()->isVisible()) continue;
        if (expandedList && (button == primary("paragraph.decreaseIndent") || button == primary("paragraph.increaseIndent"))) continue;
        more->addAction(button->defaultAction());
    }
    moreFormatting_->setVisible(!more->actions().empty());
    row->invalidate();
}

QWidget* RibbonWidget::makeInsertTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    addButtons(layout, commands, page,
               {"insert.pageBreak", "insert.table", "insert.image",
                "insert.excalidraw", "insert.equation",
                "insert.headerFooter",
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
    zoom_ = new QSpinBox(page);
    zoom_->setObjectName(QStringLiteral("ribbon.zoom"));
    zoom_->setAccessibleName(tr("Zoom"));
    zoom_->setAccessibleDescription(
        tr("Set document zoom from 25 to 400 percent"));
    zoom_->setRange(25, 400);
    zoom_->setSingleStep(10);
    zoom_->setSuffix(QStringLiteral("%"));
    zoom_->setKeyboardTracking(false);
    zoom_->setValue(100);
    connect(zoom_, qOverload<int>(&QSpinBox::valueChanged), this,
            &RibbonWidget::zoomRequested);
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

    auto* properties = new CommandButton(page);
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

    const auto addChooser = [&](const QString& key, const QString& text, const QString& description,
                                const QString& icon) {
        auto* button = new MenuButton(page);
        button->setObjectName(QStringLiteral("ribbonButton.table.%1").arg(key));
        button->setText(text);
        button->setAccessibleName(text);
        button->setToolTip(description);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(paintedCommandIcon(icon, button));
        button->setIconSize(QSize(22, 22));
        button->setAutoRaise(true);
        button->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(text, button);
        menu->setObjectName(QStringLiteral("ribbonMenu.table.%1").arg(key));
        button->setMenu(menu);
        layout->addWidget(button);
        tableTools_.push_back(button);
        return button;
    };
    const auto addCommands = [&](QMenu* menu, std::initializer_list<const char*> ids) {
        for (const auto* id : ids) if (auto* action = commands.action(QString::fromLatin1(id))) {
            action->setIcon(paintedCommandIcon(QString::fromLatin1(id), page));
            menu->addAction(action);
        }
    };
    auto* select = buttonFor(commands, QStringLiteral("table.selectAll"), page);
    layout->addWidget(select);
    tableTools_.push_back(select);
    auto* insert = addChooser(QStringLiteral("insert"), tr("Insert"), tr("Insert table rows or columns"),
        QStringLiteral("table.insertRowBelow"));
    addCommands(insert->menu(), {"table.insertRowAbove", "table.insertRowBelow", "table.insertColumnLeft", "table.insertColumnRight"});
    auto* remove = addChooser(QStringLiteral("delete"), tr("Delete"), tr("Delete selected table rows or columns"),
        QStringLiteral("table.deleteRows"));
    addCommands(remove->menu(), {"table.deleteRows", "table.deleteColumns"});
    auto* sizeChooser = addChooser(QStringLiteral("size"), tr("Size"), tr("Column width, row minimum height, and distribution"),
        QStringLiteral("table.distributeColumns"));
    auto* alignmentChooser = addChooser(QStringLiteral("alignment"), tr("Alignment"),
        tr("Position text horizontally and vertically in selected cells"), QStringLiteral("table.align.topLeft"));
    auto* alignmentMenu = alignmentChooser->menu();
    auto* alignment = new QWidget(alignmentMenu);
    alignment->setObjectName(QStringLiteral("ribbon.tableAlignment"));
    alignment->setAccessibleName(tr("Text position in selected table cells"));
    auto* positions = new QGridLayout(alignment);
    positions->setContentsMargins(0, 0, 0, 0);
    positions->setSpacing(2);
    const char* placements[] = {"topLeft", "topCenter", "topRight",
        "middleLeft", "middleCenter", "middleRight",
        "bottomLeft", "bottomCenter", "bottomRight"};
    for (int index = 0; index < 9; ++index) {
        auto* button = buttonFor(commands,
            QStringLiteral("table.align.%1").arg(placements[index]), alignment);
        button->setFixedSize(26, 26);
        button->setIconSize(QSize(22, 22));
        positions->addWidget(button, index / 3, index % 3);
        if (auto* action = button->defaultAction())
            connect(action, &QAction::triggered, alignmentMenu, &QMenu::hide);
    }
    auto* alignmentAction = new QWidgetAction(alignmentMenu);
    alignmentAction->setDefaultWidget(alignment);
    alignmentMenu->addAction(alignmentAction);
    connect(alignmentMenu, &QMenu::aboutToShow, alignment, [alignment] {
        QTimer::singleShot(0, alignment, [alignment] {
            if (!alignment->isVisible()) return;
            const auto buttons = alignment->findChildren<QToolButton*>();
            for (auto* button : buttons) if (button->isChecked()) {
                button->setFocus(Qt::PopupFocusReason);
                return;
            }
            if (!buttons.empty()) buttons.front()->setFocus(Qt::PopupFocusReason);
        });
    });

    auto* dimensions = new QWidget(sizeChooser->menu());
    dimensions->setObjectName(QStringLiteral("ribbon.tableDimensions"));
    auto* sizes = new QGridLayout(dimensions);
    sizes->setContentsMargins(0, 0, 0, 0);
    sizes->setHorizontalSpacing(4);
    sizes->setVerticalSpacing(2);
    const auto addSize = [&](const QString& name, const QString& label, const QString& accessible,
                             double minimum, int row) {
        auto* spin = new QDoubleSpinBox(dimensions);
        spin->setObjectName(name);
        spin->setAccessibleName(accessible);
        spin->setToolTip(accessible);
        spin->setRange(minimum, 1584);
        spin->setDecimals(2);
        spin->setSuffix(tr(" pt"));
        spin->setKeyboardTracking(false);
        spin->setFixedWidth(108);
        auto* caption = new QLabel(label, dimensions);
        caption->setBuddy(spin);
        sizes->addWidget(caption, row, 0);
        sizes->addWidget(spin, row, 1);
        return spin;
    };
    tableColumnWidth_ = addSize(QStringLiteral("ribbon.tableColumnWidth"), tr("&Width"),
        tr("Selected column width in points"), 1, 0);
    tableRowMinimumHeight_ = addSize(QStringLiteral("ribbon.tableRowMinimumHeight"), tr("Min &height"),
        tr("Selected row minimum height in points; Auto fits content"), 0, 1);
    tableRowMinimumHeight_->setSpecialValueText(tr("Auto"));
    const auto connectSize = [this](QDoubleSpinBox* spin, bool column) {
        spin->setProperty("committedValue", spin->value());
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, spin, column] {
            const double points = spin->value();
            const double committed = spin->property("committedValue").toDouble();
            if (points < (column ? 1 : 0)) {
                const QSignalBlocker blocker(spin);
                spin->setValue(committed);
                return;
            }
            if (qFuzzyCompare(points + 1, committed + 1)) return;
            {
                const QSignalBlocker blocker(spin);
                spin->setMinimum(column ? 1 : 0);
                spin->setSpecialValueText(column ? QString{} : tr("Auto"));
                spin->setValue(points);
                spin->setProperty("committedValue", points);
            }
            if (column) emit tableColumnWidthRequested(points);
            else emit tableRowMinimumHeightRequested(points);
        });
    };
    connectSize(tableColumnWidth_, true);
    connectSize(tableRowMinimumHeight_, false);
    auto* dimensionAction = new QWidgetAction(sizeChooser->menu());
    dimensionAction->setDefaultWidget(dimensions);
    sizeChooser->menu()->addAction(dimensionAction);
    sizeChooser->menu()->addSeparator();
    addCommands(sizeChooser->menu(), {"table.distributeColumns", "table.distributeRows"});
    connect(sizeChooser->menu(), &QMenu::aboutToShow, tableColumnWidth_, [column = tableColumnWidth_] {
        QTimer::singleShot(0, column, [column] {
            if (!column->isVisible()) return;
            column->setFocus(Qt::PopupFocusReason);
            column->selectAll();
        });
    });

    auto* borders = addChooser(QStringLiteral("borders"), tr("Borders"),
        tr("Apply borders to selected cells; use Select Table for the whole table"), QStringLiteral("table.borders"));
    auto* presets = borders->menu();
    auto* borderTools = new QWidget(presets);
    borderTools->setObjectName(QStringLiteral("ribbon.tableBorderTools"));
    auto* borderLayout = new QHBoxLayout(borderTools);
    borderLayout->setContentsMargins(0, 0, 0, 0);
    borderLayout->setSpacing(2);
    tableBorderWidth_ = new QDoubleSpinBox(borderTools);
    tableBorderWidth_->setObjectName(QStringLiteral("ribbon.tableBorderWidth"));
    tableBorderWidth_->setAccessibleName(tr("Border line weight in points"));
    tableBorderWidth_->setToolTip(tr("Line weight for the next border preset"));
    tableBorderWidth_->setRange(0.125, 12);
    tableBorderWidth_->setDecimals(3);
    tableBorderWidth_->setSingleStep(0.25);
    tableBorderWidth_->setSuffix(tr(" pt"));
    tableBorderWidth_->setKeyboardTracking(false);
    tableBorderWidth_->setValue(0.5);
    tableBorderWidth_->setFixedWidth(92);
    auto* weightLabel = new QLabel(tr("Line &weight"), borderTools);
    weightLabel->setBuddy(tableBorderWidth_);
    borderLayout->addWidget(weightLabel);
    borderLayout->addWidget(tableBorderWidth_);
    auto* color = new CommandButton(borderTools);
    color->setObjectName(QStringLiteral("ribbonButton.table.borderColor"));
    color->setAccessibleName(tr("Border color"));
    color->setToolTip(tr("Color for the next border preset"));
    color->setIconSize(QSize(22, 22));
    color->setFixedSize(30, 30);
    color->setAutoRaise(true);
    color->setIcon(paintedCommandIcon(QStringLiteral("table.borderColor"), color, tableBorderColor_));
    color->setPopupMode(QToolButton::InstantPopup);
    auto* colors = new QMenu(tr("Border color"), color);
    colors->setObjectName(QStringLiteral("ribbonMenu.table.borderColor"));
    auto* palette = new ColorPaletteWidget(ColorPaletteWidget::Usage::Text, colors);
    palette->setAccessibleName(tr("Border color palette"));
    palette->setCurrentColor(tableBorderColor_);
    auto* paletteAction = new QWidgetAction(colors);
    paletteAction->setDefaultWidget(palette);
    colors->addAction(paletteAction);
    const auto chooseColor = [this, color, palette, colors](const QColor& selected) {
        tableBorderColor_ = selected.isValid() ? selected : QColor(Qt::black);
        color->setIcon(paintedCommandIcon(QStringLiteral("table.borderColor"), color, tableBorderColor_));
        palette->setCurrentColor(tableBorderColor_);
        colors->hide();
    };
    connect(palette, &ColorPaletteWidget::colorSelected, this, chooseColor);
    connect(palette, &ColorPaletteWidget::defaultRequested, this, [chooseColor] { chooseColor(Qt::black); });
    connect(palette, &ColorPaletteWidget::customColorRequested, this, [this, chooseColor, colors, palette] {
        colors->hide();
        const auto selected = QColorDialog::getColor(tableBorderColor_, this, tr("Border color"));
        if (selected.isValid()) {
            palette->rememberColor(selected);
            chooseColor(selected);
        }
    });
    connect(colors, &QMenu::aboutToShow, palette, [palette] {
        QTimer::singleShot(0, palette, [palette] { if (palette->isVisible()) palette->setFocus(Qt::PopupFocusReason); });
    });
    color->setMenu(colors);
    borderLayout->addWidget(color);
    auto* borderToolsAction = new QWidgetAction(presets);
    borderToolsAction->setDefaultWidget(borderTools);
    presets->addAction(borderToolsAction);
    presets->addSeparator();
    for (const auto* preset : {"all", "outside", "inside", "none", "top", "bottom", "left", "right"})
        if (auto* action = commands.action(QStringLiteral("table.borders.%1").arg(preset))) presets->addAction(action);

    auto* styles = new MenuButton(page);
    styles->setObjectName(QStringLiteral("ribbonButton.table.styles"));
    styles->setText(tr("Table Styles"));
    styles->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    styles->setPopupMode(QToolButton::InstantPopup);
    styles->setAutoRaise(true);

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
    auto* styleMenu = new QMenu(styles->text(), styles);
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
    tableTools_.push_back(styles);
    tableMore_ = new MenuButton(page);
    tableMore_->setObjectName(QStringLiteral("ribbonButton.table.more"));
    tableMore_->setText(tr("More"));
    tableMore_->setAccessibleName(tr("More table controls"));
    tableMore_->setToolTip(tr("Table controls that do not fit in this row"));
    tableMore_->setAutoRaise(true);
    tableMore_->setPopupMode(QToolButton::InstantPopup);
    tableMore_->setMenu(new QMenu(tableMore_));
    layout->addWidget(tableMore_);
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makePictureTab(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("ribbon.pictureTab"));
    auto* contextLabel = new QLabel(tr("Picture"), page);
    contextLabel->setObjectName(QStringLiteral("ribbon.pictureContextLabel"));
    layout->addWidget(contextLabel);

    addButtons(layout, commands, page,
               {"picture.editExcalidraw", "picture.size",
                "picture.altText", "picture.layoutOptions"});
    pictureEditExcalidraw_ =
        commands.action(QStringLiteral("picture.editExcalidraw"));
    layout->addWidget(divider(page));

    pictureWrapInline_ = commands.action(QStringLiteral("picture.wrapInline"));
    pictureWrapSquare_ = commands.action(QStringLiteral("picture.wrapSquare"));
    pictureWrapTopBottom_ = commands.action(QStringLiteral("picture.wrapTopBottom"));
    addButtons(layout, commands, page,
               {"picture.wrapInline", "picture.wrapSquare", "picture.wrapTopBottom"});
    layout->addWidget(divider(page));
    addButtons(layout, commands, page, {"picture.delete"});
    layout->addStretch(1);
    return page;
}

QWidget* RibbonWidget::makeEquationRow(CommandRegistry& commands) {
    QHBoxLayout* layout{};
    auto* page = makeTabPage(this, layout);
    page->setObjectName(QStringLiteral("ribbon.equationTab"));
    layout->addWidget(new QLabel(tr("Equation"), page));
    addButtons(layout, commands, page,
               {"equation.edit", "equation.inline", "equation.display", "equation.delete"});
    layout->addStretch(1);
    return page;
}

void RibbonWidget::setFontFamily(const QString& family) {
    const QSignalBlocker blocker(fontFamily_);
    fontFamily_->setCurrentText(family);
}

void RibbonWidget::setParagraphStyle(const QString& styleId) {
    if (paragraphStyleId_ == styleId) return;
    paragraphStyleId_ = styleId;
    refreshParagraphStyle();
}

void RibbonWidget::setParagraphStyleAvailable(
    bool available, const QString& unavailableLabel) {
    if (paragraphStylesMenu_) paragraphStylesMenu_->setEnabled(available);
    const QString label = available
        ? QString{}
        : (unavailableLabel.isEmpty()
               ? tr("Paragraph styles unavailable")
               : unavailableLabel);
    if (paragraphStyleAvailable_ == available &&
        paragraphStyleUnavailableLabel_ == label) {
        return;
    }
    paragraphStyleAvailable_ = available;
    paragraphStyleUnavailableLabel_ = label;
    refreshParagraphStyle();
}

void RibbonWidget::refreshParagraphStyle() {
    if (!paragraphStyle_) return;
    const QSignalBlocker blocker(paragraphStyle_);
    // A temporary row represents either a mixed selection or an imported
    // custom style, or an explicitly unavailable editor context. Remove it
    // only when the semantic context actually changes; the public setters
    // short-circuit repeated cursor notifications.
    for (int index = paragraphStyle_->count() - 1; index >= 0; --index) {
        if (paragraphStyle_->itemData(index, Qt::UserRole + 1).toBool()) {
            paragraphStyle_->removeItem(index);
        }
    }

    paragraphStyle_->setEnabled(paragraphStyleAvailable_);
    if (!paragraphStyleAvailable_) {
        paragraphStyle_->addItem(paragraphStyleUnavailableLabel_);
        const int transient = paragraphStyle_->count() - 1;
        paragraphStyle_->setItemData(transient, true, Qt::UserRole + 1);
        paragraphStyle_->setCurrentIndex(transient);
        paragraphStyle_->setToolTip(paragraphStyleUnavailableLabel_);
        paragraphStyle_->setAccessibleDescription(
            paragraphStyleUnavailableLabel_);
        return;
    }

    paragraphStyle_->setAccessibleDescription({});
    const int found = paragraphStyleId_.isEmpty()
        ? -1
        : paragraphStyle_->findData(paragraphStyleId_);
    if (found >= 0) {
        paragraphStyle_->setCurrentIndex(found);
        paragraphStyle_->setToolTip(
            tr("Paragraph style: %1").arg(paragraphStyle_->itemText(found)));
        return;
    }

    const QString label = paragraphStyleId_.isEmpty()
        ? tr("Multiple styles")
        : tr("Custom: %1").arg(paragraphStyleId_);
    paragraphStyle_->addItem(label, paragraphStyleId_);
    const int transient = paragraphStyle_->count() - 1;
    paragraphStyle_->setItemData(transient, true, Qt::UserRole + 1);
    paragraphStyle_->setCurrentIndex(transient);
    paragraphStyle_->setToolTip(label);
}

void RibbonWidget::setFontPointSize(double points) {
    const QSignalBlocker blocker(fontSize_);
    fontSize_->setCurrentText(QString::number(points, 'g', 4));
}

void RibbonWidget::setTextColor(const QColor& color) {
    if (!textColor_) return;
    textColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.textColor"), textColor_, color.isValid() ? color : QColor(Qt::black)));
    if (auto* grid=textColor_->menu()->findChild<ColorPaletteWidget*>()) grid->setCurrentColor(color);
}

void RibbonWidget::setHighlightColor(const QColor& color) {
    if (!highlightColor_) return;
    highlightColor_->setIcon(paintedCommandIcon(
        QStringLiteral("format.highlightColor"), highlightColor_,
        color.isValid() ? color : QColor(Qt::transparent)));
    if (auto* grid=highlightColor_->menu()->findChild<ColorPaletteWidget*>()) grid->setCurrentColor(color);
}

void RibbonWidget::rememberTextColor(const QColor& color) {
    if (textColor_) if (auto* grid=textColor_->menu()->findChild<ColorPaletteWidget*>()) grid->rememberColor(color);
}

void RibbonWidget::rememberHighlightColor(const QColor& color) {
    if (highlightColor_) if (auto* grid=highlightColor_->menu()->findChild<ColorPaletteWidget*>()) grid->rememberColor(color);
}

void RibbonWidget::setZoomPercent(int percent) {
    const QSignalBlocker blocker(zoom_);
    zoom_->setValue(percent);
}

void RibbonWidget::updateTablePresentation() {
    if (!tablePage_ || !tableMore_) return;
    auto* overflow = tableMore_->menu();
    overflow->clear();
    for (auto* button : tableTools_) button->show();
    tableMore_->hide();
    const auto margins = tablePage_->layout()->contentsMargins();
    const int available = width() - margins.left() - margins.right();
    const auto required = [&] {
        int total = 0, count = 0;
        for (auto* button : tableTools_) if (!button->isHidden()) {
            total += button->sizeHint().width();
            ++count;
        }
        if (!tableMore_->isHidden()) { total += tableMore_->sizeHint().width(); ++count; }
        return total + std::max(0, count - 1) * tablePage_->layout()->spacing();
    };
    if (required() <= available) return;
    tableMore_->show();
    // Keep the most frequently used formatting groups available as long as possible.
    for (const int index : {6, 0, 2, 1, 3, 5, 4}) {
        auto* button = tableTools_[index];
        button->hide();
        if (auto* menu = button->menu()) overflow->addMenu(menu);
        else if (auto* action = button->defaultAction()) overflow->addAction(action);
        if (required() <= available) break;
    }
}

void RibbonWidget::updateContextPresentation() {
    if (!contextRow_) return;
    QWidget* active = headerFooterPage_ ? headerFooterPage_.data() : equationContextActive_ ? equationPage_ : pictureContextActive_ ? picturePage_
        : tableContextActive_ ? tablePage_ : listContextActive_ ? listPage_ : nullptr;
    for (auto* page : {listPage_, tablePage_, picturePage_, equationPage_, headerFooterPage_.data()}) {
        if (!page) continue;
        if (page == tablePage_ && page != active)
            for (auto* menu : page->findChildren<QMenu*>()) menu->hide();
        page->setVisible(page == active);
        if (page == active) {
            if (page == tablePage_) updateTablePresentation();
            const auto padding = page->contentsMargins();
            const int layoutWidth = std::max(1, width() - padding.left() - padding.right());
            const int height = page->layout()->hasHeightForWidth()
                ? page->layout()->heightForWidth(layoutWidth) : page->layout()->sizeHint().height();
            page->setFixedHeight(height + padding.top() + padding.bottom());
        }
    }
    contextRow_->setVisible(active != nullptr);
    updateGeometry();
}

void RibbonWidget::setBodyFormattingEnabled(bool enabled) {
    tabs_->setEnabled(enabled);
}

void RibbonWidget::setHeaderFooterContext(QWidget* controls) {
    if (headerFooterPage_ == controls) {
        updateContextPresentation();
        return;
    }
    disconnect(headerFooterDestroyedConnection_);
    if (headerFooterPage_) {
        contextRow_->layout()->removeWidget(headerFooterPage_);
        headerFooterPage_->hide();
    }
    headerFooterPage_ = controls;
    if (controls) {
        controls->setParent(contextRow_);
        controls->setMinimumSize(0, 0);
        controls->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        if (!dynamic_cast<ContextFlowLayout*>(controls->layout())) makeContextFlow(controls);
        contextRow_->layout()->addWidget(controls);
        headerFooterDestroyedConnection_ = connect(controls, &QObject::destroyed, this, [this] {
            updateContextPresentation();
            updateCompactPresentation();
        });
    }
    updateContextPresentation();
    updateCompactPresentation();
}

void RibbonWidget::setListContext(bool visible, int level) {
    listContextActive_ = visible;
    if (listLevel_) {
        const int clamped = std::clamp(level, 1, 10);
        listLevel_->setText(tr("Level %1").arg(clamped));
        listLevel_->setAccessibleDescription(tr("The cursor is in list level %1 of 10").arg(clamped));
    }
    if (tabs_ && listTabIndex_ >= 0) {
        if (!visible && tabs_->currentIndex() == listTabIndex_) tabs_->setCurrentIndex(0);
        tabs_->setTabVisible(listTabIndex_, visible);
    }
    updateContextPresentation();
    updateCompactPresentation();
}

double RibbonWidget::tableBorderWidthPoints() const {
    return tableBorderWidth_->value();
}

QColor RibbonWidget::tableBorderColor() const {
    return tableBorderColor_;
}

void RibbonWidget::setTableDimensions(double columnWidthPoints, double rowMinimumHeightPoints) {
    for (const auto& [spin, points] : {std::pair{tableColumnWidth_, columnWidthPoints},
                                    std::pair{tableRowMinimumHeight_, rowMinimumHeightPoints}}) {
        const QSignalBlocker blocker(spin);
        const bool column = spin == tableColumnWidth_;
        spin->setMinimum(points < 0 ? -1 : column ? 1 : 0);
        spin->setSpecialValueText(points < 0 ? tr("Mixed") : column ? QString{} : tr("Auto"));
        spin->setValue(points < 0 ? -1 : points);
        spin->setProperty("committedValue", spin->value());
    }
}

void RibbonWidget::setTableCellAlignment(std::optional<std::pair<core::ParagraphAlignment,
        core::TableCellVerticalAlignment>> alignment) {
    const char* rows[] = {"top", "middle", "bottom"};
    const char* columns[] = {"Left", "Center", "Right"};
    const auto vertical = alignment ? alignment->second : core::TableCellVerticalAlignment::top;
    const auto horizontal = alignment ? alignment->first : core::ParagraphAlignment::left;
    for (int row = 0; row < 3; ++row) for (int column = 0; column < 3; ++column) {
        auto* button = tablePage_->findChild<QToolButton*>(QStringLiteral("ribbonButton.table.align.%1%2")
            .arg(rows[row], columns[column]));
        if (!button || !button->defaultAction()) continue;
        const auto expectedVertical = row == 0 ? core::TableCellVerticalAlignment::top
            : row == 1 ? core::TableCellVerticalAlignment::center : core::TableCellVerticalAlignment::bottom;
        const auto expectedHorizontal = column == 0 ? core::ParagraphAlignment::left
            : column == 1 ? core::ParagraphAlignment::center : core::ParagraphAlignment::right;
        const QSignalBlocker blocker(button->defaultAction());
        button->defaultAction()->setChecked(alignment && vertical == expectedVertical && horizontal == expectedHorizontal);
    }
}

void RibbonWidget::setTableContext(bool visible) {
    tableContextActive_ = visible;
    if (tablePage_) tablePage_->setEnabled(visible);
    if (tablePage_) for (auto* button : tablePage_->findChildren<QToolButton*>()) {
        if (auto* action = button->defaultAction()) action->setEnabled(visible);
        else button->setEnabled(visible);
    }
    if (tablePage_) for (auto* menu : tablePage_->findChildren<QMenu*>())
        for (auto* action : menu->actions()) action->setEnabled(visible);
    if (tabs_ && tableTabIndex_ >= 0) {
        if (!visible && tabs_->currentIndex() == tableTabIndex_) tabs_->setCurrentIndex(0);
        tabs_->setTabVisible(tableTabIndex_, visible);
    }
    updateContextPresentation();
    updateCompactPresentation();
}

void RibbonWidget::setPictureContext(bool visible, core::ImagePlacement placement, bool editableExcalidraw,
                                     bool floatingLayoutSupported) {
    pictureContextActive_ = visible;
    if (auto* label = picturePage_->findChild<QLabel*>(QStringLiteral("ribbon.pictureContextLabel")))
        label->setText(editableExcalidraw ? tr("Figure") : tr("Picture"));
    if (picturePage_) for (auto* button : picturePage_->findChildren<QToolButton*>()) {
        if (auto* action = button->defaultAction()) action->setEnabled(visible);
        else button->setEnabled(visible);
    }
    for (auto* action : {pictureWrapInline_, pictureWrapSquare_, pictureWrapTopBottom_})
        if (action) action->setEnabled(visible);
    for (auto* action : {pictureWrapSquare_, pictureWrapTopBottom_})
        if (action) action->setEnabled(visible && floatingLayoutSupported);
    if (auto* layoutButton = picturePage_->findChild<QToolButton*>(QStringLiteral("ribbonButton.picture.layoutOptions")))
        if (auto* action = layoutButton->defaultAction()) action->setEnabled(visible && floatingLayoutSupported);
    if (pictureEditExcalidraw_) {
        pictureEditExcalidraw_->setEnabled(visible && editableExcalidraw);
        pictureEditExcalidraw_->setVisible(editableExcalidraw);
        if (auto* edit = picturePage_->findChild<QToolButton*>(QStringLiteral("ribbonButton.picture.editExcalidraw")))
            edit->setVisible(editableExcalidraw);
    }
    for (const auto& [action, target] : {std::pair{pictureWrapInline_, core::ImagePlacement::inline_with_text},
             std::pair{pictureWrapSquare_, core::ImagePlacement::square},
             std::pair{pictureWrapTopBottom_, core::ImagePlacement::top_and_bottom}}) {
        if (!action) continue;
        const QSignalBlocker blocker(action);
        action->setChecked(placement == target);
    }
    if (tabs_ && pictureTabIndex_ >= 0) {
        if (!visible && tabs_->currentIndex() == pictureTabIndex_) tabs_->setCurrentIndex(0);
        tabs_->setTabVisible(pictureTabIndex_, visible);
    }
    updateContextPresentation();
    updateCompactPresentation();
}

void RibbonWidget::setEquationContext(bool visible, bool display) {
    equationContextActive_ = visible;
    for (auto* button : equationPage_->findChildren<QToolButton*>()) {
        auto* action = button->defaultAction();
        if (!action) { button->hide(); continue; }
        action->setEnabled(visible);
        if (action->objectName() == QStringLiteral("equation.inline") ||
            action->objectName() == QStringLiteral("equation.display")) {
            const QSignalBlocker blocker(action);
            action->setChecked(display == action->objectName().endsWith(QStringLiteral("display")));
        }
    }
    updateContextPresentation();
    updateCompactPresentation();
}

}  // namespace docxstudio::app
