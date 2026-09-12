#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace docxstudio::corpus {

struct FixtureResult {
    std::string id;
    std::filesystem::path path;
    bool opened{false};
    bool exact_copy_verified{false};
    bool localized_edit_verified{false};
    std::string error;
};

struct RunReport {
    bool passed{false};
    std::size_t fixture_count{0};
    std::size_t opened_count{0};
    std::size_t exact_copy_count{0};
    std::size_t localized_edit_count{0};
    std::size_t required_fixture_count{0};
    std::vector<FixtureResult> fixtures;
    std::string error;
};

// Runs a schema-versioned JSON manifest. Relative fixture paths are resolved
// against the manifest directory. Every fixture must declare a license,
// immutable source revision, provenance URL, and SHA-256 digest.
[[nodiscard]] RunReport runManifest(
    const std::filesystem::path& manifest,
    const std::filesystem::path& output_directory,
    std::optional<std::size_t> minimum_fixture_count = std::nullopt);

[[nodiscard]] std::string sha256File(const std::filesystem::path& path);
[[nodiscard]] std::string reportJson(const RunReport& report);

}  // namespace docxstudio::corpus
