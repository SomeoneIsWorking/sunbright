#include <sunbright/native_render/image_decode.h>

#include <algorithm>
#include <array>
#include <limits>

namespace sb::native_render {
namespace {

constexpr std::uint32_t kMaximumImageExtent = 1024;

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

struct TileInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes = 0;
};

bool tile_info(EncodedImageFormat format, TileInfo& result) noexcept {
    switch (format) {
    case EncodedImageFormat::Intensity4:
    case EncodedImageFormat::Indexed4:
    case EncodedImageFormat::BlockCompressed:
        result = {8, 8, 32};
        return true;
    case EncodedImageFormat::Intensity8:
    case EncodedImageFormat::IntensityAlpha4:
    case EncodedImageFormat::Indexed8:
        result = {8, 4, 32};
        return true;
    case EncodedImageFormat::IntensityAlpha8:
    case EncodedImageFormat::Rgb565:
    case EncodedImageFormat::Rgb5A3:
    case EncodedImageFormat::Indexed14:
        result = {4, 4, 32};
        return true;
    case EncodedImageFormat::Rgba8:
        result = {4, 4, 64};
        return true;
    }
    return false;
}

bool indexed(EncodedImageFormat format) noexcept {
    return format == EncodedImageFormat::Indexed4 || format == EncodedImageFormat::Indexed8 ||
           format == EncodedImageFormat::Indexed14;
}

bool valid_palette_format(PaletteFormat format) noexcept {
    switch (format) {
    case PaletteFormat::IntensityAlpha8:
    case PaletteFormat::Rgb565:
    case PaletteFormat::Rgb5A3:
        return true;
    }
    return false;
}

std::uint32_t maximum_palette_entries(EncodedImageFormat format) noexcept {
    switch (format) {
    case EncodedImageFormat::Indexed4:
        return 16;
    case EncodedImageFormat::Indexed8:
        return 256;
    case EncodedImageFormat::Indexed14:
        return 16384;
    default:
        return 0;
    }
}

std::uint16_t read_be16(const std::uint8_t* source) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(source[0]) << 8U) | source[1]);
}

std::uint32_t read_be32(const std::uint8_t* source) noexcept {
    return (static_cast<std::uint32_t>(source[0]) << 24U) |
           (static_cast<std::uint32_t>(source[1]) << 16U) |
           (static_cast<std::uint32_t>(source[2]) << 8U) | source[3];
}

std::uint8_t expand3(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 5U) | (value << 2U) | (value >> 1U));
}

std::uint8_t expand4(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 4U) | value);
}

std::uint8_t expand5(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
}

std::uint8_t expand6(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

Rgba rgb565(std::uint16_t value) noexcept {
    return {expand5((value >> 11U) & 0x1fU), expand6((value >> 5U) & 0x3fU), expand5(value & 0x1fU),
            255};
}

Rgba rgb5a3(std::uint16_t value) noexcept {
    if ((value & 0x8000U) != 0) {
        return {expand5((value >> 10U) & 0x1fU), expand5((value >> 5U) & 0x1fU),
                expand5(value & 0x1fU), 255};
    }
    return {expand4((value >> 8U) & 0x0fU), expand4((value >> 4U) & 0x0fU), expand4(value & 0x0fU),
            expand3((value >> 12U) & 0x07U)};
}

std::uint8_t s3tc_blend(std::uint8_t a, std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>((3U * a + 5U * b) >> 3U);
}

std::uint8_t half_blend(std::uint8_t a, std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(a) + b) >> 1U);
}

void put(std::span<std::uint8_t> output, std::uint32_t width, std::uint32_t height, std::uint32_t x,
         std::uint32_t y, Rgba value) noexcept {
    if (x >= width || y >= height)
        return;
    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    output[offset] = value.r;
    output[offset + 1] = value.g;
    output[offset + 2] = value.b;
    output[offset + 3] = value.a;
}

bool palette_color(const EncodedImageView& source, std::uint32_t index, Rgba& color) noexcept {
    if (index >= source.paletteEntries)
        return false;
    const std::uint8_t* entry = source.palette.data() + index * 2U;
    switch (source.paletteFormat) {
    case PaletteFormat::IntensityAlpha8:
        color = {entry[1], entry[1], entry[1], entry[0]};
        return true;
    case PaletteFormat::Rgb565:
        color = rgb565(read_be16(entry));
        return true;
    case PaletteFormat::Rgb5A3:
        color = rgb5a3(read_be16(entry));
        return true;
    }
    return false;
}

ImageDecodeError validate_source(const EncodedImageView& source,
                                 std::size_t& sourceBytes) noexcept {
    TileInfo tile{};
    if (!tile_info(source.format, tile))
        return ImageDecodeError::UnsupportedFormat;
    if (source.width == 0 || source.height == 0 || source.width > kMaximumImageExtent ||
        source.height > kMaximumImageExtent) {
        return ImageDecodeError::InvalidExtent;
    }
    if (!encoded_image_data_size(source.width, source.height, source.format, sourceBytes))
        return ImageDecodeError::SizeOverflow;
    if (source.pixels.size() < sourceBytes)
        return ImageDecodeError::SourceTooShort;

    if (!indexed(source.format)) {
        if (source.paletteEntries != 0 || !source.palette.empty())
            return ImageDecodeError::UnexpectedPalette;
        return ImageDecodeError::None;
    }
    if (source.paletteEntries == 0 || source.palette.empty())
        return ImageDecodeError::PaletteRequired;
    if (!valid_palette_format(source.paletteFormat))
        return ImageDecodeError::UnsupportedPaletteFormat;
    if (source.paletteEntries > maximum_palette_entries(source.format))
        return ImageDecodeError::InvalidPaletteCount;
    const std::size_t paletteBytes = static_cast<std::size_t>(source.paletteEntries) * 2U;
    if (source.palette.size() < paletteBytes)
        return ImageDecodeError::PaletteTooShort;
    return ImageDecodeError::None;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}

} // namespace

bool decode_image_format(std::uint8_t raw, EncodedImageFormat& format) noexcept {
    switch (raw) {
    case 0:
        format = EncodedImageFormat::Intensity4;
        return true;
    case 1:
        format = EncodedImageFormat::Intensity8;
        return true;
    case 2:
        format = EncodedImageFormat::IntensityAlpha4;
        return true;
    case 3:
        format = EncodedImageFormat::IntensityAlpha8;
        return true;
    case 4:
        format = EncodedImageFormat::Rgb565;
        return true;
    case 5:
        format = EncodedImageFormat::Rgb5A3;
        return true;
    case 6:
        format = EncodedImageFormat::Rgba8;
        return true;
    case 8:
        format = EncodedImageFormat::Indexed4;
        return true;
    case 9:
        format = EncodedImageFormat::Indexed8;
        return true;
    case 10:
        format = EncodedImageFormat::Indexed14;
        return true;
    case 14:
        format = EncodedImageFormat::BlockCompressed;
        return true;
    default:
        return false;
    }
}

bool decode_palette_format(std::uint8_t raw, PaletteFormat& format) noexcept {
    switch (raw) {
    case 0:
        format = PaletteFormat::IntensityAlpha8;
        return true;
    case 1:
        format = PaletteFormat::Rgb565;
        return true;
    case 2:
        format = PaletteFormat::Rgb5A3;
        return true;
    default:
        return false;
    }
}

bool encoded_image_data_size(std::uint32_t width, std::uint32_t height, EncodedImageFormat format,
                             std::size_t& bytes) noexcept {
    bytes = 0;
    TileInfo tile{};
    if (width == 0 || height == 0 || width > kMaximumImageExtent || height > kMaximumImageExtent ||
        !tile_info(format, tile)) {
        return false;
    }
    const std::uint64_t tilesWide =
        (static_cast<std::uint64_t>(width) + tile.width - 1U) / tile.width;
    const std::uint64_t tilesHigh =
        (static_cast<std::uint64_t>(height) + tile.height - 1U) / tile.height;
    if (tilesWide > std::numeric_limits<std::uint64_t>::max() / tilesHigh ||
        tilesWide * tilesHigh > std::numeric_limits<std::uint64_t>::max() / tile.bytes) {
        return false;
    }
    const std::uint64_t total = tilesWide * tilesHigh * tile.bytes;
    if (total > std::numeric_limits<std::size_t>::max())
        return false;
    bytes = static_cast<std::size_t>(total);
    return true;
}

bool encoded_image_chain_size(std::uint32_t width, std::uint32_t height, EncodedImageFormat format,
                              std::uint32_t levelCount, std::size_t& bytes) noexcept {
    bytes = 0;
    if (levelCount == 0)
        return false;
    std::uint32_t remainingWidth = width;
    std::uint32_t remainingHeight = height;
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        std::size_t levelBytes = 0;
        if (!encoded_image_data_size(remainingWidth, remainingHeight, format, levelBytes) ||
            levelBytes > std::numeric_limits<std::size_t>::max() - bytes) {
            bytes = 0;
            return false;
        }
        bytes += levelBytes;
        if (level + 1U < levelCount && remainingWidth == 1 && remainingHeight == 1) {
            bytes = 0;
            return false;
        }
        remainingWidth = std::max(remainingWidth >> 1U, 1U);
        remainingHeight = std::max(remainingHeight >> 1U, 1U);
    }
    return true;
}

bool decoded_image_data_size(std::uint32_t width, std::uint32_t height,
                             std::size_t& bytes) noexcept {
    bytes = 0;
    if (width == 0 || height == 0 || width > kMaximumImageExtent || height > kMaximumImageExtent) {
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4U)
        return false;
    bytes = static_cast<std::size_t>(pixels * 4U);
    return true;
}

const char* encoded_image_format_name(EncodedImageFormat format) noexcept {
    switch (format) {
    case EncodedImageFormat::Intensity4:
        return "Intensity4";
    case EncodedImageFormat::Intensity8:
        return "Intensity8";
    case EncodedImageFormat::IntensityAlpha4:
        return "IntensityAlpha4";
    case EncodedImageFormat::IntensityAlpha8:
        return "IntensityAlpha8";
    case EncodedImageFormat::Rgb565:
        return "Rgb565";
    case EncodedImageFormat::Rgb5A3:
        return "Rgb5A3";
    case EncodedImageFormat::Rgba8:
        return "Rgba8";
    case EncodedImageFormat::Indexed4:
        return "Indexed4";
    case EncodedImageFormat::Indexed8:
        return "Indexed8";
    case EncodedImageFormat::Indexed14:
        return "Indexed14";
    case EncodedImageFormat::BlockCompressed:
        return "BlockCompressed";
    }
    return "Unknown";
}

const char* palette_format_name(PaletteFormat format) noexcept {
    switch (format) {
    case PaletteFormat::IntensityAlpha8:
        return "IntensityAlpha8";
    case PaletteFormat::Rgb565:
        return "Rgb565";
    case PaletteFormat::Rgb5A3:
        return "Rgb5A3";
    }
    return "Unknown";
}

const char* image_decode_error_name(ImageDecodeError error) noexcept {
    switch (error) {
    case ImageDecodeError::None:
        return "None";
    case ImageDecodeError::UnsupportedFormat:
        return "UnsupportedFormat";
    case ImageDecodeError::InvalidExtent:
        return "InvalidExtent";
    case ImageDecodeError::SizeOverflow:
        return "SizeOverflow";
    case ImageDecodeError::SourceTooShort:
        return "SourceTooShort";
    case ImageDecodeError::OutputSizeMismatch:
        return "OutputSizeMismatch";
    case ImageDecodeError::PaletteRequired:
        return "PaletteRequired";
    case ImageDecodeError::UnexpectedPalette:
        return "UnexpectedPalette";
    case ImageDecodeError::UnsupportedPaletteFormat:
        return "UnsupportedPaletteFormat";
    case ImageDecodeError::InvalidPaletteCount:
        return "InvalidPaletteCount";
    case ImageDecodeError::PaletteTooShort:
        return "PaletteTooShort";
    case ImageDecodeError::PaletteIndexOutOfRange:
        return "PaletteIndexOutOfRange";
    }
    return "Unknown";
}

ImageDecodeError decode_image_rgba8(const EncodedImageView& source,
                                    std::span<std::uint8_t> rgba8) noexcept {
    std::size_t sourceBytes = 0;
    const ImageDecodeError sourceError = validate_source(source, sourceBytes);
    if (sourceError != ImageDecodeError::None)
        return sourceError;
    std::size_t outputBytes = 0;
    if (!decoded_image_data_size(source.width, source.height, outputBytes))
        return ImageDecodeError::SizeOverflow;
    if (rgba8.size() != outputBytes)
        return ImageDecodeError::OutputSizeMismatch;

    std::fill(rgba8.begin(), rgba8.end(), 0);
    std::size_t cursor = 0;
    switch (source.format) {
    case EncodedImageFormat::Intensity4:
    case EncodedImageFormat::Indexed4:
        for (std::uint32_t tileY = 0; tileY < source.height; tileY += 8) {
            for (std::uint32_t tileX = 0; tileX < source.width; tileX += 8) {
                for (std::uint32_t y = 0; y < 8; ++y) {
                    for (std::uint32_t x = 0; x < 8; x += 2) {
                        const std::uint8_t packed = source.pixels[cursor++];
                        for (std::uint32_t half = 0; half < 2; ++half) {
                            const std::uint8_t value = half == 0 ? packed >> 4U : packed & 0x0fU;
                            Rgba color{};
                            if (source.format == EncodedImageFormat::Intensity4) {
                                const std::uint8_t intensity = expand4(value);
                                color = {intensity, intensity, intensity, intensity};
                            } else if (!palette_color(source, value, color)) {
                                return ImageDecodeError::PaletteIndexOutOfRange;
                            }
                            put(rgba8, source.width, source.height, tileX + x + half, tileY + y,
                                color);
                        }
                    }
                }
            }
        }
        return ImageDecodeError::None;
    case EncodedImageFormat::Intensity8:
    case EncodedImageFormat::IntensityAlpha4:
    case EncodedImageFormat::Indexed8:
        for (std::uint32_t tileY = 0; tileY < source.height; tileY += 4) {
            for (std::uint32_t tileX = 0; tileX < source.width; tileX += 8) {
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 8; ++x) {
                        const std::uint8_t value = source.pixels[cursor++];
                        Rgba color{};
                        if (source.format == EncodedImageFormat::Intensity8) {
                            color = {value, value, value, value};
                        } else if (source.format == EncodedImageFormat::IntensityAlpha4) {
                            const std::uint8_t intensity = expand4(value & 0x0fU);
                            color = {intensity, intensity, intensity, expand4(value >> 4U)};
                        } else if (!palette_color(source, value, color)) {
                            return ImageDecodeError::PaletteIndexOutOfRange;
                        }
                        put(rgba8, source.width, source.height, tileX + x, tileY + y, color);
                    }
                }
            }
        }
        return ImageDecodeError::None;
    case EncodedImageFormat::IntensityAlpha8:
    case EncodedImageFormat::Rgb565:
    case EncodedImageFormat::Rgb5A3:
    case EncodedImageFormat::Indexed14:
        for (std::uint32_t tileY = 0; tileY < source.height; tileY += 4) {
            for (std::uint32_t tileX = 0; tileX < source.width; tileX += 4) {
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const std::uint8_t* texel = source.pixels.data() + cursor;
                        cursor += 2;
                        Rgba color{};
                        if (source.format == EncodedImageFormat::IntensityAlpha8) {
                            color = {texel[1], texel[1], texel[1], texel[0]};
                        } else if (source.format == EncodedImageFormat::Rgb565) {
                            color = rgb565(read_be16(texel));
                        } else if (source.format == EncodedImageFormat::Rgb5A3) {
                            color = rgb5a3(read_be16(texel));
                        } else if (!palette_color(source, read_be16(texel) & 0x3fffU, color)) {
                            return ImageDecodeError::PaletteIndexOutOfRange;
                        }
                        put(rgba8, source.width, source.height, tileX + x, tileY + y, color);
                    }
                }
            }
        }
        return ImageDecodeError::None;
    case EncodedImageFormat::Rgba8:
        for (std::uint32_t tileY = 0; tileY < source.height; tileY += 4) {
            for (std::uint32_t tileX = 0; tileX < source.width; tileX += 4) {
                const std::uint8_t* alphaRed = source.pixels.data() + cursor;
                const std::uint8_t* greenBlue = alphaRed + 32;
                cursor += 64;
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const std::size_t texel = y * 4U + x;
                        put(rgba8, source.width, source.height, tileX + x, tileY + y,
                            {alphaRed[texel * 2U + 1U], greenBlue[texel * 2U],
                             greenBlue[texel * 2U + 1U], alphaRed[texel * 2U]});
                    }
                }
            }
        }
        return ImageDecodeError::None;
    case EncodedImageFormat::BlockCompressed:
        for (std::uint32_t tileY = 0; tileY < source.height; tileY += 8) {
            for (std::uint32_t tileX = 0; tileX < source.width; tileX += 8) {
                for (std::uint32_t block = 0; block < 4; ++block) {
                    const std::uint16_t endpoint0 = read_be16(source.pixels.data() + cursor);
                    const std::uint16_t endpoint1 = read_be16(source.pixels.data() + cursor + 2);
                    const std::uint32_t selectors = read_be32(source.pixels.data() + cursor + 4);
                    cursor += 8;
                    std::array<Rgba, 4> colors{rgb565(endpoint0), rgb565(endpoint1)};
                    if (endpoint0 > endpoint1) {
                        colors[2] = {s3tc_blend(colors[1].r, colors[0].r),
                                     s3tc_blend(colors[1].g, colors[0].g),
                                     s3tc_blend(colors[1].b, colors[0].b), 255};
                        colors[3] = {s3tc_blend(colors[0].r, colors[1].r),
                                     s3tc_blend(colors[0].g, colors[1].g),
                                     s3tc_blend(colors[0].b, colors[1].b), 255};
                    } else {
                        colors[2] = {half_blend(colors[0].r, colors[1].r),
                                     half_blend(colors[0].g, colors[1].g),
                                     half_blend(colors[0].b, colors[1].b), 255};
                        colors[3] = colors[2];
                        colors[3].a = 0;
                    }
                    const std::uint32_t originX = tileX + (block & 1U) * 4U;
                    const std::uint32_t originY = tileY + (block >> 1U) * 4U;
                    for (std::uint32_t y = 0; y < 4; ++y) {
                        for (std::uint32_t x = 0; x < 4; ++x) {
                            const unsigned shift = 30U - (y * 4U + x) * 2U;
                            put(rgba8, source.width, source.height, originX + x, originY + y,
                                colors[(selectors >> shift) & 3U]);
                        }
                    }
                }
            }
        }
        return ImageDecodeError::None;
    }
    return ImageDecodeError::UnsupportedFormat;
}

bool image_content_revision(const EncodedImageView& source, std::uint64_t& revision) noexcept {
    std::size_t sourceBytes = 0;
    if (validate_source(source, sourceBytes) != ImageDecodeError::None)
        return false;
    std::uint64_t hash = 1469598103934665603ULL;
    hash_u32(hash, source.width);
    hash_u32(hash, source.height);
    hash_byte(hash, static_cast<std::uint8_t>(source.format));
    for (std::uint8_t byte : source.pixels.first(sourceBytes))
        hash_byte(hash, byte);
    if (indexed(source.format)) {
        hash_byte(hash, static_cast<std::uint8_t>(source.paletteFormat));
        hash_u32(hash, source.paletteEntries);
        const std::size_t paletteBytes = static_cast<std::size_t>(source.paletteEntries) * 2U;
        for (std::uint8_t byte : source.palette.first(paletteBytes))
            hash_byte(hash, byte);
    }
    revision = hash;
    return true;
}

} // namespace sb::native_render
