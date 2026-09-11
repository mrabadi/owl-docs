#include "docxstudio/math/latex_parser.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace docxstudio::math {
namespace {

struct ParseFailure {
    ParseError error;
};

std::optional<std::size_t> invalidUtf8Offset(std::string_view source) noexcept {
    for (std::size_t index = 0; index < source.size();) {
        const auto first = static_cast<unsigned char>(source[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            return index;
        }
        if (index + length > source.size()) {
            return index;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(source[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return index + continuation;
            }
        }

        const auto second = static_cast<unsigned char>(source[index + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
            return index;
        }
        index += length;
    }
    return std::nullopt;
}

ParseLimits effectiveLimits(const ParseLimits& requested) noexcept {
    constexpr ParseLimits hard{};
    return ParseLimits{
        std::min(requested.max_input_bytes, hard.max_input_bytes),
        std::min(requested.max_depth, hard.max_depth),
        std::min(requested.max_nodes, hard.max_nodes),
        std::min(requested.max_matrix_rows, hard.max_matrix_rows),
        std::min(requested.max_matrix_columns, hard.max_matrix_columns),
    };
}

bool isAsciiLetter(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool isEmptyRow(const MathNodePtr& node) {
    if (!node) {
        return true;
    }
    if (const auto* row = std::get_if<Row>(&node->value)) {
        return row->children.empty();
    }
    return false;
}

class Parser {
public:
    Parser(std::string_view source, ParseLimits limits) : source_(source), limits_(limits) {}

    MathAst parse() {
        skipWhitespace();
        if (eof()) {
            fail(ParseErrorCode::empty_input, "Equation is empty");
        }
        auto root = parseSequence(Stop::end);
        skipWhitespace();
        if (!eof()) {
            fail(ParseErrorCode::unexpected_token, "Unexpected token");
        }
        if (isEmptyRow(root)) {
            fail(ParseErrorCode::empty_input, "Equation is empty", 0);
        }
        return MathAst{std::move(root)};
    }

private:
    enum class Stop { end, closing_brace, closing_bracket, right_command, matrix_cell };

    class DepthScope {
    public:
        explicit DepthScope(Parser& parser) : parser_(parser) { parser_.enterDepth(); }
        ~DepthScope() { --parser_.depth_; }

        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;

    private:
        Parser& parser_;
    };

    [[noreturn]] void fail(ParseErrorCode code, std::string message,
                           std::optional<std::size_t> offset = std::nullopt) const {
        throw ParseFailure{ParseError{code, offset.value_or(position_), std::move(message)}};
    }

    void enterDepth() {
        if (depth_ >= limits_.max_depth) {
            fail(ParseErrorCode::excessive_depth, "Maximum expression nesting depth exceeded");
        }
        ++depth_;
    }

    template <typename Value>
    MathNodePtr makeNode(Value value) {
        if (node_count_ >= limits_.max_nodes) {
            fail(ParseErrorCode::excessive_nodes, "Maximum AST node count exceeded");
        }
        ++node_count_;
        return std::make_shared<const MathNode>(MathNodeValue{std::move(value)});
    }

    bool eof() const noexcept { return position_ >= source_.size(); }
    char peek() const noexcept { return eof() ? '\0' : source_[position_]; }

    void skipWhitespace() noexcept {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            ++position_;
        }
    }

    bool atRowBreak() const noexcept {
        return position_ + 1 < source_.size() && source_[position_] == '\\' &&
               source_[position_ + 1] == '\\';
    }

    bool atCommand(std::string_view command) const noexcept {
        if (eof() || source_[position_] != '\\' ||
            position_ + 1 + command.size() > source_.size() ||
            source_.substr(position_ + 1, command.size()) != command) {
            return false;
        }
        const auto end = position_ + 1 + command.size();
        return end == source_.size() || !isAsciiLetter(source_[end]);
    }

    std::string readCommand() {
        const auto command_offset = position_;
        if (eof() || peek() != '\\') {
            fail(ParseErrorCode::unexpected_token, "Expected a LaTeX command");
        }
        ++position_;
        if (eof()) {
            fail(ParseErrorCode::unexpected_end, "Trailing backslash", command_offset);
        }
        if (!isAsciiLetter(peek())) {
            std::string command(1, peek());
            ++position_;
            return command;
        }

        const auto start = position_;
        while (!eof() && isAsciiLetter(peek())) {
            ++position_;
        }
        return std::string(source_.substr(start, position_ - start));
    }

    MathNodePtr parseSequence(Stop stop) {
        std::vector<MathNodePtr> children;
        while (true) {
            skipWhitespace();
            if (eof()) {
                break;
            }
            if (stop == Stop::closing_brace && peek() == '}') {
                break;
            }
            if (stop == Stop::closing_bracket && peek() == ']') {
                break;
            }
            if (stop == Stop::right_command && atCommand("right")) {
                break;
            }
            if (stop == Stop::matrix_cell &&
                (peek() == '&' || atRowBreak() || atCommand("end"))) {
                break;
            }

            if (peek() == '}') {
                fail(ParseErrorCode::unexpected_token, "Unmatched closing brace");
            }
            if (peek() == '&') {
                fail(ParseErrorCode::unexpected_token,
                     "Matrix column separator is only valid inside a matrix");
            }
            if (atRowBreak()) {
                fail(ParseErrorCode::unexpected_token,
                     "Matrix row separator is only valid inside a matrix");
            }
            children.push_back(parsePostfixedAtom());
        }
        return makeNode(Row{std::move(children)});
    }

    MathNodePtr parsePostfixedAtom() {
        auto base = parseAtom();
        std::optional<MathNodePtr> subscript;
        std::optional<MathNodePtr> superscript;

        while (true) {
            skipWhitespace();
            if (peek() != '_' && peek() != '^') {
                break;
            }
            const auto marker = peek();
            const auto marker_offset = position_++;
            if ((marker == '_' && subscript) || (marker == '^' && superscript)) {
                fail(ParseErrorCode::duplicate_script,
                     marker == '_' ? "Duplicate subscript" : "Duplicate superscript",
                     marker_offset);
            }
            auto argument = parseScriptArgument();
            if (marker == '_') {
                subscript = std::move(argument);
            } else {
                superscript = std::move(argument);
            }
        }

        if (subscript || superscript) {
            return makeNode(Script{std::move(base), std::move(subscript), std::move(superscript)});
        }
        return base;
    }

    MathNodePtr parseScriptArgument() {
        skipWhitespace();
        if (eof()) {
            fail(ParseErrorCode::missing_argument, "Script marker requires an argument");
        }
        if (peek() == '{') {
            auto argument = parseRequiredContent("script");
            if (isEmptyRow(argument)) {
                fail(ParseErrorCode::empty_argument, "Script argument cannot be empty");
            }
            return argument;
        }
        if (peek() == '_' || peek() == '^' || peek() == '}' || peek() == '&') {
            fail(ParseErrorCode::missing_argument, "Script marker requires an atom or group");
        }
        return parseAtom();
    }

    MathNodePtr parseAtom() {
        skipWhitespace();
        if (eof()) {
            fail(ParseErrorCode::unexpected_end, "Expected a math atom");
        }

        const auto character = static_cast<unsigned char>(peek());
        if (peek() == '{') {
            return parseExplicitGroup();
        }
        if (peek() == '\\') {
            return parseCommand();
        }
        if (peek() == '_' || peek() == '^') {
            fail(ParseErrorCode::unexpected_token, "Script marker has no base");
        }
        if (std::isdigit(character) != 0) {
            return parseNumber();
        }
        if (isAsciiLetter(peek()) || character >= 0x80) {
            return parseIdentifier();
        }
        constexpr std::string_view operators = "+-=*/(),.!<>|:;'[]";
        if (operators.find(peek()) != std::string_view::npos) {
            std::string operation(1, peek());
            ++position_;
            return makeNode(Operator{std::move(operation)});
        }
        if (peek() == '%' || peek() == '#' || peek() == '$' || peek() == '~') {
            fail(ParseErrorCode::forbidden_command,
                 "Comments, macro parameters, math delimiters, and active characters are not allowed");
        }
        fail(ParseErrorCode::unexpected_token,
             "Unsupported character at byte " + std::to_string(position_));
    }

    MathNodePtr parseIdentifier() {
        const auto start = position_;
        while (!eof()) {
            const auto character = static_cast<unsigned char>(peek());
            if (isAsciiLetter(peek())) {
                ++position_;
            } else if (character >= 0x80) {
                // UTF-8 has already been validated, so consume one complete scalar.
                const std::size_t length = character < 0xe0 ? 2 : (character < 0xf0 ? 3 : 4);
                position_ += length;
            } else {
                break;
            }
        }
        return makeNode(Identifier{std::string(source_.substr(start, position_ - start))});
    }

    MathNodePtr parseNumber() {
        const auto start = position_;
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            ++position_;
        }
        if (!eof() && peek() == '.' && position_ + 1 < source_.size() &&
            std::isdigit(static_cast<unsigned char>(source_[position_ + 1])) != 0) {
            ++position_;
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++position_;
            }
        }
        return makeNode(Number{std::string(source_.substr(start, position_ - start))});
    }

    MathNodePtr parseExplicitGroup() {
        auto body = parseRequiredContent("group");
        return makeNode(Group{std::move(body)});
    }

    MathNodePtr parseRequiredContent(std::string_view owner) {
        skipWhitespace();
        if (eof() || peek() != '{') {
            fail(ParseErrorCode::missing_argument,
                 std::string("\\") + std::string(owner) + " requires a braced argument");
        }
        ++position_;
        DepthScope depth(*this);
        auto body = parseSequence(Stop::closing_brace);
        if (eof()) {
            fail(ParseErrorCode::unexpected_end,
                 "Unterminated braced argument for " + std::string(owner));
        }
        ++position_;  // closing brace
        return body;
    }

    static bool isForbidden(std::string_view command) {
        static const std::unordered_set<std::string> commands{
            "def",       "gdef",       "edef",      "xdef",       "let",
            "futurelet", "newcommand", "renewcommand", "providecommand", "input",
            "include",   "includegraphics", "usepackage", "documentclass", "openin",
            "openout",   "read",       "write",     "immediate",  "special",
            "csname",    "catcode",    "loop",      "repeat",     "shellescape",
        };
        return commands.contains(std::string(command));
    }

    MathNodePtr parseCommand() {
        const auto command_offset = position_;
        const auto command = readCommand();
        if (isForbidden(command)) {
            fail(ParseErrorCode::forbidden_command,
                 "Forbidden LaTeX command: \\" + command, command_offset);
        }
        if (command == "frac") {
            auto numerator = parseRequiredContent("frac");
            auto denominator = parseRequiredContent("frac");
            if (isEmptyRow(numerator) || isEmptyRow(denominator)) {
                fail(ParseErrorCode::empty_argument, "Fraction arguments cannot be empty",
                     command_offset);
            }
            return makeNode(Fraction{std::move(numerator), std::move(denominator)});
        }
        if (command == "sqrt") {
            return parseRadical(command_offset);
        }
        if (command == "sum" || command == "int") {
            return makeNode(LargeOperator{command == "sum" ? LargeOperatorKind::sum
                                                           : LargeOperatorKind::integral});
        }
        if (command == "left") {
            return parseDelimited(command_offset);
        }
        if (command == "begin") {
            return parseMatrix(command_offset);
        }
        if (command == "right") {
            fail(ParseErrorCode::mismatched_delimiter, "\\right has no matching \\left",
                 command_offset);
        }
        if (command == "end") {
            fail(ParseErrorCode::invalid_environment, "\\end has no matching \\begin",
                 command_offset);
        }
        if (command == "\\") {
            fail(ParseErrorCode::unexpected_token,
                 "Matrix row separator is only valid inside a matrix", command_offset);
        }

        static const std::unordered_set<std::string> identifiers{
            "alpha", "beta",  "gamma", "delta", "epsilon", "varepsilon", "zeta",
            "eta",   "theta", "vartheta", "iota", "kappa", "lambda", "mu", "nu",
            "xi",    "pi",    "varpi", "rho", "sigma", "tau", "upsilon", "phi",
            "varphi", "chi",  "psi", "omega", "Gamma", "Delta", "Theta", "Lambda",
            "Xi", "Pi", "Sigma", "Upsilon", "Phi", "Psi", "Omega", "infty",
            "sin", "cos", "tan", "log", "ln", "exp", "lim",
        };
        if (identifiers.contains(command)) {
            return makeNode(Identifier{"\\" + command});
        }

        static const std::unordered_set<std::string> operators{
            "times", "cdot", "pm", "mp", "div", "le", "leq", "ge", "geq",
            "neq", "approx", "sim", "to", "in", "notin", "subset", "subseteq",
            "cup", "cap", "land", "lor", "partial", "nabla", "{", "}", "|",
        };
        if (operators.contains(command)) {
            return makeNode(Operator{"\\" + command});
        }

        fail(ParseErrorCode::unknown_command, "Unknown LaTeX command: \\" + command,
             command_offset);
    }

    MathNodePtr parseRadical(std::size_t command_offset) {
        skipWhitespace();
        std::optional<MathNodePtr> index;
        if (!eof() && peek() == '[') {
            ++position_;
            DepthScope depth(*this);
            auto value = parseSequence(Stop::closing_bracket);
            if (eof()) {
                fail(ParseErrorCode::unexpected_end, "Unterminated optional root index",
                     command_offset);
            }
            ++position_;  // closing bracket
            if (isEmptyRow(value)) {
                fail(ParseErrorCode::empty_argument, "Root index cannot be empty", command_offset);
            }
            index = std::move(value);
        }
        auto radicand = parseRequiredContent("sqrt");
        if (isEmptyRow(radicand)) {
            fail(ParseErrorCode::empty_argument, "Radicand cannot be empty", command_offset);
        }
        return makeNode(Radical{std::move(index), std::move(radicand)});
    }

    std::string parseDelimiter() {
        skipWhitespace();
        if (eof()) {
            fail(ParseErrorCode::mismatched_delimiter, "Missing scalable delimiter");
        }
        constexpr std::string_view characters = ".()[]|<>";
        if (characters.find(peek()) != std::string_view::npos) {
            std::string delimiter(1, peek());
            ++position_;
            return delimiter;
        }
        if (peek() != '\\') {
            fail(ParseErrorCode::mismatched_delimiter, "Unsupported scalable delimiter");
        }
        const auto offset = position_;
        const auto command = readCommand();
        static const std::unordered_set<std::string> delimiters{
            "langle", "rangle", "lbrace", "rbrace", "vert", "Vert", "{", "}", "|",
        };
        if (!delimiters.contains(command)) {
            fail(ParseErrorCode::mismatched_delimiter,
                 "Unsupported scalable delimiter: \\" + command, offset);
        }
        return "\\" + command;
    }

    MathNodePtr parseDelimited(std::size_t command_offset) {
        auto left = parseDelimiter();
        DepthScope depth(*this);
        auto body = parseSequence(Stop::right_command);
        if (eof() || !atCommand("right")) {
            fail(ParseErrorCode::mismatched_delimiter, "\\left has no matching \\right",
                 command_offset);
        }
        const auto right_command = readCommand();
        (void)right_command;
        auto right = parseDelimiter();
        if (isEmptyRow(body)) {
            fail(ParseErrorCode::empty_argument, "Scalable delimiters cannot enclose an empty body",
                 command_offset);
        }
        return makeNode(Delimited{std::move(left), std::move(right), std::move(body)});
    }

    std::string parseEnvironmentName() {
        skipWhitespace();
        if (eof() || peek() != '{') {
            fail(ParseErrorCode::invalid_environment, "Environment name must be braced");
        }
        ++position_;
        const auto start = position_;
        while (!eof() && (isAsciiLetter(peek()) || peek() == '*')) {
            ++position_;
        }
        if (position_ == start || eof() || peek() != '}') {
            fail(ParseErrorCode::invalid_environment, "Malformed environment name", start);
        }
        std::string name(source_.substr(start, position_ - start));
        ++position_;
        return name;
    }

    static std::optional<MatrixEnvironment> matrixEnvironment(std::string_view name) {
        if (name == "matrix") return MatrixEnvironment::matrix;
        if (name == "pmatrix") return MatrixEnvironment::pmatrix;
        if (name == "bmatrix") return MatrixEnvironment::bmatrix;
        if (name == "Bmatrix") return MatrixEnvironment::Bmatrix;
        if (name == "vmatrix") return MatrixEnvironment::vmatrix;
        if (name == "Vmatrix") return MatrixEnvironment::Vmatrix;
        return std::nullopt;
    }

    void consumeEndEnvironment(std::string_view expected, std::size_t begin_offset) {
        const auto end_offset = position_;
        const auto command = readCommand();
        if (command != "end") {
            fail(ParseErrorCode::invalid_environment, "Expected \\end", end_offset);
        }
        const auto actual = parseEnvironmentName();
        if (actual != expected) {
            fail(ParseErrorCode::invalid_environment,
                 "Environment mismatch: expected \\end{" + std::string(expected) +
                     "}, found \\end{" + actual + "}",
                 begin_offset);
        }
    }

    MathNodePtr parseMatrix(std::size_t command_offset) {
        const auto name = parseEnvironmentName();
        const auto environment = matrixEnvironment(name);
        if (!environment) {
            fail(ParseErrorCode::invalid_environment,
                 "Unsupported environment: " + name, command_offset);
        }

        DepthScope depth(*this);
        std::vector<std::vector<MathNodePtr>> rows;
        while (true) {
            skipWhitespace();
            if (eof()) {
                fail(ParseErrorCode::unexpected_end,
                     "Unterminated matrix environment: " + name, command_offset);
            }
            if (atCommand("end")) {
                consumeEndEnvironment(name, command_offset);
                break;
            }

            std::vector<MathNodePtr> cells;
            while (true) {
                cells.push_back(parseSequence(Stop::matrix_cell));
                if (cells.size() > limits_.max_matrix_columns) {
                    fail(ParseErrorCode::matrix_too_large,
                         "Maximum matrix column count exceeded", command_offset);
                }
                skipWhitespace();
                if (eof()) {
                    fail(ParseErrorCode::unexpected_end,
                         "Unterminated matrix environment: " + name, command_offset);
                }
                if (peek() == '&') {
                    ++position_;
                    continue;
                }
                if (atRowBreak()) {
                    position_ += 2;
                    break;
                }
                if (atCommand("end")) {
                    consumeEndEnvironment(name, command_offset);
                    rows.push_back(std::move(cells));
                    if (rows.size() > limits_.max_matrix_rows) {
                        fail(ParseErrorCode::matrix_too_large,
                             "Maximum matrix row count exceeded", command_offset);
                    }
                    return makeNode(Matrix{*environment, std::move(rows)});
                }
                fail(ParseErrorCode::unexpected_token,
                     "Expected &, \\\\, or \\end inside matrix");
            }

            rows.push_back(std::move(cells));
            if (rows.size() > limits_.max_matrix_rows) {
                fail(ParseErrorCode::matrix_too_large,
                     "Maximum matrix row count exceeded", command_offset);
            }
        }
        if (rows.empty()) {
            fail(ParseErrorCode::empty_argument, "Matrix cannot be empty", command_offset);
        }
        return makeNode(Matrix{*environment, std::move(rows)});
    }

    std::string_view source_;
    ParseLimits limits_;
    std::size_t position_{0};
    std::size_t depth_{0};
    std::size_t node_count_{0};
};

}  // namespace

ParseResult parseLatex(std::string_view source, const ParseLimits& limits) {
    const auto effective = effectiveLimits(limits);
    if (source.size() > effective.max_input_bytes) {
        return ParseError{ParseErrorCode::input_too_large, 0,
                          "LaTeX input exceeds the configured byte limit"};
    }
    if (const auto invalid = invalidUtf8Offset(source)) {
        return ParseError{ParseErrorCode::invalid_utf8, *invalid,
                          "LaTeX input is not valid UTF-8"};
    }
    try {
        return Parser(source, effective).parse();
    } catch (const ParseFailure& failure) {
        return failure.error;
    }
}

}  // namespace docxstudio::math
