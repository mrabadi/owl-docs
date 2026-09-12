#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace docxstudio::raster {

enum class Format { unknown, png, jpeg };

struct ValidationLimits {
    std::size_t maximum_encoded_bytes{64U * 1024U * 1024U};
    int maximum_dimension{16'384};
    std::int64_t maximum_pixels{64LL * 1024LL * 1024LL};
    std::int64_t maximum_decoded_bytes{256LL * 1024LL * 1024LL};
    std::size_t maximum_container_segments{65'536U};
};

enum class ValidationStatus {
    valid,
    invalid_limits,
    empty_input,
    encoded_size_limit,
    unsupported_format,
    format_mismatch,
    malformed_structure,
    invalid_dimensions,
    dimension_limit,
    pixel_limit,
    decoded_byte_limit,
};

struct Inspection {
    ValidationStatus status{ValidationStatus::empty_input};
    Format format{Format::unknown};
    std::uint64_t width{0};
    std::uint64_t height{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == ValidationStatus::valid;
    }
};

// Validates the complete PNG/JPEG container without allocating decoded pixel
// storage. PNG chunk CRCs and ordering are checked; JPEG marker, segment,
// scan, and terminal-marker structure are checked. Entropy decoding remains
// the responsibility of the bounded platform decoder.
[[nodiscard]] Inspection inspect(
    std::span<const std::uint8_t> encoded,
    Format expected_format = Format::unknown,
    const ValidationLimits& limits = {});

[[nodiscard]] constexpr std::int64_t worstCaseBytesPerPixel(
    Format format) noexcept {
    return format == Format::png ? 8LL : 4LL;
}

}  // namespace docxstudio::raster
