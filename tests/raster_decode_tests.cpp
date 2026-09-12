#include "docxstudio/app/RasterDecoder.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using docxstudio::app::RasterDecodeLimits;
using docxstudio::app::RasterDecodeResult;
using docxstudio::app::RasterDecodeStatus;
using docxstudio::app::RasterImageFormat;
using docxstudio::app::BoundedRasterCache;
using docxstudio::app::RasterCacheLimits;
using docxstudio::app::RasterCacheStatus;
using docxstudio::app::decodeRasterImage;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::span<const std::uint8_t> byteView(const QByteArray& bytes) {
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())};
}

std::vector<std::uint8_t> byteVector(const QByteArray& bytes) {
    const auto view = byteView(bytes);
    return {view.begin(), view.end()};
}

QByteArray encodedImage(const char* format) {
    QImage image(19, 11, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(
                x, y,
                QColor::fromRgb((x * 31 + y * 7) % 256,
                                (x * 13 + y * 29) % 256,
                                (x * 3 + y * 47) % 256));
        }
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    check(buffer.open(QIODevice::WriteOnly),
          "could not open in-memory image buffer");
    check(image.save(&buffer, format, 90),
          "required in-memory image codec is unavailable");
    return encoded;
}

void checkBoundedSuccess(const RasterDecodeResult& result,
                         const RasterDecodeLimits& limits,
                         std::string_view context) {
    if (!result.ok()) return;
    check(result.image.width() > 0 && result.image.height() > 0, context);
    check(result.image.width() <= limits.maximum_dimension &&
              result.image.height() <= limits.maximum_dimension,
          context);
    const std::int64_t pixels =
        static_cast<std::int64_t>(result.image.width()) *
        static_cast<std::int64_t>(result.image.height());
    check(pixels <= limits.maximum_pixels, context);
    check(static_cast<std::int64_t>(result.image.sizeInBytes()) <=
              limits.maximum_decoded_bytes,
          context);
}

std::uint32_t pngCrc32(std::span<const std::uint8_t> bytes) {
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

void writeBigEndian32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                      std::uint32_t value) {
    check(offset <= bytes.size() && bytes.size() - offset >= 4U,
          "invalid synthetic PNG patch offset");
    bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> pngWithDimensions(const QByteArray& source,
                                            std::uint32_t width,
                                            std::uint32_t height) {
    auto bytes = byteVector(source);
    check(bytes.size() >= 33U, "synthetic PNG is missing IHDR");
    writeBigEndian32(bytes, 16U, width);
    writeBigEndian32(bytes, 20U, height);
    const std::uint32_t crc =
        pngCrc32(std::span<const std::uint8_t>(bytes).subspan(12U, 17U));
    writeBigEndian32(bytes, 29U, crc);
    return bytes;
}

bool jpegStartOfFrame(std::uint8_t marker) {
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

std::uint16_t readBigEndian16(const std::vector<std::uint8_t>& bytes,
                              std::size_t offset) {
    check(offset <= bytes.size() && bytes.size() - offset >= 2U,
          "invalid synthetic JPEG read offset");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1U]));
}

void writeBigEndian16(std::vector<std::uint8_t>& bytes, std::size_t offset,
                      std::uint16_t value) {
    check(offset <= bytes.size() && bytes.size() - offset >= 2U,
          "invalid synthetic JPEG patch offset");
    bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> jpegWithDimensions(const QByteArray& source,
                                             std::uint16_t width,
                                             std::uint16_t height) {
    auto bytes = byteVector(source);
    std::size_t offset = 2U;
    while (offset < bytes.size()) {
        check(bytes[offset] == 0xffU,
              "unexpected marker layout in synthetic JPEG");
        while (offset < bytes.size() && bytes[offset] == 0xffU) ++offset;
        check(offset < bytes.size(), "truncated synthetic JPEG marker");
        const std::uint8_t marker = bytes[offset++];
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        check(bytes.size() - offset >= 2U,
              "truncated synthetic JPEG segment");
        const std::uint16_t segment_length = readBigEndian16(bytes, offset);
        check(segment_length >= 2U &&
                  static_cast<std::size_t>(segment_length) <=
                      bytes.size() - offset,
              "invalid synthetic JPEG segment length");
        if (jpegStartOfFrame(marker)) {
            check(segment_length >= 8U,
                  "synthetic JPEG frame header is too short");
            writeBigEndian16(bytes, offset + 3U, height);
            writeBigEndian16(bytes, offset + 5U, width);
            return bytes;
        }
        offset += static_cast<std::size_t>(segment_length);
    }
    throw std::runtime_error("synthetic JPEG has no frame header");
}

void testValidAndBoundedDecodes(const QByteArray& png,
                                const QByteArray& jpeg) {
    const RasterDecodeLimits limits;
    const auto png_result = decodeRasterImage(byteView(png), limits);
    check(png_result.ok() && png_result.format == RasterImageFormat::png &&
              png_result.source_pixel_size == QSize(19, 11),
          "valid bounded PNG did not decode");
    checkBoundedSuccess(png_result, limits, "PNG exceeded declared limits");

    const auto jpeg_result = decodeRasterImage(byteView(jpeg), limits);
    check(jpeg_result.ok() && jpeg_result.format == RasterImageFormat::jpeg &&
              jpeg_result.source_pixel_size == QSize(19, 11),
          "valid bounded JPEG did not decode");
    checkBoundedSuccess(jpeg_result, limits,
                        "JPEG exceeded declared limits");
}

void testRejectionsAndLimitPreflight(const QByteArray& png,
                                     const QByteArray& jpeg) {
    const std::array<std::uint8_t, 12> gif{
        'G', 'I', 'F', '8', '9', 'a', 1U, 0U, 1U, 0U, 0U, 0U};
    check(decodeRasterImage(gif).status ==
              RasterDecodeStatus::unsupported_format,
          "unsupported raster format was admitted");
    check(decodeRasterImage({}).status == RasterDecodeStatus::empty_input,
          "empty raster input was not rejected");

    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = static_cast<std::size_t>(png.size() - 1);
    check(decodeRasterImage(byteView(png), limits).status ==
              RasterDecodeStatus::encoded_size_limit,
          "encoded-byte limit was not enforced");

    limits = {};
    limits.maximum_dimension = 18;
    check(decodeRasterImage(byteView(png), limits).status ==
              RasterDecodeStatus::dimension_limit,
          "dimension preflight was not enforced");

    limits = {};
    limits.maximum_pixels = 200;
    check(decodeRasterImage(byteView(jpeg), limits).status ==
              RasterDecodeStatus::pixel_limit,
          "pixel-count preflight was not enforced");

    limits = {};
    limits.maximum_decoded_bytes = 1'600;
    check(decodeRasterImage(byteView(png), limits).status ==
              RasterDecodeStatus::decoded_byte_limit,
          "decoded-byte preflight was not enforced");

    limits = {};
    limits.maximum_dimension = 0;
    check(decodeRasterImage(byteView(jpeg), limits).status ==
              RasterDecodeStatus::invalid_limits,
          "invalid caller limits were not rejected");

    auto truncated_png = byteVector(png);
    truncated_png.pop_back();
    check(decodeRasterImage(truncated_png).status ==
              RasterDecodeStatus::malformed_structure,
          "truncated PNG was not rejected structurally");
    auto truncated_jpeg = byteVector(jpeg);
    truncated_jpeg.pop_back();
    check(decodeRasterImage(truncated_jpeg).status ==
              RasterDecodeStatus::malformed_structure,
          "truncated JPEG was not rejected structurally");

    auto corrupt_png = byteVector(png);
    check(corrupt_png.size() > 29U,
          "PNG fixture is too short for CRC corruption test");
    corrupt_png[29U] ^= 0x01U;
    check(decodeRasterImage(corrupt_png).status ==
              RasterDecodeStatus::malformed_structure,
          "same-format PNG with a corrupt chunk CRC was accepted");

    auto corrupt_jpeg = byteVector(jpeg);
    check(corrupt_jpeg.size() > 5U && corrupt_jpeg[2U] == 0xffU,
          "JPEG fixture is too short for segment corruption test");
    corrupt_jpeg[4U] = 0U;
    corrupt_jpeg[5U] = 1U;
    check(decodeRasterImage(corrupt_jpeg).status ==
              RasterDecodeStatus::malformed_structure,
          "same-format JPEG with an invalid segment length was accepted");

    const auto zero_width_png = pngWithDimensions(png, 0U, 11U);
    check(decodeRasterImage(zero_width_png).status ==
              RasterDecodeStatus::invalid_dimensions,
          "zero-width PNG was not rejected before decode");

    const auto png_bomb = pngWithDimensions(png, 1'000'000U, 1'000'000U);
    check(decodeRasterImage(png_bomb).status ==
              RasterDecodeStatus::dimension_limit,
          "bomb-like PNG header reached the decoder");
    const auto jpeg_bomb = jpegWithDimensions(jpeg, 32'767U, 32'767U);
    check(decodeRasterImage(jpeg_bomb).status ==
              RasterDecodeStatus::dimension_limit,
          "bomb-like JPEG header reached the decoder");
}

void testBoundedDecodeCache(const QByteArray& png, const QByteArray& jpeg) {
    RasterCacheLimits limits;
    limits.maximum_references = 3U;
    limits.maximum_unique_images = 2U;
    BoundedRasterCache repeated(limits);
    const auto first = repeated.decode("word/media/owl.png", byteView(png));
    const auto second = repeated.decode("word/media/owl.png", byteView(png));
    const auto third = repeated.decode("word/media/owl.png", byteView(png));
    const auto fourth = repeated.decode("word/media/owl.png", byteView(png));
    check(first.status == RasterCacheStatus::decoded && first.ok(),
          "cache did not decode its first package image");
    check(second.status == RasterCacheStatus::cache_hit && second.ok() &&
              third.status == RasterCacheStatus::cache_hit && third.ok(),
          "repeated package image was decoded more than once");
    check(first.image.constBits() == second.image.constBits() &&
              second.image.constBits() == third.image.constBits(),
          "repeated package image did not share decoded pixel storage");
    check(fourth.status == RasterCacheStatus::reference_limit &&
              !fourth.ok() && repeated.referenceCount() == 3U &&
              repeated.uniqueImageCount() == 1U,
          "image-reference amplification limit was not enforced");
    check(repeated.aggregateDecodedBytes() ==
              static_cast<std::int64_t>(first.image.sizeInBytes()),
          "cache charged repeated image pixels more than once");

    const auto standalone = decodeRasterImage(byteView(jpeg));
    check(standalone.ok(), "JPEG cache budget fixture did not decode");
    RasterCacheLimits aggregate_limits;
    aggregate_limits.maximum_aggregate_decoded_bytes =
        static_cast<std::int64_t>(standalone.image.sizeInBytes()) + 1LL;
    BoundedRasterCache aggregate(aggregate_limits);
    const auto aggregate_first =
        aggregate.decode("word/media/first.jpg", byteView(jpeg));
    const auto aggregate_second =
        aggregate.decode("word/media/second.jpg", byteView(jpeg));
    check(aggregate_first.ok() &&
              aggregate_second.status ==
                  RasterCacheStatus::aggregate_decoded_byte_limit &&
              !aggregate_second.ok(),
          "aggregate decoded-pixel limit was not enforced before allocation");
    check(aggregate.aggregateDecodedBytes() ==
              static_cast<std::int64_t>(aggregate_first.image.sizeInBytes()),
          "rejected aggregate image changed cache accounting");

    RasterCacheLimits unique_limits;
    unique_limits.maximum_unique_images = 1U;
    BoundedRasterCache unique(unique_limits);
    check(unique.decode("one", byteView(png)).ok(),
          "unique-image cache fixture did not decode");
    check(unique.decode("two", byteView(jpeg)).status ==
              RasterCacheStatus::unique_image_limit,
          "unique-image limit was not enforced");
}

void exerciseMutation(const std::vector<std::uint8_t>& bytes,
                      const RasterDecodeLimits& limits) {
    const auto result = decodeRasterImage(bytes, limits);
    if (!result.ok()) {
        check(result.image.isNull(), "failed decode retained image pixels");
        return;
    }
    check(result.format == RasterImageFormat::png ||
              result.format == RasterImageFormat::jpeg,
          "mutation decoded through an unapproved image codec");
    checkBoundedSuccess(result, limits,
                        "accepted mutation exceeded decode limits");
}

void testDeterministicMutationSmoke(const QByteArray& png,
                                    const QByteArray& jpeg) {
    RasterDecodeLimits limits;
    limits.maximum_encoded_bytes = 2U * 1024U * 1024U;
    limits.maximum_dimension = 512;
    limits.maximum_pixels = 512LL * 512LL;
    limits.maximum_decoded_bytes = 2LL * 1024LL * 1024LL;

    for (const QByteArray* seed : {&png, &jpeg}) {
        const auto original = byteVector(*seed);
        const std::size_t stride = std::max<std::size_t>(1U,
                                                         original.size() / 97U);
        for (std::size_t offset = 0; offset < original.size();
             offset += stride) {
            auto mutation = original;
            mutation[offset] ^= static_cast<std::uint8_t>(
                0x5aU ^ static_cast<std::uint8_t>(offset & 0xffU));
            exerciseMutation(mutation, limits);
        }
        const std::size_t truncation_stride =
            std::max<std::size_t>(1U, original.size() / 41U);
        for (std::size_t size = 0; size < original.size();
             size += truncation_stride) {
            exerciseMutation(
                std::vector<std::uint8_t>(original.begin(),
                                          original.begin() +
                                              static_cast<std::ptrdiff_t>(size)),
                limits);
        }
    }

    std::uint64_t state = 0x6f776c2d646f6373ULL;
    const auto next = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    for (std::size_t sample = 0; sample < 512U; ++sample) {
        const std::size_t size = static_cast<std::size_t>(next() % 4'096U);
        std::vector<std::uint8_t> bytes(size);
        for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(next() & 0xffU);
        }
        exerciseMutation(bytes, limits);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        const QByteArray png = encodedImage("PNG");
        const QByteArray jpeg = encodedImage("JPEG");
        testValidAndBoundedDecodes(png, jpeg);
        testRejectionsAndLimitPreflight(png, jpeg);
        testBoundedDecodeCache(png, jpeg);
        testDeterministicMutationSmoke(png, jpeg);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "raster decode tests passed\n";
    return 0;
}
