#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace docxstudio::codex {

using Json = nlohmann::json;

inline constexpr std::string_view kRedactedValue = "[REDACTED]";

[[nodiscard]] bool isSensitiveKey(std::string_view key);

// Returns a deep copy suitable for diagnostics. Authentication material is
// removed while non-secret catalog fields such as tokenUsage remain intact.
[[nodiscard]] Json redactJson(const Json& value);

// Redacts structured JSON when possible, then masks common API-key, bearer,
// JWT, environment-variable, and OAuth-query representations.
[[nodiscard]] std::string redactForLog(std::string_view text);

}  // namespace docxstudio::codex
