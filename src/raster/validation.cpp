#include "docxstudio/raster/validation.h"

#include <algorithm>
#include <array>
#include <limits>

namespace docxstudio::raster {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
constexpr std::array<std::uint8_t, 4> kIhdr{'I', 'H', 'D', 'R'};
constexpr std::array<std::uint8_t, 4> kPlte{'P', 'L', 'T', 'E'};
constexpr std::array<std::uint8_t, 4> kIdat{'I', 'D', 'A', 'T'};
constexpr std::array<std::uint8_t, 4> kIend{'I', 'E', 'N', 'D'};

std::uint16_t bigEndian16(std::span<const std::uint8_t> bytes,
                          std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U]));
}

std::uint32_t bigEndian32(std::span<const std::uint8_t> bytes,
                          std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool chunkTypeEquals(std::span<const std::uint8_t> bytes,
                     std::size_t type_offset,
                     const std::array<std::uint8_t, 4>& expected) {
    return type_offset <= bytes.size() &&
           bytes.size() - type_offset >= expected.size() &&
           std::equal(expected.begin(), expected.end(),
                      bytes.begin() +
                          static_cast<std::ptrdiff_t>(type_offset));
}

bool validChunkType(std::span<const std::uint8_t> type) {
    if (type.size() != 4U) return false;
    for (const std::uint8_t byte : type) {
        const bool alphabetic = (byte >= 'A' && byte <= 'Z') ||
                                (byte >= 'a' && byte <= 'z');
        if (!alphabetic) return false;
    }
    // PNG reserves the third chunk-name bit; it must be uppercase.
    return type[2] >= 'A' && type[2] <= 'Z';
}

bool validPngColorModel(std::uint8_t bit_depth, std::uint8_t color_type) {
    switch (color_type) {
        case 0U:
            return bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
                   bit_depth == 8U || bit_depth == 16U;
        case 2U: return bit_depth == 8U || bit_depth == 16U;
        case 3U:
            return bit_depth == 1U || bit_depth == 2U || bit_depth == 4U ||
                   bit_depth == 8U;
        case 4U:
        case 6U: return bit_depth == 8U || bit_depth == 16U;
        default: return false;
    }
}

Inspection withStatus(ValidationStatus status, Format format,
                      std::uint64_t width = 0,
                      std::uint64_t height = 0) {
    return Inspection{status, format, width, height};
}

ValidationStatus dimensionsStatus(
    Format format, std::uint64_t width, std::uint64_t height,
    const ValidationLimits& limits) {
    if (width == 0U || height == 0U) {
        return ValidationStatus::invalid_dimensions;
    }
    if (width > static_cast<std::uint64_t>(limits.maximum_dimension) ||
        height > static_cast<std::uint64_t>(limits.maximum_dimension)) {
        return ValidationStatus::dimension_limit;
    }
    const std::uint64_t pixels = width * height;
    if (pixels > static_cast<std::uint64_t>(limits.maximum_pixels)) {
        return ValidationStatus::pixel_limit;
    }
    const std::int64_t bytes_per_pixel = worstCaseBytesPerPixel(format);
    if (pixels > static_cast<std::uint64_t>(
                     limits.maximum_decoded_bytes / bytes_per_pixel)) {
        return ValidationStatus::decoded_byte_limit;
    }
    return ValidationStatus::valid;
}

Inspection inspectPng(std::span<const std::uint8_t> bytes,
                      const ValidationLimits& limits) {
    if (bytes.size() < 33U || bigEndian32(bytes, 8U) != 13U ||
        !chunkTypeEquals(bytes, 12U, kIhdr)) {
        return withStatus(ValidationStatus::malformed_structure, Format::png);
    }
    const std::uint32_t ihdr_crc = bigEndian32(bytes, 29U);
    if (crc32(bytes.subspan(12U, 17U)) != ihdr_crc) {
        return withStatus(ValidationStatus::malformed_structure, Format::png);
    }

    const std::uint64_t width = bigEndian32(bytes, 16U);
    const std::uint64_t height = bigEndian32(bytes, 20U);
    const std::uint8_t bit_depth = bytes[24U];
    const std::uint8_t color_type = bytes[25U];
    if (!validPngColorModel(bit_depth, color_type) || bytes[26U] != 0U ||
        bytes[27U] != 0U || bytes[28U] > 1U) {
        return withStatus(
            ValidationStatus::malformed_structure, Format::png, width, height);
    }
    const ValidationStatus dimension_status =
        dimensionsStatus(Format::png, width, height, limits);
    if (dimension_status != ValidationStatus::valid) {
        return withStatus(dimension_status, Format::png, width, height);
    }

    bool saw_ihdr = false;
    bool saw_plte = false;
    bool saw_idat = false;
    bool idat_ended = false;
    std::uint64_t idat_bytes = 0U;
    std::size_t segment_count = 0U;
    std::size_t offset = kPngSignature.size();
    while (offset <= bytes.size() && bytes.size() - offset >= 12U) {
        if (++segment_count > limits.maximum_container_segments) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::png,
                width, height);
        }
        const std::uint32_t payload_size = bigEndian32(bytes, offset);
        const std::uint64_t complete_chunk_size =
            12ULL + static_cast<std::uint64_t>(payload_size);
        if (complete_chunk_size >
            static_cast<std::uint64_t>(bytes.size() - offset)) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::png,
                width, height);
        }
        const std::size_t type_offset = offset + 4U;
        const std::size_t payload_offset = offset + 8U;
        const std::size_t crc_offset =
            payload_offset + static_cast<std::size_t>(payload_size);
        const auto type = bytes.subspan(type_offset, 4U);
        if (!validChunkType(type) ||
            crc32(bytes.subspan(
                type_offset, 4U + static_cast<std::size_t>(payload_size))) !=
                bigEndian32(bytes, crc_offset)) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::png,
                width, height);
        }

        const bool ihdr = chunkTypeEquals(bytes, type_offset, kIhdr);
        const bool plte = chunkTypeEquals(bytes, type_offset, kPlte);
        const bool idat = chunkTypeEquals(bytes, type_offset, kIdat);
        const bool iend = chunkTypeEquals(bytes, type_offset, kIend);
        if (!saw_ihdr) {
            if (!ihdr || payload_size != 13U) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::png,
                    width, height);
            }
            saw_ihdr = true;
        } else if (ihdr) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::png,
                width, height);
        }

        if (plte) {
            if (saw_plte || saw_idat || payload_size == 0U ||
                payload_size > 768U || payload_size % 3U != 0U ||
                color_type == 0U || color_type == 4U) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::png,
                    width, height);
            }
            saw_plte = true;
        } else if (idat) {
            if (idat_ended || (color_type == 3U && !saw_plte)) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::png,
                    width, height);
            }
            saw_idat = true;
            idat_bytes += payload_size;
        } else if (saw_idat && !iend) {
            idat_ended = true;
        }

        const std::size_t next =
            offset + static_cast<std::size_t>(complete_chunk_size);
        if (iend) {
            if (payload_size != 0U || !saw_idat || idat_bytes == 0U ||
                next != bytes.size()) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::png,
                    width, height);
            }
            return withStatus(
                ValidationStatus::valid, Format::png, width, height);
        }
        // Unknown critical chunks cannot be interpreted safely. The first
        // chunk-name letter is uppercase for critical chunks.
        if (type[0] >= 'A' && type[0] <= 'Z' && !ihdr && !plte && !idat) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::png,
                width, height);
        }
        offset = next;
    }
    return withStatus(
        ValidationStatus::malformed_structure, Format::png, width, height);
}

bool isStartOfFrame(std::uint8_t marker) {
    switch (marker) {
        case 0xc0U:
        case 0xc1U:
        case 0xc2U:
        case 0xc3U:
        case 0xc5U:
        case 0xc6U:
        case 0xc7U:
        case 0xc9U:
        case 0xcaU:
        case 0xcbU:
        case 0xcdU:
        case 0xceU:
        case 0xcfU: return true;
        default: return false;
    }
}

Inspection inspectJpeg(std::span<const std::uint8_t> bytes,
                       const ValidationLimits& limits) {
    if (bytes.size() < 4U || bytes[0] != 0xffU || bytes[1] != 0xd8U) {
        return withStatus(
            ValidationStatus::malformed_structure, Format::jpeg);
    }
    bool saw_frame = false;
    bool saw_scan = false;
    bool in_entropy_data = false;
    std::uint64_t width = 0U;
    std::uint64_t height = 0U;
    std::size_t segment_count = 0U;
    std::size_t offset = 2U;

    while (offset < bytes.size()) {
        if (bytes[offset] != 0xffU) {
            if (in_entropy_data) {
                ++offset;
                continue;
            }
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        while (offset < bytes.size() && bytes[offset] == 0xffU) ++offset;
        if (offset >= bytes.size()) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        const std::uint8_t marker = bytes[offset++];
        if (in_entropy_data && marker == 0x00U) {
            continue;
        }
        if (in_entropy_data && marker >= 0xd0U && marker <= 0xd7U) {
            continue;
        }
        in_entropy_data = false;
        if (++segment_count > limits.maximum_container_segments ||
            marker == 0x00U || marker == 0xd8U ||
            (marker >= 0xd0U && marker <= 0xd7U)) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        if (marker == 0xd9U) {
            if (!saw_frame || !saw_scan || offset != bytes.size()) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::jpeg,
                    width, height);
            }
            return withStatus(
                ValidationStatus::valid, Format::jpeg, width, height);
        }
        if (marker == 0x01U) continue;
        if (bytes.size() - offset < 2U) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        const std::uint16_t segment_length = bigEndian16(bytes, offset);
        if (segment_length < 2U ||
            static_cast<std::size_t>(segment_length) > bytes.size() - offset) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        const std::size_t segment_end =
            offset + static_cast<std::size_t>(segment_length);
        if (isStartOfFrame(marker)) {
            if (saw_frame || segment_length < 11U) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::jpeg,
                    width, height);
            }
            const std::uint8_t component_count = bytes[offset + 7U];
            if (component_count == 0U || component_count > 4U ||
                segment_length !=
                    static_cast<std::uint16_t>(8U + 3U * component_count)) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::jpeg,
                    width, height);
            }
            height = bigEndian16(bytes, offset + 3U);
            width = bigEndian16(bytes, offset + 5U);
            const ValidationStatus dimension_status =
                dimensionsStatus(Format::jpeg, width, height, limits);
            if (dimension_status != ValidationStatus::valid) {
                return withStatus(
                    dimension_status, Format::jpeg, width, height);
            }
            saw_frame = true;
        } else if (marker == 0xdaU) {
            if (!saw_frame || segment_length < 8U) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::jpeg,
                    width, height);
            }
            const std::uint8_t component_count = bytes[offset + 2U];
            if (component_count == 0U || component_count > 4U ||
                segment_length !=
                    static_cast<std::uint16_t>(6U + 2U * component_count)) {
                return withStatus(
                    ValidationStatus::malformed_structure, Format::jpeg,
                    width, height);
            }
            saw_scan = true;
            in_entropy_data = true;
        } else if (marker == 0xddU && segment_length != 4U) {
            return withStatus(
                ValidationStatus::malformed_structure, Format::jpeg,
                width, height);
        }
        offset = segment_end;
    }
    return withStatus(
        ValidationStatus::malformed_structure, Format::jpeg, width, height);
}

}  // namespace

Inspection inspect(std::span<const std::uint8_t> encoded,
                   Format expected_format,
                   const ValidationLimits& limits) {
    if (limits.maximum_encoded_bytes == 0U ||
        limits.maximum_dimension <= 0 || limits.maximum_pixels <= 0 ||
        limits.maximum_decoded_bytes <= 0 ||
        limits.maximum_container_segments == 0U) {
        return withStatus(ValidationStatus::invalid_limits, Format::unknown);
    }
    if (encoded.empty()) {
        return withStatus(ValidationStatus::empty_input, Format::unknown);
    }
    if (encoded.size() > limits.maximum_encoded_bytes) {
        return withStatus(
            ValidationStatus::encoded_size_limit, Format::unknown);
    }
    if (expected_format != Format::unknown && expected_format != Format::png &&
        expected_format != Format::jpeg) {
        return withStatus(
            ValidationStatus::unsupported_format, Format::unknown);
    }

    Format detected = Format::unknown;
    if (encoded.size() >= kPngSignature.size() &&
        std::equal(kPngSignature.begin(), kPngSignature.end(),
                   encoded.begin())) {
        detected = Format::png;
    } else if (encoded.size() >= 2U && encoded[0] == 0xffU &&
               encoded[1] == 0xd8U) {
        detected = Format::jpeg;
    }
    if (detected == Format::unknown) {
        return withStatus(
            ValidationStatus::unsupported_format, Format::unknown);
    }
    if (expected_format != Format::unknown && expected_format != detected) {
        return withStatus(ValidationStatus::format_mismatch, detected);
    }
    return detected == Format::png ? inspectPng(encoded, limits)
                                   : inspectJpeg(encoded, limits);
}

}  // namespace docxstudio::raster
