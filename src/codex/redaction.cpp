#include "docxstudio/codex/redaction.hpp"

#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>

namespace docxstudio::codex {
namespace {

std::string normalizedKey(const std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size());
    for (const unsigned char character : key) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(
                static_cast<char>(std::tolower(character)));
        }
    }
    return normalized;
}

bool endsWith(const std::string& value, const std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

void redactJsonInPlace(Json& value) {
    if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            if (isSensitiveKey(key)) {
                child = kRedactedValue;
            } else {
                redactJsonInPlace(child);
            }
        }
        return;
    }

    if (value.is_array()) {
        for (Json& child : value) {
            redactJsonInPlace(child);
        }
    }
}

std::string regexReplace(std::string input,
                         const std::regex& pattern,
                         const std::string& replacement) {
    return std::regex_replace(input, pattern, replacement);
}

}  // namespace

bool isSensitiveKey(const std::string_view key) {
    static const std::unordered_set<std::string> exact = {
        "authorization",       "proxyauthorization", "apikey",
        "openaiapikey",        "accesskey",          "accesstoken",
        "refreshtoken",        "idtoken",            "bearertoken",
        "token",               "authtoken",          "sessiontoken",
        "devicetoken",         "wstoken",             "password",
        "passwd",
        "secret",              "clientsecret",       "cookie",
        "setcookie",           "credential",         "credentials",
        "chatgptauthtokens",   "authenticationtoken",
    };

    const std::string normalized = normalizedKey(key);
    if (exact.count(normalized) != 0) {
        return true;
    }

    // Deliberately do not redact every key ending in "token": tokenUsage,
    // inputTokens, and similar telemetry are useful and are not credentials.
    return endsWith(normalized, "apikey") ||
           endsWith(normalized, "accesstoken") ||
           endsWith(normalized, "refreshtoken") ||
           endsWith(normalized, "idtoken") ||
           endsWith(normalized, "clientsecret") ||
           endsWith(normalized, "password");
}

Json redactJson(const Json& value) {
    Json redacted = value;
    redactJsonInPlace(redacted);
    return redacted;
}

std::string redactForLog(const std::string_view text) {
    std::string output(text);

    const Json parsed = Json::parse(output, nullptr, false);
    if (!parsed.is_discarded()) {
        output = redactJson(parsed).dump();
    }

    static const std::regex bearer(
        R"((\bBearer\s+)[A-Za-z0-9._~+/=-]+)",
        std::regex_constants::icase);
    static const std::regex apiKey(
        R"(\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{8,})",
        std::regex_constants::icase);
    static const std::regex jwt(
        R"(\beyJ[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{5,}\.[A-Za-z0-9_-]{5,}\b)");
    static const std::regex environmentKey(
        R"((\bOPENAI_API_KEY\s*=\s*)[^\s;]+)",
        std::regex_constants::icase);
    static const std::regex oauthQuery(
        R"(((?:access_token|refresh_token|id_token|api_key|code)=)[^&\s]+)",
        std::regex_constants::icase);

    output = regexReplace(std::move(output), bearer,
                          "$1" + std::string(kRedactedValue));
    output = regexReplace(std::move(output), apiKey,
                          std::string(kRedactedValue));
    output = regexReplace(std::move(output), jwt,
                          std::string(kRedactedValue));
    output = regexReplace(std::move(output), environmentKey,
                          "$1" + std::string(kRedactedValue));
    output = regexReplace(std::move(output), oauthQuery,
                          "$1" + std::string(kRedactedValue));
    return output;
}

}  // namespace docxstudio::codex
