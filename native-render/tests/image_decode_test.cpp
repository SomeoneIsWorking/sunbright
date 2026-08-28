#include <sunbright/native_render/image_decode.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using sb::native_render::EncodedImageFormat;
using sb::native_render::EncodedImageView;
using sb::native_render::ImageDecodeError;
using sb::native_render::PaletteFormat;

using Pixel = std::array<std::uint8_t, 4>;

std::vector<std::uint8_t> source_for(EncodedImageFormat format, std::uint32_t width = 1,
                                     std::uint32_t height = 1) {
    std::size_t bytes = 0;
    assert(sb::native_render::encoded_image_data_size(width, height, format, bytes));
    return std::vector<std::uint8_t>(bytes);
}

std::vector<std::uint8_t> decode(const EncodedImageView& source) {
    std::size_t outputBytes = 0;
    assert(sb::native_render::decoded_image_data_size(source.width, source.height, outputBytes));
    std::vector<std::uint8_t> output(outputBytes);
    assert(sb::native_render::decode_image_rgba8(source, output) == ImageDecodeError::None);
    return output;
}

Pixel pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t x,
            std::uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
    return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
}

void set_be16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void check_sizes_and_raw_formats() {
    constexpr std::array formats{
        EncodedImageFormat::Intensity4,      EncodedImageFormat::Intensity8,
        EncodedImageFormat::IntensityAlpha4, EncodedImageFormat::IntensityAlpha8,
        EncodedImageFormat::Rgb565,          EncodedImageFormat::Rgb5A3,
        EncodedImageFormat::Rgba8,           EncodedImageFormat::Indexed4,
        EncodedImageFormat::Indexed8,        EncodedImageFormat::Indexed14,
        EncodedImageFormat::BlockCompressed,
    };
    constexpr std::array<std::uint8_t, formats.size()> raw{0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14};
    for (std::size_t index = 0; index < formats.size(); ++index) {
        EncodedImageFormat decoded{};
        assert(sb::native_render::decode_image_format(raw[index], decoded));
        assert(decoded == formats[index]);
        assert(sb::native_render::encoded_image_format_name(decoded)[0] != '\0');
    }
    EncodedImageFormat imageFormat{};
    assert(!sb::native_render::decode_image_format(7, imageFormat));

    PaletteFormat paletteFormat{};
    assert(sb::native_render::decode_palette_format(0, paletteFormat));
    assert(paletteFormat == PaletteFormat::IntensityAlpha8);
    assert(sb::native_render::decode_palette_format(1, paletteFormat));
    assert(paletteFormat == PaletteFormat::Rgb565);
    assert(sb::native_render::decode_palette_format(2, paletteFormat));
    assert(paletteFormat == PaletteFormat::Rgb5A3);
    assert(!sb::native_render::decode_palette_format(3, paletteFormat));

    std::size_t bytes = 0;
    assert(
        sb::native_render::encoded_image_data_size(9, 9, EncodedImageFormat::Intensity4, bytes) &&
        bytes == 128);
    assert(
        sb::native_render::encoded_image_data_size(9, 5, EncodedImageFormat::Intensity8, bytes) &&
        bytes == 128);
    assert(sb::native_render::encoded_image_data_size(5, 5, EncodedImageFormat::Rgba8, bytes) &&
           bytes == 256);
    assert(sb::native_render::encoded_image_data_size(9, 9, EncodedImageFormat::BlockCompressed,
                                                      bytes) &&
           bytes == 128);
    assert(!sb::native_render::encoded_image_data_size(0, 1, EncodedImageFormat::Rgba8, bytes));
    assert(!sb::native_render::encoded_image_data_size(1025, 1, EncodedImageFormat::Rgba8, bytes));
    assert(sb::native_render::decoded_image_data_size(1024, 1024, bytes) &&
           bytes == 1024U * 1024U * 4U);
    assert(!sb::native_render::decoded_image_data_size(1, 1025, bytes));

    assert(sb::native_render::encoded_image_chain_size(8, 8, EncodedImageFormat::Intensity4, 4,
                                                       bytes) &&
           bytes == 128);
    assert(!sb::native_render::encoded_image_chain_size(8, 8, EncodedImageFormat::Intensity4, 5,
                                                        bytes));
    assert(!sb::native_render::encoded_image_chain_size(8, 8, EncodedImageFormat::Intensity4, 0,
                                                        bytes));
}

void check_direct_formats() {
    auto intensity4 = source_for(EncodedImageFormat::Intensity4, 2, 1);
    intensity4[0] = 0x1f;
    auto rgba = decode({EncodedImageFormat::Intensity4, 2, 1, intensity4});
    assert((pixel(rgba, 2, 0, 0) == Pixel{17, 17, 17, 17}));
    assert((pixel(rgba, 2, 1, 0) == Pixel{255, 255, 255, 255}));

    auto intensity8 = source_for(EncodedImageFormat::Intensity8, 9, 1);
    intensity8[0] = 0x11;
    intensity8[32] = 0x22;
    rgba = decode({EncodedImageFormat::Intensity8, 9, 1, intensity8});
    assert((pixel(rgba, 9, 0, 0) == Pixel{17, 17, 17, 17}));
    assert((pixel(rgba, 9, 8, 0) == Pixel{34, 34, 34, 34}));

    auto intensityAlpha4 = source_for(EncodedImageFormat::IntensityAlpha4);
    intensityAlpha4[0] = 0xa3;
    rgba = decode({EncodedImageFormat::IntensityAlpha4, 1, 1, intensityAlpha4});
    assert((pixel(rgba, 1, 0, 0) == Pixel{51, 51, 51, 170}));

    auto intensityAlpha8 = source_for(EncodedImageFormat::IntensityAlpha8);
    intensityAlpha8[0] = 128;
    intensityAlpha8[1] = 32;
    rgba = decode({EncodedImageFormat::IntensityAlpha8, 1, 1, intensityAlpha8});
    assert((pixel(rgba, 1, 0, 0) == Pixel{32, 32, 32, 128}));

    auto rgb565 = source_for(EncodedImageFormat::Rgb565);
    set_be16(rgb565, 0, 0x8410);
    rgba = decode({EncodedImageFormat::Rgb565, 1, 1, rgb565});
    assert((pixel(rgba, 1, 0, 0) == Pixel{132, 130, 132, 255}));

    auto rgb5a3 = source_for(EncodedImageFormat::Rgb5A3);
    set_be16(rgb5a3, 0, 0xc210);
    rgba = decode({EncodedImageFormat::Rgb5A3, 1, 1, rgb5a3});
    assert((pixel(rgba, 1, 0, 0) == Pixel{132, 132, 132, 255}));
    set_be16(rgb5a3, 0, 0x3888);
    rgba = decode({EncodedImageFormat::Rgb5A3, 1, 1, rgb5a3});
    assert((pixel(rgba, 1, 0, 0) == Pixel{136, 136, 136, 109}));

    auto rgba8 = source_for(EncodedImageFormat::Rgba8);
    rgba8[0] = 128;
    rgba8[1] = 10;
    rgba8[32] = 20;
    rgba8[33] = 30;
    rgba = decode({EncodedImageFormat::Rgba8, 1, 1, rgba8});
    assert((pixel(rgba, 1, 0, 0) == Pixel{10, 20, 30, 128}));
}

void check_indexed_formats() {
    auto indexed4 = source_for(EncodedImageFormat::Indexed4);
    indexed4[0] = 0xf0;
    std::array<std::uint8_t, 32> palette{};
    set_be16(palette, 30, 0xf800);
    auto rgba =
        decode({EncodedImageFormat::Indexed4, 1, 1, indexed4, PaletteFormat::Rgb565, 16, palette});
    assert((pixel(rgba, 1, 0, 0) == Pixel{255, 0, 0, 255}));
    palette[30] = 64;
    palette[31] = 32;
    rgba = decode({EncodedImageFormat::Indexed4, 1, 1, indexed4, PaletteFormat::IntensityAlpha8, 16,
                   palette});
    assert((pixel(rgba, 1, 0, 0) == Pixel{32, 32, 32, 64}));

    auto indexed8 = source_for(EncodedImageFormat::Indexed8);
    indexed8[0] = 1;
    set_be16(palette, 2, 0x83e0);
    rgba =
        decode({EncodedImageFormat::Indexed8, 1, 1, indexed8, PaletteFormat::Rgb5A3, 16, palette});
    assert((pixel(rgba, 1, 0, 0) == Pixel{0, 255, 0, 255}));

    auto indexed14 = source_for(EncodedImageFormat::Indexed14);
    set_be16(indexed14, 0, 0xc001);
    rgba = decode(
        {EncodedImageFormat::Indexed14, 1, 1, indexed14, PaletteFormat::Rgb5A3, 16, palette});
    assert((pixel(rgba, 1, 0, 0) == Pixel{0, 255, 0, 255}));
}

void check_block_compressed() {
    auto compressed = source_for(EncodedImageFormat::BlockCompressed, 8, 8);
    set_be16(compressed, 0, 0xffff);
    set_be16(compressed, 2, 0x0000);
    compressed[4] = 0xb0;
    set_be16(compressed, 8, 0x07e0);
    set_be16(compressed, 10, 0x0000);
    set_be16(compressed, 16, 0x001f);
    set_be16(compressed, 18, 0x0000);
    set_be16(compressed, 24, 0xffff);
    set_be16(compressed, 26, 0x0000);

    auto rgba = decode({EncodedImageFormat::BlockCompressed, 8, 8, compressed});
    assert((pixel(rgba, 8, 0, 0) == Pixel{159, 159, 159, 255}));
    assert((pixel(rgba, 8, 1, 0) == Pixel{95, 95, 95, 255}));
    assert((pixel(rgba, 8, 4, 0) == Pixel{0, 255, 0, 255}));
    assert((pixel(rgba, 8, 0, 4) == Pixel{0, 0, 255, 255}));
    assert((pixel(rgba, 8, 4, 4) == Pixel{255, 255, 255, 255}));

    auto transparent = source_for(EncodedImageFormat::BlockCompressed);
    set_be16(transparent, 0, 0x0000);
    set_be16(transparent, 2, 0xffff);
    transparent[4] = 0xc0;
    rgba = decode({EncodedImageFormat::BlockCompressed, 1, 1, transparent});
    assert((pixel(rgba, 1, 0, 0) == Pixel{127, 127, 127, 0}));
}

void check_fail_fast_contracts() {
    auto rgba8 = source_for(EncodedImageFormat::Rgba8);
    std::array<std::uint8_t, 4> output{};
    const EncodedImageView source{EncodedImageFormat::Rgba8, 1, 1, rgba8};
    assert(sb::native_render::decode_image_rgba8(source, output) == ImageDecodeError::None);
    assert(sb::native_render::decode_image_rgba8(source, std::span(output).first(3)) ==
           ImageDecodeError::OutputSizeMismatch);
    rgba8.pop_back();
    assert(sb::native_render::decode_image_rgba8({EncodedImageFormat::Rgba8, 1, 1, rgba8},
                                                 output) == ImageDecodeError::SourceTooShort);

    auto indexed4 = source_for(EncodedImageFormat::Indexed4);
    indexed4[0] = 0xf0;
    std::array<std::uint8_t, 32> palette{};
    assert(sb::native_render::decode_image_rgba8({EncodedImageFormat::Indexed4, 1, 1, indexed4},
                                                 output) == ImageDecodeError::PaletteRequired);
    assert(sb::native_render::decode_image_rgba8(
               {EncodedImageFormat::Indexed4, 1, 1, indexed4, PaletteFormat::Rgb565, 17, palette},
               output) == ImageDecodeError::InvalidPaletteCount);
    assert(sb::native_render::decode_image_rgba8({EncodedImageFormat::Indexed4, 1, 1, indexed4,
                                                  PaletteFormat::Rgb565, 16,
                                                  std::span(palette).first(31)},
                                                 output) == ImageDecodeError::PaletteTooShort);
    assert(sb::native_render::decode_image_rgba8(
               {EncodedImageFormat::Indexed4, 1, 1, indexed4, PaletteFormat::Rgb565, 1,
                std::span(palette).first(2)},
               output) == ImageDecodeError::PaletteIndexOutOfRange);
    assert(sb::native_render::decode_image_rgba8({EncodedImageFormat::Intensity4, 1, 1, indexed4,
                                                  PaletteFormat::Rgb565, 1,
                                                  std::span(palette).first(2)},
                                                 output) == ImageDecodeError::UnexpectedPalette);
}

void check_revisions() {
    auto rgba8 = source_for(EncodedImageFormat::Rgba8);
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    EncodedImageView source{EncodedImageFormat::Rgba8, 1, 1, rgba8};
    assert(sb::native_render::image_content_revision(source, first));
    rgba8[1]++;
    assert(sb::native_render::image_content_revision(source, second));
    assert(first != second);

    auto indexed4 = source_for(EncodedImageFormat::Indexed4);
    std::array<std::uint8_t, 2> palette{};
    source = {EncodedImageFormat::Indexed4, 1, 1, indexed4, PaletteFormat::Rgb565, 1, palette};
    assert(sb::native_render::image_content_revision(source, first));
    palette[1]++;
    assert(sb::native_render::image_content_revision(source, second));
    assert(first != second);
}

} // namespace

int main() {
    check_sizes_and_raw_formats();
    check_direct_formats();
    check_indexed_formats();
    check_block_compressed();
    check_fail_fast_contracts();
    check_revisions();
}
