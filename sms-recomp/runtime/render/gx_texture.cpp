// GX compatibility adapter for the shared game-asset texture decoder. The decoder itself lives in
// native-render and exposes only content encodings plus RGBA8; this file owns the guest-address and
// GX-enum boundary needed by the legacy FIFO renderer.

#include "gx_texture.h"

#include <sunbright/native_render/image_decode.h>

#include <limits>
#include <span>

namespace {

using sb::native_render::EncodedImageFormat;

bool image_format(std::uint32_t format, EncodedImageFormat& result) noexcept {
    // C14 requires the LOADTLUT source/count association that the compatibility FIFO path does not
    // yet retain. The semantic runtime adapters do have that information and use the shared decoder
    // directly.
    if (format == GX_TF_C14X2 || format > std::numeric_limits<std::uint8_t>::max())
        return false;
    return sb::native_render::decode_image_format(static_cast<std::uint8_t>(format), result);
}

bool guest_span(u32 address, std::size_t size, std::span<const std::uint8_t>& bytes) noexcept {
    if (size == 0 || size - 1U > std::numeric_limits<u32>::max() ||
        address > std::numeric_limits<u32>::max() - static_cast<u32>(size - 1U)) {
        return false;
    }
    const u8* first = sb_ram_fast(address);
    const u8* last = sb_ram_fast(address + static_cast<u32>(size - 1U));
    if (first == nullptr || last != first + size - 1U)
        return false;
    bytes = {first, size};
    return true;
}

} // namespace

bool gx_texture_format_supported(std::uint32_t format) {
    EncodedImageFormat converted{};
    return image_format(format, converted);
}

const char* gx_texture_format_name(std::uint32_t format) {
    EncodedImageFormat converted{};
    if (image_format(format, converted))
        return sb::native_render::encoded_image_format_name(converted);
    if (format == GX_TF_C14X2)
        return "Indexed14";
    return "Unknown";
}

std::size_t gx_texture_data_size(std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    EncodedImageFormat converted{};
    std::size_t bytes = 0;
    if (!image_format(format, converted) ||
        !sb::native_render::encoded_image_data_size(width, height, converted, bytes)) {
        return 0;
    }
    return bytes;
}

bool gx_decode_texture(u32 address, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                       std::uint32_t paletteAddress, std::uint8_t* output) {
    EncodedImageFormat converted{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (output == nullptr || width > 1024 || height > 1024 || !image_format(format, converted) ||
        !sb::native_render::encoded_image_data_size(width, height, converted, sourceBytes) ||
        !sb::native_render::decoded_image_data_size(width, height, outputBytes)) {
        return false;
    }

    std::span<const std::uint8_t> pixels;
    if (!guest_span(address, sourceBytes, pixels))
        return false;

    std::uint32_t paletteEntries = 0;
    if (converted == EncodedImageFormat::Indexed4)
        paletteEntries = 16;
    else if (converted == EncodedImageFormat::Indexed8)
        paletteEntries = 256;
    std::span<const std::uint8_t> palette;
    if (paletteEntries != 0 &&
        (paletteAddress == 0 || !guest_span(paletteAddress, paletteEntries * 2U, palette))) {
        return false;
    }

    const sb::native_render::EncodedImageView source{
        converted,      width,   height, pixels, sb::native_render::PaletteFormat::Rgb5A3,
        paletteEntries, palette,
    };
    return sb::native_render::decode_image_rgba8(source, {output, outputBytes}) ==
           sb::native_render::ImageDecodeError::None;
}
