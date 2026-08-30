#include "guest_jut_texture_adapter.h"

#include <sunbright/native_render/image_decode.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace sb::recomp {

bool capture_guest_jut_texture(const BigEndianGuestReader& reader, std::uint32_t textureAddress,
                               CapturedGuestTexture& capture) noexcept {
    CapturedGuestTexture result{};
    std::uint32_t resource = 0;
    std::uint32_t texelAddress = 0;
    std::uint32_t rawFormat = 0;
    std::uint32_t hasAlpha = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t wrapU = 0;
    std::uint8_t wrapV = 0;
    std::uint8_t min = 0;
    std::uint8_t mag = 0;
    if (textureAddress == 0 || !reader.u32(textureAddress + 0x20, resource) || resource == 0 ||
        !reader.u32(textureAddress + 0x24, texelAddress) || texelAddress == 0 ||
        !reader.u32(textureAddress + 0x34, rawFormat) ||
        rawFormat > std::numeric_limits<std::uint8_t>::max() ||
        !reader.u32(textureAddress + 0x38, hasAlpha) || !reader.u16(textureAddress + 0x3c, width) ||
        !reader.u16(textureAddress + 0x3e, height) || !reader.u8(textureAddress + 0x40, wrapU) ||
        !reader.u8(textureAddress + 0x41, wrapV) || !reader.u8(textureAddress + 0x42, min) ||
        !reader.u8(textureAddress + 0x43, mag) || width == 0 || height == 0) {
        return false;
    }

    auto& texture = result.texture;
    texture.resource = resource;
    texture.width = width;
    texture.height = height;
    texture.hasAlpha = hasAlpha != 0;
    if (!native_render::decode_address_mode(wrapU, texture.addressU) ||
        !native_render::decode_address_mode(wrapV, texture.addressV) ||
        !native_render::decode_min_filter(min, texture.minFilter, texture.mipFilter) ||
        !native_render::decode_mag_filter(mag, texture.magFilter) ||
        texture.mipFilter != native_render::MipFilter::None) {
        return false;
    }

    native_render::EncodedImageFormat format{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (!native_render::decode_image_format(static_cast<std::uint8_t>(rawFormat), format) ||
        !native_render::encoded_image_data_size(width, height, format, sourceBytes) ||
        !native_render::decoded_image_data_size(width, height, outputBytes)) {
        return false;
    }
    std::vector<std::uint8_t> encoded(sourceBytes);
    if (!reader.bytes(texelAddress, encoded.data(), encoded.size()))
        return false;

    native_render::PaletteFormat paletteFormat = native_render::PaletteFormat::Rgb5A3;
    std::uint32_t paletteEntries = 0;
    std::vector<std::uint8_t> palette;
    if (format == native_render::EncodedImageFormat::Indexed4 ||
        format == native_render::EncodedImageFormat::Indexed8 ||
        format == native_render::EncodedImageFormat::Indexed14) {
        std::uint32_t paletteObject = 0;
        std::uint32_t rawPaletteFormat = 0;
        std::uint32_t paletteAddress = 0;
        std::uint16_t paletteEntryCount = 0;
        if (!reader.u32(textureAddress + 0x2c, paletteObject) || paletteObject == 0 ||
            !reader.u32(paletteObject + 0x10, rawPaletteFormat) ||
            rawPaletteFormat > std::numeric_limits<std::uint8_t>::max() ||
            !native_render::decode_palette_format(static_cast<std::uint8_t>(rawPaletteFormat),
                                                  paletteFormat) ||
            !reader.u32(paletteObject + 0x14, paletteAddress) || paletteAddress == 0 ||
            !reader.u16(paletteObject + 0x18, paletteEntryCount) || paletteEntryCount == 0) {
            return false;
        }
        paletteEntries = paletteEntryCount;
        palette.resize(static_cast<std::size_t>(paletteEntries) * 2U);
        if (!reader.bytes(paletteAddress, palette.data(), palette.size()))
            return false;
    }

    const native_render::EncodedImageView source{format,        width,          height, encoded,
                                                 paletteFormat, paletteEntries, palette};
    result.rgba8.resize(outputBytes);
    if (native_render::decode_image_rgba8(source, result.rgba8) !=
            native_render::ImageDecodeError::None ||
        !native_render::image_content_revision(source, texture.revision)) {
        return false;
    }
    capture = std::move(result);
    return true;
}

} // namespace sb::recomp
