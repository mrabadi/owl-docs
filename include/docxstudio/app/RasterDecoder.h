#pragma once

#include "docxstudio/raster/validation.h"

#include <QImage>
#include <QSize>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace docxstudio::app {

// Limits are deliberately part of the request contract so the same
// preflight can later be moved behind a restricted image-decoder process.
// The current backend is in-process and only admits PNG and JPEG payloads.
using RasterDecodeLimits = raster::ValidationLimits;
using RasterImageFormat = raster::Format;

enum class RasterDecodeStatus {
    decoded,
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
    decoder_rejected,
    metadata_mismatch,
};

struct RasterDecodeResult {
    RasterDecodeStatus status{RasterDecodeStatus::empty_input};
    RasterImageFormat format{RasterImageFormat::unknown};
    QSize source_pixel_size;
    QImage image;

    [[nodiscard]] bool ok() const noexcept {
        return status == RasterDecodeStatus::decoded && !image.isNull();
    }
};

// Performs container validation and dimension/decoded-size preflight before
// invoking Qt's raster codec. It never uses a filename or resolves external
// resources. A failure always returns a null image.
[[nodiscard]] RasterDecodeResult decodeRasterImage(
    std::span<const std::uint8_t> encoded,
    const RasterDecodeLimits& limits = {});

struct RasterCacheLimits {
    RasterDecodeLimits per_image;
    std::size_t maximum_references{4'096U};
    std::size_t maximum_unique_images{1'024U};
    std::int64_t maximum_aggregate_decoded_bytes{
        256LL * 1024LL * 1024LL};
};

enum class RasterCacheStatus {
    decoded,
    cache_hit,
    decode_rejected,
    invalid_identity,
    reference_limit,
    unique_image_limit,
    aggregate_decoded_byte_limit,
};

struct RasterCacheResult {
    RasterCacheStatus status{RasterCacheStatus::decode_rejected};
    RasterDecodeStatus decode_status{RasterDecodeStatus::empty_input};
    QImage image;

    [[nodiscard]] bool ok() const noexcept {
        return (status == RasterCacheStatus::decoded ||
                status == RasterCacheStatus::cache_hit) &&
               !image.isNull();
    }
};

// Caches both successful and failed decodes by a content-stable package
// identity. QImage copies remain implicitly shared, so repeated DrawingML
// references do not duplicate pixel storage. Reference, unique-image, and
// aggregate decoded-byte limits bound the surrounding presentation vector.
class BoundedRasterCache {
public:
    explicit BoundedRasterCache(RasterCacheLimits limits = {});

    [[nodiscard]] RasterCacheResult decode(
        std::string_view stable_identity,
        std::span<const std::uint8_t> encoded);

    [[nodiscard]] std::size_t referenceCount() const noexcept {
        return reference_count_;
    }
    [[nodiscard]] std::size_t uniqueImageCount() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] std::int64_t aggregateDecodedBytes() const noexcept {
        return aggregate_decoded_bytes_;
    }

private:
    struct Entry {
        RasterDecodeStatus decode_status{RasterDecodeStatus::empty_input};
        QImage image;
    };

    RasterCacheLimits limits_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::size_t reference_count_{0};
    std::int64_t aggregate_decoded_bytes_{0};
};

}  // namespace docxstudio::app
