#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace docxstudio::core {

// A value-semantic, process-independent 128-bit identifier. The all-zero value
// is reserved as an invalid/sentinel identifier.
struct NodeId {
    std::uint64_t high{0};
    std::uint64_t low{0};

    [[nodiscard]] static NodeId generate();
    [[nodiscard]] static std::optional<NodeId> parse(std::string_view value);
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] constexpr bool isValid() const noexcept { return high != 0 || low != 0; }

    auto operator<=>(const NodeId&) const = default;
};

struct NodeIdHash {
    [[nodiscard]] std::size_t operator()(const NodeId& id) const noexcept;
};

class Revision {
public:
    constexpr Revision() noexcept = default;
    explicit constexpr Revision(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] std::optional<Revision> next() const noexcept;

    auto operator<=>(const Revision&) const = default;

private:
    std::uint64_t value_{0};
};

enum class ErrorCode {
    revision_conflict,
    paragraph_not_found,
    duplicate_node_id,
    invalid_node_id,
    invalid_position,
    invalid_range,
    invalid_utf16,
    invalid_formatting,
    invalid_operation,
    history_empty,
    preview_not_found,
    preview_conflict,
    revision_overflow,
};

struct Error {
    ErrorCode code{ErrorCode::invalid_operation};
    std::string message;

    auto operator<=>(const Error&) const = default;
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const { return std::get<T>(storage_); }
    [[nodiscard]] Error& error() { return std::get<Error>(storage_); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] const Error& error() const { return error_.value(); }

private:
    std::optional<Error> error_;
};

}  // namespace docxstudio::core
