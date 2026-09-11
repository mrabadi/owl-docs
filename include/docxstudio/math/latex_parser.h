#pragma once

#include "docxstudio/math/ast.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace docxstudio::math {

struct ParseLimits {
    // These are also hard ceilings. Callers may lower them for a particular
    // surface, but values above these defaults are clamped by parseLatex.
    std::size_t max_input_bytes{64 * 1024};
    std::size_t max_depth{64};
    std::size_t max_nodes{10'000};
    std::size_t max_matrix_rows{256};
    std::size_t max_matrix_columns{256};
};

enum class ParseErrorCode {
    empty_input,
    input_too_large,
    excessive_depth,
    excessive_nodes,
    invalid_utf8,
    unexpected_token,
    unexpected_end,
    unknown_command,
    forbidden_command,
    missing_argument,
    empty_argument,
    duplicate_script,
    mismatched_delimiter,
    invalid_environment,
    matrix_too_large,
};

struct ParseError {
    ParseErrorCode code{ParseErrorCode::unexpected_token};
    std::size_t byte_offset{0};
    std::string message;
};

class ParseResult {
public:
    ParseResult(MathAst ast) : value_(std::move(ast)) {}
    ParseResult(ParseError error) : value_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept {
        return std::holds_alternative<MathAst>(value_);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] const MathAst& value() const { return std::get<MathAst>(value_); }
    [[nodiscard]] const ParseError& error() const { return std::get<ParseError>(value_); }

private:
    std::variant<MathAst, ParseError> value_;
};

// Parses a deliberately inert LaTeX math subset. It performs no macro expansion,
// callbacks, I/O, file lookup, process execution, or environment access.
[[nodiscard]] ParseResult parseLatex(std::string_view source,
                                     const ParseLimits& limits = {});

}  // namespace docxstudio::math
