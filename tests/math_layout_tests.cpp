#include "docxstudio/app/MathLayout.h"
#include "docxstudio/math/latex_parser.h"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void renderAndCheck(std::string_view latex) {
    const auto parsed = docxstudio::math::parseLatex(latex);
    check(static_cast<bool>(parsed), "math layout fixture did not parse");
    QFont font(QStringLiteral("Carlito"));
    font.setPixelSize(24);
    const docxstudio::app::MathLayout layout(parsed.value(), font);
    const auto metrics = layout.metrics();
    check(layout.valid() && std::isfinite(metrics.width) &&
              std::isfinite(metrics.ascent) &&
              std::isfinite(metrics.descent) && metrics.width > 0.0 &&
              metrics.height() > 0.0 && metrics.width < 4096.0 &&
              metrics.height() < 4096.0,
          "math layout returned invalid or unbounded geometry");

    const int imageWidth = static_cast<int>(std::ceil(metrics.width)) + 80;
    const int imageHeight = static_cast<int>(std::ceil(metrics.height())) + 80;
    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    layout.draw(painter, QPointF(40.0, 40.0 + metrics.ascent), Qt::black);
    painter.end();
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 0) ++painted;
        }
    }
    check(painted > 8, "math layout produced no visible vector artwork");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const std::vector<std::string_view> expressions{
        R"(x+\frac{a}{b})",
        R"(\sqrt[3]{x^2+y_1})",
        R"(\sum_{i=1}^{n} i \times \int_0^1 x)",
        R"(\left\langle \alpha+\beta \right\rangle)",
        R"(\begin{matrix}a&b\\c&d\end{matrix})",
        R"(\begin{pmatrix}1&2\\3&4\end{pmatrix})",
        R"(\begin{bmatrix}1&2\\3&4\end{bmatrix})",
        R"(\begin{Bmatrix}1&2\\3&4\end{Bmatrix})",
        R"(\begin{vmatrix}1&2\\3&4\end{vmatrix})",
        R"(\begin{Vmatrix}1&2\\3&4\end{Vmatrix})",
    };
    for (const auto expression : expressions) renderAndCheck(expression);
    std::cout << "math layout tests passed\n";
    return 0;
}
