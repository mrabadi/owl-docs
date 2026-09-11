#include "docxstudio/core/types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <random>

namespace docxstudio::core {
namespace {

std::uint64_t splitMix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t processSeed() {
    std::random_device random;
    const auto time = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return splitMix64((static_cast<std::uint64_t>(random()) << 32U) ^ random() ^ time);
}

int hexValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

}  // namespace

NodeId NodeId::generate() {
    static const std::uint64_t seed = processSeed();
    static std::atomic<std::uint64_t> sequence{1};

    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    NodeId id{splitMix64(seed ^ serial), splitMix64(seed + serial)};
    if (!id.isValid()) {
        id.low = 1;
    }
    return id;
}

std::optional<NodeId> NodeId::parse(std::string_view value) {
    std::array<char, 32> digits{};
    std::size_t count = 0;
    for (const char character : value) {
        if (character == '-') {
            continue;
        }
        if (count == digits.size() || hexValue(character) < 0) {
            return std::nullopt;
        }
        digits[count++] = character;
    }
    if (count != digits.size()) {
        return std::nullopt;
    }

    NodeId id;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        auto& half = index < 16 ? id.high : id.low;
        half = (half << 4U) | static_cast<std::uint64_t>(hexValue(digits[index]));
    }
    if (!id.isValid()) {
        return std::nullopt;
    }
    return id;
}

std::string NodeId::toString() const {
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result(36, '-');
    const std::array<std::size_t, 32> positions{
        0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17,
        19, 20, 21, 22, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const std::uint64_t half = index < 16 ? high : low;
        const unsigned shift = static_cast<unsigned>((15 - (index % 16)) * 4);
        result[positions[index]] = hexadecimal[(half >> shift) & 0x0fU];
    }
    return result;
}

std::size_t NodeIdHash::operator()(const NodeId& id) const noexcept {
    const auto mixed = splitMix64(id.high) ^ splitMix64(id.low + 0x9e3779b97f4a7c15ULL);
    return static_cast<std::size_t>(mixed);
}

std::optional<Revision> Revision::next() const noexcept {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return Revision(value_ + 1);
}

}  // namespace docxstudio::core
