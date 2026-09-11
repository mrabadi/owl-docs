#pragma once

#include "docxstudio/math/ast.h"

#include <QColor>
#include <QFont>
#include <QPointF>

class QPainter;

namespace docxstudio::app {

struct MathLayoutMetrics {
    qreal width{};
    qreal ascent{};
    qreal descent{};

    [[nodiscard]] qreal height() const noexcept { return ascent + descent; }
};

// Inert vector typesetting for the exact MathAst subset accepted by our safe
// LaTeX parser. Screen, print, and PDF all use this same painter path.
class MathLayout final {
public:
    MathLayout(math::MathAst ast, QFont baseFont);

    [[nodiscard]] bool valid() const noexcept { return ast_.root != nullptr; }
    [[nodiscard]] MathLayoutMetrics metrics() const;
    void draw(QPainter& painter, const QPointF& baseline,
              const QColor& color = Qt::black) const;

private:
    math::MathAst ast_;
    QFont baseFont_;
};

}  // namespace docxstudio::app
