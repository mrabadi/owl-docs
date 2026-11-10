#include "docxstudio/math/latex_parser.h"
#include "docxstudio/ooxml/docx_document.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

int main() {
    using namespace docxstudio;
    std::string pattern = "/tmp/owl-math-roundtrip-XXXXXX";
    const char* directory = mkdtemp(pattern.data());
    if (!directory) return 1;
    const auto path = std::filesystem::path(directory) / "equations.docx";
    const std::vector<std::string> sources{
        R"(\text{when x is positive})",
        R"(\mathrm{AB}+\mathbf{x}+\mathit{y}+\operatorname{rank})",
        R"(\hat{x}+\vec{v}+\bar{z}+\overline{AB}+\underline{CD})",
        R"(\prod_{i=1}^{n} i\,x\quad y\!z+\dfrac{a}{b}+\tfrac{c}{d})",
        R"(\begin{aligned}a&=b\\c&=d\end{aligned})",
        R"(\begin{cases}x & \text{if positive}\\0 & \text{otherwise}\end{cases})",
        R"(\left\{\begin{matrix}a&b\\c&d\end{matrix}\right.)",
        R"(\begin{cases}x&\text{positive}\\0\end{cases})",
        R"(\begin{matrix}a\\b&c\end{matrix})",
        R"(\begin{aligned}a&=b\\c\end{aligned})",
    };
    for (const auto& source : sources) {
        const auto parsed = math::parseLatex(source);
        if (!parsed) { std::cerr << "fixture did not parse: " << source << '\n'; return 1; }
        const auto canonical = math::toCanonicalLatex(parsed.value());
        ooxml::NewParagraph paragraph;
        paragraph.runs.emplace_back("", ooxml::BasicRunFormat{}, ooxml::EquationPayload{canonical, false});
        const auto result = ooxml::DocxDocument::writeNew(path, {paragraph});
        if (!result.saved) { std::cerr << "equation could not save: " << source << '\n'; return 1; }
        ooxml::Error error;
        const auto reopened = ooxml::DocxDocument::open(path, &error);
        if (!reopened || reopened->paragraphs().empty() || reopened->paragraphs()[0].runs.empty() ||
            reopened->paragraphs()[0].runs[0].fragments.empty() ||
            !reopened->paragraphs()[0].runs[0].fragments[0].equation ||
            reopened->paragraphs()[0].runs[0].fragments[0].equation->canonical_latex != canonical) {
            std::cerr << "native math source changed on reopen: " << canonical << '\n';
            return 1;
        }
    }
    std::filesystem::remove_all(directory);
    std::cout << "extended math DOCX roundtrip tests passed\n";
}
