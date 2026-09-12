#include "docxstudio/app/RasterDecoder.h"

#include <QBuffer>
#include <QByteArray>
#include <QImageReader>

#include <algorithm>
#include <limits>
#include <utility>

namespace docxstudio::app {
namespace {

RasterDecodeStatus decodeStatus(raster::ValidationStatus status) {
    switch (status) {
        case raster::ValidationStatus::valid:
            return RasterDecodeStatus::decoded;
        case raster::ValidationStatus::invalid_limits:
            return RasterDecodeStatus::invalid_limits;
        case raster::ValidationStatus::empty_input:
            return RasterDecodeStatus::empty_input;
        case raster::ValidationStatus::encoded_size_limit:
            return RasterDecodeStatus::encoded_size_limit;
        case raster::ValidationStatus::unsupported_format:
            return RasterDecodeStatus::unsupported_format;
        case raster::ValidationStatus::format_mismatch:
            return RasterDecodeStatus::format_mismatch;
        case raster::ValidationStatus::malformed_structure:
            return RasterDecodeStatus::malformed_structure;
        case raster::ValidationStatus::invalid_dimensions:
            return RasterDecodeStatus::invalid_dimensions;
        case raster::ValidationStatus::dimension_limit:
            return RasterDecodeStatus::dimension_limit;
        case raster::ValidationStatus::pixel_limit:
            return RasterDecodeStatus::pixel_limit;
        case raster::ValidationStatus::decoded_byte_limit:
            return RasterDecodeStatus::decoded_byte_limit;
    }
    return RasterDecodeStatus::decoder_rejected;
}

RasterDecodeResult failedResult(
    RasterDecodeStatus status,
    const raster::Inspection& inspection = {}) {
    RasterDecodeResult result;
    result.status = status;
    result.format = inspection.format;
    if (inspection.width <=
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()) &&
        inspection.height <=
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        result.source_pixel_size =
            QSize(static_cast<int>(inspection.width),
                  static_cast<int>(inspection.height));
    }
    return result;
}

QByteArray qtFormat(RasterImageFormat format) {
    if (format == RasterImageFormat::png) return QByteArrayLiteral("png");
    if (format == RasterImageFormat::jpeg) return QByteArrayLiteral("jpeg");
    return {};
}

RasterCacheResult cacheFailure(RasterCacheStatus status,
                               RasterDecodeStatus decode_status =
                                   RasterDecodeStatus::empty_input) {
    return RasterCacheResult{status, decode_status, {}};
}

}  // namespace

RasterDecodeResult decodeRasterImage(
    std::span<const std::uint8_t> encoded,
    const RasterDecodeLimits& limits) {
    if (encoded.size() >
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        return failedResult(RasterDecodeStatus::encoded_size_limit);
    }
    const raster::Inspection inspection =
        raster::inspect(encoded, RasterImageFormat::unknown, limits);
    if (!inspection.ok()) {
        return failedResult(decodeStatus(inspection.status), inspection);
    }

    const QByteArray data(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<qsizetype>(encoded.size()));
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return failedResult(RasterDecodeStatus::decoder_rejected, inspection);
    }
    QImageReader reader(&buffer, qtFormat(inspection.format));
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(false);
    if (!reader.canRead()) {
        return failedResult(RasterDecodeStatus::decoder_rejected, inspection);
    }
    const QSize reported_size = reader.size();
    const QSize expected_size(static_cast<int>(inspection.width),
                              static_cast<int>(inspection.height));
    if (!reported_size.isValid() || reported_size != expected_size) {
        return failedResult(RasterDecodeStatus::metadata_mismatch, inspection);
    }

    QImage image = reader.read();
    if (image.isNull()) {
        return failedResult(RasterDecodeStatus::decoder_rejected, inspection);
    }
    if (image.size() != expected_size || image.sizeInBytes() < 0 ||
        static_cast<std::int64_t>(image.sizeInBytes()) >
            limits.maximum_decoded_bytes) {
        return failedResult(RasterDecodeStatus::metadata_mismatch, inspection);
    }

    RasterDecodeResult result;
    result.status = RasterDecodeStatus::decoded;
    result.format = inspection.format;
    result.source_pixel_size = expected_size;
    result.image = std::move(image);
    return result;
}

BoundedRasterCache::BoundedRasterCache(RasterCacheLimits limits)
    : limits_(std::move(limits)) {}

RasterCacheResult BoundedRasterCache::decode(
    std::string_view stable_identity,
    std::span<const std::uint8_t> encoded) {
    if (stable_identity.empty()) {
        return cacheFailure(RasterCacheStatus::invalid_identity);
    }
    if (reference_count_ >= limits_.maximum_references) {
        return cacheFailure(RasterCacheStatus::reference_limit);
    }
    ++reference_count_;

    const auto cached = entries_.find(stable_identity);
    if (cached != entries_.end()) {
        if (cached->second.image.isNull()) {
            return cacheFailure(
                RasterCacheStatus::decode_rejected,
                cached->second.decode_status);
        }
        return RasterCacheResult{
            RasterCacheStatus::cache_hit,
            cached->second.decode_status,
            cached->second.image};
    }
    if (entries_.size() >= limits_.maximum_unique_images) {
        return cacheFailure(RasterCacheStatus::unique_image_limit);
    }
    if (limits_.maximum_aggregate_decoded_bytes <= 0 ||
        aggregate_decoded_bytes_ >=
            limits_.maximum_aggregate_decoded_bytes) {
        return cacheFailure(
            RasterCacheStatus::aggregate_decoded_byte_limit);
    }

    RasterDecodeLimits effective_limits = limits_.per_image;
    effective_limits.maximum_decoded_bytes = std::min(
        effective_limits.maximum_decoded_bytes,
        limits_.maximum_aggregate_decoded_bytes - aggregate_decoded_bytes_);
    RasterDecodeResult decoded = decodeRasterImage(encoded, effective_limits);
    if (!decoded.ok()) {
        const RasterDecodeStatus rejected_status = decoded.status;
        entries_.emplace(
            std::string(stable_identity), Entry{rejected_status, {}});
        const bool aggregate_limited =
            rejected_status == RasterDecodeStatus::decoded_byte_limit &&
            effective_limits.maximum_decoded_bytes <
                limits_.per_image.maximum_decoded_bytes;
        return cacheFailure(
            aggregate_limited
                ? RasterCacheStatus::aggregate_decoded_byte_limit
                : RasterCacheStatus::decode_rejected,
            rejected_status);
    }

    const std::int64_t decoded_bytes =
        static_cast<std::int64_t>(decoded.image.sizeInBytes());
    if (decoded_bytes < 0 ||
        decoded_bytes > limits_.maximum_aggregate_decoded_bytes -
                            aggregate_decoded_bytes_) {
        entries_.emplace(
            std::string(stable_identity),
            Entry{RasterDecodeStatus::decoded_byte_limit, {}});
        return cacheFailure(
            RasterCacheStatus::aggregate_decoded_byte_limit,
            RasterDecodeStatus::decoded_byte_limit);
    }

    aggregate_decoded_bytes_ += decoded_bytes;
    const RasterDecodeStatus decoded_status = decoded.status;
    auto [entry, inserted] = entries_.emplace(
        std::string(stable_identity),
        Entry{decoded_status, decoded.image});
    if (!inserted) {
        // The cache is single-threaded today. This defensive path preserves
        // accounting if that invariant changes without silently duplicating
        // decoded storage.
        aggregate_decoded_bytes_ -= decoded_bytes;
        return entry->second.image.isNull()
            ? cacheFailure(
                  RasterCacheStatus::decode_rejected,
                  entry->second.decode_status)
            : RasterCacheResult{
                  RasterCacheStatus::cache_hit,
                  entry->second.decode_status,
                  entry->second.image};
    }
    return RasterCacheResult{
        RasterCacheStatus::decoded, decoded_status, std::move(decoded.image)};
}

}  // namespace docxstudio::app
