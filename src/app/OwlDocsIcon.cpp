#include "docxstudio/app/OwlDocsIcon.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QString>

#include <array>

namespace docxstudio::app {
namespace {

QIcon nativeOwlIcon() {
    // A code-drawn fallback matching the packaged low-poly artwork. The normal
    // path uses hand-tuned PNGs at every common launcher size.
    QPixmap image(512, 512);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(4.0, 4.0);
    painter.setPen(Qt::NoPen);

    const QColor orange(QStringLiteral("#E95420"));
    const QColor aubergine(QStringLiteral("#77216F"));
    const QColor deepAubergine(QStringLiteral("#5E2750"));
    const QColor ivory(QStringLiteral("#FFF7F2"));

    painter.setBrush(deepAubergine);
    painter.drawRoundedRect(QRectF(4, 4, 120, 120), 27, 27);
    painter.setBrush(QColor(QStringLiteral("#77216F")));
    painter.setOpacity(0.58);
    painter.drawPolygon(QPolygonF{QPointF(31, 4), QPointF(97, 4),
                                  QPointF(124, 31), QPointF(124, 97),
                                  QPointF(97, 124), QPointF(31, 124),
                                  QPointF(4, 97), QPointF(4, 31)});
    painter.setOpacity(1.0);

    QPainterPath silhouette;
    silhouette.moveTo(23, 18);
    silhouette.lineTo(45, 28);
    silhouette.lineTo(64, 18);
    silhouette.lineTo(83, 28);
    silhouette.lineTo(105, 18);
    silhouette.lineTo(100, 43);
    silhouette.lineTo(109, 57);
    silhouette.lineTo(105, 91);
    silhouette.lineTo(88, 111);
    silhouette.lineTo(64, 122);
    silhouette.lineTo(40, 111);
    silhouette.lineTo(23, 91);
    silhouette.lineTo(19, 57);
    silhouette.lineTo(28, 43);
    silhouette.closeSubpath();
    QPen silhouettePen(ivory);
    silhouettePen.setWidthF(3.5);
    silhouettePen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(silhouettePen);
    painter.setBrush(orange);
    painter.drawPath(silhouette);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#F58251")));
    painter.drawPolygon(QPolygonF{QPointF(23, 18), QPointF(45, 28),
                                  QPointF(28, 43)});
    painter.setBrush(QColor(QStringLiteral("#C34113")));
    painter.drawPolygon(QPolygonF{QPointF(105, 18), QPointF(83, 28),
                                  QPointF(100, 43)});
    painter.setBrush(QColor(QStringLiteral("#F36B3D")));
    painter.drawPolygon(QPolygonF{QPointF(19, 57), QPointF(28, 43),
                                  QPointF(39, 72), QPointF(23, 91)});
    painter.setBrush(QColor(QStringLiteral("#B7370D")));
    painter.drawPolygon(QPolygonF{QPointF(109, 57), QPointF(100, 43),
                                  QPointF(89, 72), QPointF(105, 91)});
    painter.setBrush(QColor(QStringLiteral("#D74817")));
    painter.drawPolygon(QPolygonF{QPointF(23, 91), QPointF(39, 72),
                                  QPointF(48, 108), QPointF(40, 111)});
    painter.setBrush(QColor(QStringLiteral("#F57946")));
    painter.drawPolygon(QPolygonF{QPointF(105, 91), QPointF(89, 72),
                                  QPointF(80, 108), QPointF(88, 111)});
    painter.setBrush(QColor(QStringLiteral("#F47A47")));
    painter.drawPolygon(QPolygonF{QPointF(40, 111), QPointF(48, 75),
                                  QPointF(64, 122)});
    painter.setBrush(QColor(QStringLiteral("#C53B10")));
    painter.drawPolygon(QPolygonF{QPointF(88, 111), QPointF(80, 75),
                                  QPointF(64, 122)});

    painter.setBrush(aubergine);
    painter.drawPolygon(QPolygonF{QPointF(28, 43), QPointF(45, 28),
                                  QPointF(64, 38), QPointF(52, 71),
                                  QPointF(39, 72)});
    painter.setBrush(deepAubergine);
    painter.drawPolygon(QPolygonF{QPointF(100, 43), QPointF(83, 28),
                                  QPointF(64, 38), QPointF(76, 71),
                                  QPointF(89, 72)});
    painter.setBrush(QColor(QStringLiteral("#6B2356")));
    painter.drawPolygon(QPolygonF{QPointF(64, 38), QPointF(52, 71),
                                  QPointF(64, 87), QPointF(76, 71)});

    painter.setBrush(ivory);
    painter.drawPolygon(QPolygonF{QPointF(31, 49), QPointF(46, 36),
                                  QPointF(61, 46), QPointF(56, 64),
                                  QPointF(39, 68), QPointF(29, 58)});
    painter.drawPolygon(QPolygonF{QPointF(97, 49), QPointF(82, 36),
                                  QPointF(67, 46), QPointF(72, 64),
                                  QPointF(89, 68), QPointF(99, 58)});
    painter.setBrush(QColor(QStringLiteral("#2C001E")));
    painter.drawPolygon(QPolygonF{QPointF(47, 43), QPointF(56, 51),
                                  QPointF(52, 61), QPointF(42, 63),
                                  QPointF(36, 56)});
    painter.drawPolygon(QPolygonF{QPointF(81, 43), QPointF(72, 51),
                                  QPointF(76, 61), QPointF(86, 63),
                                  QPointF(92, 56)});

    QPen beakPen(ivory);
    beakPen.setWidthF(2.0);
    beakPen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(beakPen);
    painter.setBrush(QColor(QStringLiteral("#F6A077")));
    painter.drawPolygon(QPolygonF{QPointF(64, 57), QPointF(74, 66),
                                  QPointF(64, 79), QPointF(54, 66)});

    painter.setPen(Qt::NoPen);
    painter.setBrush(aubergine);
    painter.drawPolygon(QPolygonF{QPointF(39, 72), QPointF(64, 87),
                                  QPointF(89, 72), QPointF(80, 108),
                                  QPointF(64, 122), QPointF(48, 108)});
    painter.setBrush(QColor(QStringLiteral("#6A1F55")));
    painter.drawPolygon(QPolygonF{QPointF(39, 72), QPointF(64, 87),
                                  QPointF(48, 108)});
    painter.setBrush(QColor(QStringLiteral("#48133F")));
    painter.drawPolygon(QPolygonF{QPointF(89, 72), QPointF(64, 87),
                                  QPointF(80, 108)});
    painter.setBrush(deepAubergine);
    painter.drawPolygon(QPolygonF{QPointF(48, 108), QPointF(64, 87),
                                  QPointF(80, 108), QPointF(64, 122)});

    QPen facetPen(ivory);
    facetPen.setWidthF(2.0);
    facetPen.setCapStyle(Qt::RoundCap);
    facetPen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(facetPen);
    painter.setOpacity(0.72);
    QPainterPath facets;
    facets.moveTo(28, 43);
    facets.lineTo(64, 18);
    facets.lineTo(100, 43);
    facets.moveTo(19, 57);
    facets.lineTo(39, 72);
    facets.lineTo(64, 87);
    facets.lineTo(89, 72);
    facets.lineTo(109, 57);
    facets.moveTo(23, 91);
    facets.lineTo(48, 108);
    facets.lineTo(64, 122);
    facets.lineTo(80, 108);
    facets.lineTo(105, 91);
    facets.moveTo(45, 28);
    facets.lineTo(39, 72);
    facets.moveTo(83, 28);
    facets.lineTo(89, 72);
    painter.drawPath(facets);

    return QIcon(image);
}

QIcon embeddedOwlIcon() {
    constexpr std::array<int, 8> sizes{16, 24, 32, 48, 64, 128, 256, 512};
    QIcon icon;
    for (const int size : sizes) {
        const QPixmap pixmap(
            QStringLiteral(":/icons/owl-docs-%1.png").arg(size));
        if (pixmap.isNull() || pixmap.size() != QSize(size, size)) return {};
        icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    }
    return icon;
}

}  // namespace

QIcon owlDocsApplicationIcon() {
    const QIcon embedded = embeddedOwlIcon();
    return embedded.isNull() ? nativeOwlIcon() : embedded;
}

}  // namespace docxstudio::app
