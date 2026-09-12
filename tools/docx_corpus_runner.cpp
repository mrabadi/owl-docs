#include "docx_corpus.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " --manifest MANIFEST"
                 " (--output-dir DIRECTORY | --ephemeral-output)"
                 " [--minimum-fixtures COUNT]\n";
}

class EphemeralOutputDirectory {
public:
    EphemeralOutputDirectory() {
        const auto base = std::filesystem::temp_directory_path() /
                          "owl-docs-corpus-run-XXXXXX";
        std::string pattern = base.string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error(
                "could not create an ephemeral corpus output directory");
        }
        path_ = created;
    }

    ~EphemeralOutputDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    EphemeralOutputDirectory(const EphemeralOutputDirectory&) = delete;
    EphemeralOutputDirectory& operator=(const EphemeralOutputDirectory&) =
        delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::size_t parseCount(const std::string& value) {
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("invalid minimum fixture count");
    }
    return static_cast<std::size_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path manifest;
    std::filesystem::path output_directory;
    std::optional<std::size_t> minimum;
    bool ephemeral_output = false;
    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--help" || argument == "-h") {
                usage(argv[0]);
                return EXIT_SUCCESS;
            }
            if (argument == "--ephemeral-output") {
                ephemeral_output = true;
                continue;
            }
            if (index + 1 >= argc) {
                throw std::runtime_error("missing option value");
            }
            const std::string value(argv[++index]);
            if (argument == "--manifest") {
                manifest = value;
            } else if (argument == "--output-dir") {
                output_directory = value;
            } else if (argument == "--minimum-fixtures") {
                minimum = parseCount(value);
            } else {
                throw std::runtime_error("unknown option '" +
                                         std::string(argument) + "'");
            }
        }
        if (manifest.empty()) {
            throw std::runtime_error("manifest is required");
        }
        if (ephemeral_output == !output_directory.empty()) {
            throw std::runtime_error(
                "choose exactly one of --output-dir and --ephemeral-output");
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        usage(argv[0]);
        return 64;
    }

    try {
        std::optional<EphemeralOutputDirectory> ephemeral;
        if (ephemeral_output) {
            ephemeral.emplace();
            output_directory = ephemeral->path();
        }
        const auto report = docxstudio::corpus::runManifest(
            manifest, output_directory, minimum);
        std::cout << docxstudio::corpus::reportJson(report);
        return report.passed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
