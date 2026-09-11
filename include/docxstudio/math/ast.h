#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace docxstudio::math {

struct MathNode;
using MathNodePtr = std::shared_ptr<const MathNode>;

struct Row {
    std::vector<MathNodePtr> children;
};

struct Group {
    MathNodePtr body;
};

struct Identifier {
    // UTF-8 text, or a whitelisted LaTeX symbol command including its '\\'.
    std::string text;
};

struct Number {
    std::string text;
};

struct Operator {
    std::string text;
};

struct Fraction {
    MathNodePtr numerator;
    MathNodePtr denominator;
};

struct Radical {
    std::optional<MathNodePtr> index;
    MathNodePtr radicand;
};

struct Script {
    MathNodePtr base;
    std::optional<MathNodePtr> subscript;
    std::optional<MathNodePtr> superscript;
};

enum class LargeOperatorKind { sum, integral };

struct LargeOperator {
    LargeOperatorKind kind{LargeOperatorKind::sum};
};

struct Delimited {
    // Canonical delimiter token, such as "(", ".", "\\langle", or "\\{".
    std::string left;
    std::string right;
    MathNodePtr body;
};

enum class MatrixEnvironment { matrix, pmatrix, bmatrix, Bmatrix, vmatrix, Vmatrix };

struct Matrix {
    MatrixEnvironment environment{MatrixEnvironment::matrix};
    // Each cell is normally a Row node. Empty cells are represented by an empty Row.
    std::vector<std::vector<MathNodePtr>> rows;
};

using MathNodeValue = std::variant<Row, Group, Identifier, Number, Operator, Fraction, Radical,
                                   Script, LargeOperator, Delimited, Matrix>;

struct MathNode {
    explicit MathNode(MathNodeValue node_value) : value(std::move(node_value)) {}
    MathNodeValue value;
};

struct MathAst {
    MathNodePtr root;
};

[[nodiscard]] std::string toCanonicalLatex(const MathAst& ast);
[[nodiscard]] std::string toCanonicalLatex(const MathNodePtr& node);

}  // namespace docxstudio::math
