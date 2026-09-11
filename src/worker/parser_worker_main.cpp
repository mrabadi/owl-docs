#include "docxstudio/worker/protocol.h"

#include <charconv>
#include <climits>
#include <cstdio>
#include <string_view>

namespace {

bool parseDescriptorArgument(std::string_view argument, std::string_view prefix, int& descriptor) {
    if (!argument.starts_with(prefix)) {
        return false;
    }
    argument.remove_prefix(prefix.size());
    unsigned value = 0;
    const auto result =
        std::from_chars(argument.data(), argument.data() + argument.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() ||
        value > static_cast<unsigned>(INT_MAX)) {
        return false;
    }
    descriptor = static_cast<int>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int input_fd = -1;
    int output_fd = -1;
    if (argc != 3 ||
        !parseDescriptorArgument(argv[1], "--input-fd=", input_fd) ||
        !parseDescriptorArgument(argv[2], "--output-fd=", output_fd)) {
        std::fputs(
            "usage: owl-docs-parser-worker --input-fd=N --output-fd=N\n",
            stderr);
        return 64;
    }
    return docxstudio::worker::runParserWorker(input_fd, output_fd);
}
