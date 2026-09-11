#include "docxstudio/app/MathLayout.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace docxstudio::app {
namespace {

struct Box {
    qreal width{};
    qreal ascent{};
    qreal descent{};
};

template <class... Types>
struct Overloaded : Types... {
    using Types::operator()...;
};
template <class... Types>
Overloaded(Types...) -> Overloaded<Types...>;

qreal basePixelSize(const QFont& font) {
    if (font.pixelSize() > 0) return font.pixelSize();
    if (font.pointSizeF() > 0) return font.pointSizeF();
    return 11.0;
}

QFont mathFont(const QFont& base, qreal pixels, bool italic = false) {
    QFont font(base);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixels))));
    font.setItalic(italic);
    return font;
}

QString utf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString mappedToken(const std::string& token) {
    static const std::unordered_map<std::string, QString> symbols{
        {"\\alpha", QStringLiteral("α")}, {"\\beta", QStringLiteral("β")},
        {"\\gamma", QStringLiteral("γ")}, {"\\delta", QStringLiteral("δ")},
        {"\\epsilon", QStringLiteral("ϵ")}, {"\\varepsilon", QStringLiteral("ε")},
        {"\\zeta", QStringLiteral("ζ")}, {"\\eta", QStringLiteral("η")},
        {"\\theta", QStringLiteral("θ")}, {"\\vartheta", QStringLiteral("ϑ")},
        {"\\iota", QStringLiteral("ι")}, {"\\kappa", QStringLiteral("κ")},
        {"\\lambda", QStringLiteral("λ")}, {"\\mu", QStringLiteral("μ")},
        {"\\nu", QStringLiteral("ν")}, {"\\xi", QStringLiteral("ξ")},
        {"\\pi", QStringLiteral("π")}, {"\\varpi", QStringLiteral("ϖ")},
        {"\\rho", QStringLiteral("ρ")}, {"\\sigma", QStringLiteral("σ")},
        {"\\tau", QStringLiteral("τ")}, {"\\upsilon", QStringLiteral("υ")},
        {"\\phi", QStringLiteral("ϕ")}, {"\\varphi", QStringLiteral("φ")},
        {"\\chi", QStringLiteral("χ")}, {"\\psi", QStringLiteral("ψ")},
        {"\\omega", QStringLiteral("ω")}, {"\\Gamma", QStringLiteral("Γ")},
        {"\\Delta", QStringLiteral("Δ")}, {"\\Theta", QStringLiteral("Θ")},
        {"\\Lambda", QStringLiteral("Λ")}, {"\\Xi", QStringLiteral("Ξ")},
        {"\\Pi", QStringLiteral("Π")}, {"\\Sigma", QStringLiteral("Σ")},
        {"\\Upsilon", QStringLiteral("Υ")}, {"\\Phi", QStringLiteral("Φ")},
        {"\\Psi", QStringLiteral("Ψ")}, {"\\Omega", QStringLiteral("Ω")},
        {"\\infty", QStringLiteral("∞")}, {"\\times", QStringLiteral("×")},
        {"\\cdot", QStringLiteral("⋅")}, {"\\pm", QStringLiteral("±")},
        {"\\mp", QStringLiteral("∓")}, {"\\div", QStringLiteral("÷")},
        {"\\le", QStringLiteral("≤")}, {"\\leq", QStringLiteral("≤")},
        {"\\ge", QStringLiteral("≥")}, {"\\geq", QStringLiteral("≥")},
        {"\\neq", QStringLiteral("≠")}, {"\\approx", QStringLiteral("≈")},
        {"\\sim", QStringLiteral("∼")}, {"\\to", QStringLiteral("→")},
        {"\\in", QStringLiteral("∈")}, {"\\notin", QStringLiteral("∉")},
        {"\\subset", QStringLiteral("⊂")}, {"\\subseteq", QStringLiteral("⊆")},
        {"\\cup", QStringLiteral("∪")}, {"\\cap", QStringLiteral("∩")},
        {"\\land", QStringLiteral("∧")}, {"\\lor", QStringLiteral("∨")},
        {"\\partial", QStringLiteral("∂")}, {"\\nabla", QStringLiteral("∇")},
        {"\\{", QStringLiteral("{")}, {"\\}", QStringLiteral("}")},
        {"\\|", QStringLiteral("‖")}, {"\\langle", QStringLiteral("⟨")},
        {"\\rangle", QStringLiteral("⟩")}, {"\\lbrace", QStringLiteral("{")},
        {"\\rbrace", QStringLiteral("}")}, {"\\vert", QStringLiteral("|")},
        {"\\Vert", QStringLiteral("‖")},
    };
    const auto found = symbols.find(token);
    if (found != symbols.end()) return found->second;
    if (token == "\\sin" || token == "\\cos" || token == "\\tan" ||
        token == "\\log" || token == "\\ln" || token == "\\exp" ||
        token == "\\lim") {
        return utf8(token.substr(1));
    }
    return utf8(token);
}

bool isFunctionToken(const std::string& token) {
    return token == "\\sin" || token == "\\cos" || token == "\\tan" ||
           token == "\\log" || token == "\\ln" || token == "\\exp" ||
           token == "\\lim";
}

Box textBox(const QFont& base, qreal size, const QString& text,
            bool italic = false) {
    const QFontMetricsF metrics(mathFont(base, size, italic));
    return {metrics.horizontalAdvance(text), metrics.ascent(), metrics.descent()};
}

Box measureNode(const math::MathNodePtr& node, const QFont& base, qreal size);

Box measureRow(const math::Row& row, const QFont& base, qreal size) {
    Box result;
    const qreal gap = size * 0.07;
    bool first = true;
    for (const auto& child : row.children) {
        const Box box = measureNode(child, base, size);
        if (!first) result.width += gap;
        first = false;
        result.width += box.width;
        result.ascent = std::max(result.ascent, box.ascent);
        result.descent = std::max(result.descent, box.descent);
    }
    return result;
}

std::pair<QString, QString> delimiters(const math::Delimited& value) {
    const auto visible = [](const std::string& token) {
        return token == "." ? QString() : mappedToken(token);
    };
    return {visible(value.left), visible(value.right)};
}

std::pair<QString, QString> matrixDelimiters(
    math::MatrixEnvironment environment) {
    using Environment = math::MatrixEnvironment;
    switch (environment) {
        case Environment::matrix: return {};
        case Environment::pmatrix:
            return {QStringLiteral("("), QStringLiteral(")")};
        case Environment::bmatrix:
            return {QStringLiteral("["), QStringLiteral("]")};
        case Environment::Bmatrix:
            return {QStringLiteral("{"), QStringLiteral("}")};
        case Environment::vmatrix:
            return {QStringLiteral("|"), QStringLiteral("|")};
        case Environment::Vmatrix:
            return {QStringLiteral("‖"), QStringLiteral("‖")};
    }
    return {};
}

Box measureMatrix(const math::Matrix& matrix, const QFont& base, qreal size) {
    const qreal cellSize = size * 0.82;
    const std::size_t columns = std::accumulate(
        matrix.rows.begin(), matrix.rows.end(), std::size_t{},
        [](std::size_t current, const auto& row) {
            return std::max(current, row.size());
        });
    std::vector<qreal> widths(columns, 0.0);
    qreal height = 0.0;
    for (const auto& row : matrix.rows) {
        qreal rowAscent = 0.0;
        qreal rowDescent = 0.0;
        for (std::size_t column = 0; column < row.size(); ++column) {
            const Box box = measureNode(row[column], base, cellSize);
            widths[column] = std::max(widths[column], box.width);
            rowAscent = std::max(rowAscent, box.ascent);
            rowDescent = std::max(rowDescent, box.descent);
        }
        height += std::max(cellSize, rowAscent + rowDescent) + size * 0.18;
    }
    if (!matrix.rows.empty()) height -= size * 0.18;
    qreal width = std::accumulate(widths.begin(), widths.end(), 0.0);
    if (columns > 1) {
        width += static_cast<qreal>(columns - 1) * size * 0.45;
    }
    const auto [left, right] = matrixDelimiters(matrix.environment);
    const qreal delimiterSize = std::max(size, height * 0.82);
    width += textBox(base, delimiterSize, left).width;
    width += textBox(base, delimiterSize, right).width;
    return {width, height * 0.55, height * 0.45};
}

Box measureNode(const math::MathNodePtr& node, const QFont& base, qreal size) {
    if (!node) return {};
    return std::visit(
        Overloaded{
            [&](const math::Row& value) { return measureRow(value, base, size); },
            [&](const math::Group& value) {
                return measureNode(value.body, base, size);
            },
            [&](const math::Identifier& value) {
                return textBox(base, size, mappedToken(value.text),
                               !isFunctionToken(value.text));
            },
            [&](const math::Number& value) {
                return textBox(base, size, utf8(value.text));
            },
            [&](const math::Operator& value) {
                return textBox(base, size, mappedToken(value.text));
            },
            [&](const math::Fraction& value) {
                const qreal childSize = size * 0.76;
                const Box numerator =
                    measureNode(value.numerator, base, childSize);
                const Box denominator =
                    measureNode(value.denominator, base, childSize);
                const qreal pad = size * 0.18;
                const qreal gap = size * 0.12;
                const qreal rule = std::max<qreal>(0.8, size * 0.055);
                return Box{
                    std::max(numerator.width, denominator.width) + 2 * pad,
                    numerator.ascent + numerator.descent + gap + rule / 2,
                    denominator.ascent + denominator.descent + gap + rule / 2};
            },
            [&](const math::Radical& value) {
                const Box body = measureNode(value.radicand, base, size);
                const Box radical =
                    textBox(base, size * 1.2, QStringLiteral("√"));
                qreal width = radical.width + body.width + size * 0.08;
                qreal ascent =
                    std::max(radical.ascent, body.ascent + size * 0.12);
                qreal descent = std::max(radical.descent, body.descent);
                if (value.index && *value.index) {
                    const Box index =
                        measureNode(*value.index, base, size * 0.48);
                    width += index.width * 0.35;
                    ascent =
                        std::max(ascent, body.ascent + index.ascent * 0.75);
                }
                return Box{width, ascent, descent};
            },
            [&](const math::Script& value) {
                const Box body = measureNode(value.base, base, size);
                Box sub;
                Box super;
                if (value.subscript && *value.subscript)
                    sub = measureNode(*value.subscript, base, size * 0.64);
                if (value.superscript && *value.superscript)
                    super =
                        measureNode(*value.superscript, base, size * 0.64);
                return Box{
                    body.width + std::max(sub.width, super.width),
                    std::max(body.ascent,
                             body.ascent * 0.62 + super.ascent +
                                 super.descent),
                    std::max(body.descent,
                             body.descent + sub.ascent +
                                 sub.descent * 0.55)};
            },
            [&](const math::LargeOperator& value) {
                return textBox(
                    base, size * 1.38,
                    value.kind == math::LargeOperatorKind::sum
                        ? QStringLiteral("∑")
                        : QStringLiteral("∫"));
            },
            [&](const math::Delimited& value) {
                const Box body = measureNode(value.body, base, size);
                const auto [left, right] = delimiters(value);
                const qreal delimiterSize =
                    std::max(size, (body.ascent + body.descent) * 1.08);
                const Box leftBox = textBox(base, delimiterSize, left);
                const Box rightBox = textBox(base, delimiterSize, right);
                return Box{
                    leftBox.width + body.width + rightBox.width,
                    std::max({body.ascent, leftBox.ascent, rightBox.ascent}),
                    std::max(
                        {body.descent, leftBox.descent, rightBox.descent})};
            },
            [&](const math::Matrix& value) {
                return measureMatrix(value, base, size);
            }},
        node->value);
}

void drawText(QPainter& painter, const QFont& base, qreal size,
              const QPointF& baseline, const QString& text,
              const QColor& color, bool italic = false) {
    painter.setFont(mathFont(base, size, italic));
    painter.setPen(color);
    painter.drawText(baseline, text);
}

void drawNode(QPainter& painter, const math::MathNodePtr& node,
              const QFont& base, qreal size, const QPointF& baseline,
              const QColor& color);

void drawMatrix(QPainter& painter, const math::Matrix& matrix,
                const QFont& base, qreal size, const QPointF& baseline,
                const QColor& color) {
    const qreal cellSize = size * 0.82;
    const std::size_t columns = std::accumulate(
        matrix.rows.begin(), matrix.rows.end(), std::size_t{},
        [](std::size_t current, const auto& row) {
            return std::max(current, row.size());
        });
    std::vector<qreal> widths(columns, 0.0);
    std::vector<std::pair<qreal, qreal>> rowMetrics;
    qreal totalHeight = 0.0;
    for (const auto& row : matrix.rows) {
        qreal ascent = 0.0;
        qreal descent = 0.0;
        for (std::size_t column = 0; column < row.size(); ++column) {
            const Box box = measureNode(row[column], base, cellSize);
            widths[column] = std::max(widths[column], box.width);
            ascent = std::max(ascent, box.ascent);
            descent = std::max(descent, box.descent);
        }
        const qreal rowHeight = std::max(cellSize, ascent + descent);
        if (ascent + descent < rowHeight) {
            ascent += (rowHeight - ascent - descent) / 2;
            descent = rowHeight - ascent;
        }
        rowMetrics.emplace_back(ascent, descent);
        totalHeight += rowHeight + size * 0.18;
    }
    if (!matrix.rows.empty()) totalHeight -= size * 0.18;

    const auto [left, right] = matrixDelimiters(matrix.environment);
    const qreal delimiterSize = std::max(size, totalHeight * 0.82);
    const Box leftBox = textBox(base, delimiterSize, left);
    qreal x = baseline.x();
    const qreal centerBaseline = baseline.y() + totalHeight * 0.18;
    if (!left.isEmpty()) {
        drawText(painter, base, delimiterSize, QPointF(x, centerBaseline),
                 left, color);
        x += leftBox.width;
    }
    const qreal gridLeft = x;
    qreal top = baseline.y() - totalHeight * 0.55;
    for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
        qreal cellX = gridLeft;
        const qreal rowBaseline = top + rowMetrics[row].first;
        for (std::size_t column = 0; column < columns; ++column) {
            if (column < matrix.rows[row].size()) {
                const Box box =
                    measureNode(matrix.rows[row][column], base, cellSize);
                drawNode(
                    painter, matrix.rows[row][column], base, cellSize,
                    QPointF(cellX + (widths[column] - box.width) / 2,
                            rowBaseline),
                    color);
            }
            cellX += widths[column] + size * 0.45;
        }
        top += rowMetrics[row].first + rowMetrics[row].second +
               size * 0.18;
    }
    qreal gridWidth = std::accumulate(widths.begin(), widths.end(), 0.0);
    if (columns > 1) {
        gridWidth += static_cast<qreal>(columns - 1) * size * 0.45;
    }
    if (!right.isEmpty()) {
        drawText(painter, base, delimiterSize,
                 QPointF(gridLeft + gridWidth, centerBaseline), right, color);
    }
}

void drawNode(QPainter& painter, const math::MathNodePtr& node,
              const QFont& base, qreal size, const QPointF& baseline,
              const QColor& color) {
    if (!node) return;
    std::visit(
        Overloaded{
            [&](const math::Row& value) {
                qreal x = baseline.x();
                const qreal gap = size * 0.07;
                for (std::size_t index = 0; index < value.children.size();
                     ++index) {
                    drawNode(painter, value.children[index], base, size,
                             QPointF(x, baseline.y()), color);
                    x += measureNode(value.children[index], base, size).width;
                    if (index + 1 < value.children.size()) x += gap;
                }
            },
            [&](const math::Group& value) {
                drawNode(painter, value.body, base, size, baseline, color);
            },
            [&](const math::Identifier& value) {
                drawText(painter, base, size, baseline,
                         mappedToken(value.text), color,
                         !isFunctionToken(value.text));
            },
            [&](const math::Number& value) {
                drawText(painter, base, size, baseline, utf8(value.text),
                         color);
            },
            [&](const math::Operator& value) {
                drawText(painter, base, size, baseline,
                         mappedToken(value.text), color);
            },
            [&](const math::Fraction& value) {
                const qreal childSize = size * 0.76;
                const Box numerator =
                    measureNode(value.numerator, base, childSize);
                const Box denominator =
                    measureNode(value.denominator, base, childSize);
                const Box box = measureNode(node, base, size);
                const qreal gap = size * 0.12;
                const qreal rule = std::max<qreal>(0.8, size * 0.055);
                drawNode(
                    painter, value.numerator, base, childSize,
                    QPointF(
                        baseline.x() + (box.width - numerator.width) / 2,
                        baseline.y() - gap - rule / 2 - numerator.descent),
                    color);
                QPen pen(color);
                pen.setWidthF(rule);
                painter.setPen(pen);
                painter.drawLine(QPointF(baseline.x(), baseline.y()),
                                 QPointF(baseline.x() + box.width,
                                         baseline.y()));
                drawNode(
                    painter, value.denominator, base, childSize,
                    QPointF(
                        baseline.x() + (box.width - denominator.width) / 2,
                        baseline.y() + gap + rule / 2 + denominator.ascent),
                    color);
            },
            [&](const math::Radical& value) {
                const Box body = measureNode(value.radicand, base, size);
                const Box radical =
                    textBox(base, size * 1.2, QStringLiteral("√"));
                qreal x = baseline.x();
                if (value.index && *value.index) {
                    drawNode(painter, *value.index, base, size * 0.48,
                             QPointF(x,
                                     baseline.y() - body.ascent * 0.62),
                             color);
                    x += measureNode(*value.index, base, size * 0.48).width *
                         0.35;
                }
                drawText(painter, base, size * 1.2,
                         QPointF(x, baseline.y()), QStringLiteral("√"), color);
                const qreal bodyX = x + radical.width;
                QPen pen(color);
                pen.setWidthF(std::max<qreal>(0.8, size * 0.055));
                painter.setPen(pen);
                painter.drawLine(
                    QPointF(bodyX - size * 0.08,
                            baseline.y() - body.ascent - size * 0.06),
                    QPointF(bodyX + body.width,
                            baseline.y() - body.ascent - size * 0.06));
                drawNode(painter, value.radicand, base, size,
                         QPointF(bodyX, baseline.y()), color);
            },
            [&](const math::Script& value) {
                const Box body = measureNode(value.base, base, size);
                drawNode(painter, value.base, base, size, baseline, color);
                const qreal scriptX = baseline.x() + body.width;
                if (value.superscript && *value.superscript) {
                    const Box script =
                        measureNode(*value.superscript, base, size * 0.64);
                    drawNode(
                        painter, *value.superscript, base, size * 0.64,
                        QPointF(scriptX,
                                baseline.y() - body.ascent * 0.62 -
                                    script.descent),
                        color);
                }
                if (value.subscript && *value.subscript) {
                    const Box script =
                        measureNode(*value.subscript, base, size * 0.64);
                    drawNode(
                        painter, *value.subscript, base, size * 0.64,
                        QPointF(scriptX,
                                baseline.y() + body.descent +
                                    script.ascent * 0.72),
                        color);
                }
            },
            [&](const math::LargeOperator& value) {
                drawText(painter, base, size * 1.38, baseline,
                         value.kind == math::LargeOperatorKind::sum
                             ? QStringLiteral("∑")
                             : QStringLiteral("∫"),
                         color);
            },
            [&](const math::Delimited& value) {
                const Box body = measureNode(value.body, base, size);
                const auto [left, right] = delimiters(value);
                const qreal delimiterSize =
                    std::max(size,
                             (body.ascent + body.descent) * 1.08);
                qreal x = baseline.x();
                if (!left.isEmpty()) {
                    drawText(painter, base, delimiterSize,
                             QPointF(x, baseline.y()), left, color);
                    x += textBox(base, delimiterSize, left).width;
                }
                drawNode(painter, value.body, base, size,
                         QPointF(x, baseline.y()), color);
                x += body.width;
                if (!right.isEmpty()) {
                    drawText(painter, base, delimiterSize,
                             QPointF(x, baseline.y()), right, color);
                }
            },
            [&](const math::Matrix& value) {
                drawMatrix(painter, value, base, size, baseline, color);
            }},
        node->value);
}

}  // namespace

MathLayout::MathLayout(math::MathAst ast, QFont baseFont)
    : ast_(std::move(ast)), baseFont_(std::move(baseFont)) {}

MathLayoutMetrics MathLayout::metrics() const {
    const Box box =
        measureNode(ast_.root, baseFont_, basePixelSize(baseFont_));
    return {box.width, box.ascent, box.descent};
}

void MathLayout::draw(QPainter& painter, const QPointF& baseline,
                      const QColor& color) const {
    if (!valid()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    drawNode(painter, ast_.root, baseFont_, basePixelSize(baseFont_), baseline,
             color);
    painter.restore();
}

}  // namespace docxstudio::app
