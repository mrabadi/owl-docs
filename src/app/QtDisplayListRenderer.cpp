#include "docxstudio/app/QtDisplayListRenderer.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPen>
#include <QString>

#include <algorithm>
#include <type_traits>

namespace docxstudio::app {
namespace {

QColor color(const layout::Color& value) {
    return QColor(value.red, value.green, value.blue, value.alpha);
}

QRectF rect(const layout::Rect& value) {
    return QRectF(value.x, value.y, value.width, value.height);
}

}  // namespace

QtDisplayListRenderer::QtDisplayListRenderer(ImageResolver resolver)
    : imageResolver_(std::move(resolver)) {}

void QtDisplayListRenderer::paint(QPainter& painter,
                                  const layout::PageDisplayList& page,
                                  const QPointF& origin,
                                  double scale) const {
    painter.save();
    painter.translate(origin);
    painter.scale(scale, scale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    for (const auto& command : page.commands) {
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, layout::FillRect>) {
                    painter.fillRect(rect(value.bounds), color(value.color));
                } else if constexpr (std::is_same_v<T, layout::StrokeLine>) {
                    QPen pen(color(value.color));
                    pen.setWidthF(value.width);
                    painter.setPen(pen);
                    painter.drawLine(QPointF(value.x1, value.y1), QPointF(value.x2, value.y2));
                } else if constexpr (std::is_same_v<T, layout::DrawText>) {
                    QFont font(QString::fromStdString(value.style.family));
                    font.setPointSizeF(value.style.point_size);
                    font.setWeight(static_cast<QFont::Weight>(
                        std::clamp(value.style.weight, 1, 1000)));
                    font.setItalic(value.style.italic);
                    font.setUnderline(value.style.underline);
                    font.setStrikeOut(value.style.strike);
                    painter.setFont(font);
                    painter.setPen(color(value.style.foreground));
                    const auto text = QString::fromUtf16(
                        reinterpret_cast<const char16_t*>(value.text.data()),
                        static_cast<qsizetype>(value.text.size()));
                    if (value.style.highlight.alpha != 0) {
                        const QFontMetricsF metrics(font);
                        painter.fillRect(QRectF(value.x, value.baseline_y - metrics.ascent(),
                                                metrics.horizontalAdvance(text), metrics.height()),
                                         color(value.style.highlight));
                    }
                    painter.drawText(QPointF(value.x, value.baseline_y), text);
                } else if constexpr (std::is_same_v<T, layout::DrawImage>) {
                    if (imageResolver_) {
                        const auto image = imageResolver_(value.asset_id);
                        if (!image.isNull()) {
                            painter.drawImage(rect(value.bounds), image);
                        }
                    }
                }
            },
            command);
    }
    painter.restore();
}

}  // namespace docxstudio::app
