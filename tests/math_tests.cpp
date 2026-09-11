#include "docxstudio/math/latex_parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace docxstudio::math;

namespace {

int failures = 0;

#define CHECK(expression)                                                                  \
    do {                                                                                   \
        if (!(expression)) {                                                               \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression   \
                      << '\n';                                                             \
            ++failures;                                                                    \
        }                                                                                  \
    } while (false)

std::string canonical(std::string_view source) {
    const auto result = parseLatex(source);
    if (!result) {
        std::cerr << "Unexpected parse error at " << result.error().byte_offset << ": "
                  << result.error().message << '\n';
        ++failures;
        return {};
    }
    return toCanonicalLatex(result.value());
}

void expectError(std::string_view source, ParseErrorCode expected,
                 const ParseLimits& limits = {}) {
    const auto result = parseLatex(source, limits);
    CHECK(!result);
    if (!result) {
        CHECK(result.error().code == expected);
        CHECK(!result.error().message.empty());
        CHECK(result.error().byte_offset <= source.size());
    }
}

void testAtomsGroupsAndScripts() {
    CHECK(canonical("a+12.5") == "a + 12.5");
    CHECK(canonical("{a+b} c") == "{a + b} c");
    CHECK(canonical("x^2_1") == "x_{1}^{2}");
    CHECK(canonical("\\sum_{i=1}^{n} i") == "\\sum_{i = 1}^{n} i");
    CHECK(canonical("\\int_0^1 x") == "\\int_{0}^{1} x");
    CHECK(canonical("\\alpha + Î²") == "\\alpha + Î²");

    const auto first = canonical("x^2_1+1");
    const auto second = canonical(first);
    CHECK(first == second);
}

void testFractionsRadicalsAndDelimiters() {
    CHECK(canonical("\\frac{a_1}{\\sqrt[3]{x^2}}") ==
          "\\frac{a_{1}}{\\sqrt[3]{x^{2}}}");
    CHECK(canonical("\\sqrt{x+1}") == "\\sqrt{x + 1}");
    CHECK(canonical("\\left(\\frac{a}{b}\\right)") ==
          "\\left( \\frac{a}{b} \\right)");
    CHECK(canonical("\\left\\langle x \\right\\rangle") ==
          "\\left\\langle x \\right\\rangle");
    CHECK(canonical("\\left. x \\right|") == "\\left. x \\right|");
}

void testMatrices() {
    constexpr std::string_view input =
        "\\begin{pmatrix}a&b\\\\c&\\frac{d}{e}\\end{pmatrix}";
    const auto parsed = parseLatex(input);
    CHECK(parsed);
    if (parsed) {
        CHECK(toCanonicalLatex(parsed.value()) ==
              "\\begin{pmatrix}a & b \\\\ c & \\frac{d}{e}\\end{pmatrix}");
        const auto* root = std::get_if<Row>(&parsed.value().root->value);
        CHECK(root != nullptr);
        if (root) {
            CHECK(root->children.size() == 1);
        }
        if (root && root->children.size() == 1) {
            const auto* matrix = std::get_if<Matrix>(&root->children.front()->value);
            CHECK(matrix != nullptr);
            if (matrix) {
                CHECK(matrix->environment == MatrixEnvironment::pmatrix);
                CHECK(matrix->rows.size() == 2);
                CHECK(matrix->rows[0].size() == 2);
            }
        }
    }

    CHECK(canonical("\\begin{bmatrix}1&0\\\\0&1\\\\\\end{bmatrix}") ==
          "\\begin{bmatrix}1 & 0 \\\\ 0 & 1\\end{bmatrix}");
}

void testStrictRejection() {
    expectError("\\unknown{x}", ParseErrorCode::unknown_command);
    expectError("x^^2", ParseErrorCode::missing_argument);
    expectError("x^1^2", ParseErrorCode::duplicate_script);
    expectError("\\frac{a}", ParseErrorCode::missing_argument);
    expectError("\\sqrt[]{}", ParseErrorCode::empty_argument);
    expectError("\\left(x", ParseErrorCode::mismatched_delimiter);
    expectError("\\right)x", ParseErrorCode::mismatched_delimiter);
    expectError("\\begin{align}a\\end{align}", ParseErrorCode::invalid_environment);
    expectError("\\begin{matrix}a\\end{pmatrix}", ParseErrorCode::invalid_environment);
    expectError("a&b", ParseErrorCode::unexpected_token);
    expectError("a\\\\b", ParseErrorCode::unexpected_token);
    expectError("x% hidden", ParseErrorCode::forbidden_command);
    expectError("x#1", ParseErrorCode::forbidden_command);

    const std::vector<std::string> forbidden{
        "\\def\\x{1}",
        "\\newcommand{\\x}{1}",
        "\\input{/etc/passwd}",
        "\\include{secret}",
        "\\includegraphics{secret.png}",
        "\\write18{command}",
        "\\shellescape{command}",
        "\\usepackage{anything}",
        "\\catcode`x=1",
    };
    for (const auto& source : forbidden) {
        expectError(source, ParseErrorCode::forbidden_command);
    }

    std::string invalid_utf8{"x"};
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0xaf));
    expectError(invalid_utf8, ParseErrorCode::invalid_utf8);
}

void testResourceLimits() {
    ParseLimits limits;
    limits.max_input_bytes = 4;
    expectError("12345", ParseErrorCode::input_too_large, limits);

    limits = {};
    limits.max_depth = 2;
    expectError("{{{x}}}", ParseErrorCode::excessive_depth, limits);

    limits = {};
    limits.max_nodes = 2;
    expectError("a+b", ParseErrorCode::excessive_nodes, limits);

    limits = {};
    limits.max_matrix_columns = 2;
    expectError("\\begin{matrix}a&b&c\\end{matrix}", ParseErrorCode::matrix_too_large,
                limits);

    limits = {};
    limits.max_matrix_rows = 1;
    expectError("\\begin{matrix}a\\\\b\\end{matrix}", ParseErrorCode::matrix_too_large,
                limits);
}

}  // namespace

int main() {
    testAtomsGroupsAndScripts();
    testFractionsRadicalsAndDelimiters();
    testMatrices();
    testStrictRejection();
    testResourceLimits();

    if (failures != 0) {
        std::cerr << failures << " math test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All math tests passed\n";
    return EXIT_SUCCESS;
}
