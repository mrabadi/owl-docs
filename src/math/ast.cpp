#include "docxstudio/math/ast.h"

#include <type_traits>

namespace docxstudio::math {
namespace {

template <class... Types>
struct Overloaded : Types... {
    using Types::operator()...;
};
template <class... Types>
Overloaded(Types...) -> Overloaded<Types...>;

const char* environmentName(MatrixEnvironment environment) noexcept {
    switch (environment) {
        case MatrixEnvironment::matrix:
            return "matrix";
        case MatrixEnvironment::pmatrix:
            return "pmatrix";
        case MatrixEnvironment::bmatrix:
            return "bmatrix";
        case MatrixEnvironment::Bmatrix:
            return "Bmatrix";
        case MatrixEnvironment::vmatrix:
            return "vmatrix";
        case MatrixEnvironment::Vmatrix:
            return "Vmatrix";
    }
    return "matrix";
}

std::string serialize(const MathNodePtr& node) {
    if (!node) {
        return {};
    }
    return std::visit(
        Overloaded{
            [](const Row& row) {
                std::string result;
                for (const auto& child : row.children) {
                    const auto serialized = serialize(child);
                    if (serialized.empty()) {
                        continue;
                    }
                    if (!result.empty()) {
                        result.push_back(' ');
                    }
                    result.append(serialized);
                }
                return result;
            },
            [](const Group& group) { return "{" + serialize(group.body) + "}"; },
            [](const Identifier& identifier) { return identifier.text; },
            [](const Number& number) { return number.text; },
            [](const Operator& operation) { return operation.text; },
            [](const Fraction& fraction) {
                return "\\frac{" + serialize(fraction.numerator) + "}{" +
                       serialize(fraction.denominator) + "}";
            },
            [](const Radical& radical) {
                std::string result = "\\sqrt";
                if (radical.index && *radical.index) {
                    result += "[" + serialize(*radical.index) + "]";
                }
                result += "{" + serialize(radical.radicand) + "}";
                return result;
            },
            [](const Script& script) {
                std::string result = serialize(script.base);
                // A fixed subscript-then-superscript order makes equivalent input canonical.
                if (script.subscript && *script.subscript) {
                    result += "_{" + serialize(*script.subscript) + "}";
                }
                if (script.superscript && *script.superscript) {
                    result += "^{" + serialize(*script.superscript) + "}";
                }
                return result;
            },
            [](const LargeOperator& operation) {
                return std::string(operation.kind == LargeOperatorKind::sum ? "\\sum" : "\\int");
            },
            [](const Delimited& delimited) {
                const auto body = serialize(delimited.body);
                return "\\left" + delimited.left + (body.empty() ? "" : " " + body + " ") +
                       "\\right" + delimited.right;
            },
            [](const Matrix& matrix) {
                const std::string environment = environmentName(matrix.environment);
                std::string result = "\\begin{" + environment + "}";
                for (std::size_t row = 0; row < matrix.rows.size(); ++row) {
                    if (row != 0) {
                        result += " \\\\ ";
                    }
                    for (std::size_t column = 0; column < matrix.rows[row].size(); ++column) {
                        if (column != 0) {
                            result += " & ";
                        }
                        result += serialize(matrix.rows[row][column]);
                    }
                }
                result += "\\end{" + environment + "}";
                return result;
            }},
        node->value);
}

}  // namespace

std::string toCanonicalLatex(const MathAst& ast) {
    return serialize(ast.root);
}

std::string toCanonicalLatex(const MathNodePtr& node) {
    return serialize(node);
}

}  // namespace docxstudio::math
