#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sb::native_render {

// Encodings used by the game's texture assets. These describe content storage, not a GPU API or
// renderer state: both native runtimes decode them to ordinary RGBA8 before the PC renderer sees
// an image.
enum class EncodedImageFormat : std::uint8_t {
    Intensity4 = 0,
    Intensity8 = 1,
    IntensityAlpha4 = 2,
    IntensityAlpha8 = 3,
    Rgb565 = 4,
    Rgb5A3 = 5,
    Rgba8 = 6,
    Indexed4 = 8,
    Indexed8 = 9,
    Indexed14 = 10,
    BlockCompressed = 14,
};

enum class PaletteFormat : std::uint8_t {
    IntensityAlpha8 = 0,
    Rgb565 = 1,
    Rgb5A3 = 2,
};

enum class ImageDecodeError : std::uint8_t {
    None,
    UnsupportedFormat,
    InvalidExtent,
    SizeOverflow,
    SourceTooShort,
    OutputSizeMismatch,
    PaletteRequired,
    UnexpectedPalette,
    UnsupportedPaletteFormat,
    InvalidPaletteCount,
    PaletteTooShort,
    PaletteIndexOutOfRange,
};

struct EncodedImageView {
    EncodedImageFormat format = EncodedImageFormat::Intensity4;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> pixels{};
    PaletteFormat paletteFormat = PaletteFormat::Rgb5A3;
    std::uint32_t paletteEntries = 0;
    std::span<const std::uint8_t> palette{};
};

[[nodiscard]] bool decode_image_format(std::uint8_t raw, EncodedImageFormat& format) noexcept;
[[nodiscard]] bool decode_palette_format(std::uint8_t raw, PaletteFormat& format) noexcept;
[[nodiscard]] bool encoded_image_data_size(std::uint32_t width, std::uint32_t height,
                                           EncodedImageFormat format, std::size_t& bytes) noexcept;
[[nodiscard]] bool encoded_image_chain_size(std::uint32_t width, std::uint32_t height,
                                            EncodedImageFormat format, std::uint32_t levelCount,
                                            std::size_t& bytes) noexcept;
[[nodiscard]] bool decoded_image_data_size(std::uint32_t width, std::uint32_t height,
                                           std::size_t& bytes) noexcept;
[[nodiscard]] ImageDecodeError decode_image_rgba8(const EncodedImageView& source,
                                                  std::span<std::uint8_t> rgba8) noexcept;
[[nodiscard]] const char* encoded_image_format_name(EncodedImageFormat format) noexcept;
[[nodiscard]] const char* palette_format_name(PaletteFormat format) noexcept;
[[nodiscard]] const char* image_decode_error_name(ImageDecodeError error) noexcept;

// Content revision for immutable-keyed GPU caches. It covers the dimensions, encoding, palette
// metadata, and exactly the source bytes the decoder consumes.
[[nodiscard]] bool image_content_revision(const EncodedImageView& source,
                                          std::uint64_t& revision) noexcept;

} // namespace sb::native_render
