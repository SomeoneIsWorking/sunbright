#include "native_jut_texture_adapter.h"

#include <sunbright/native_render/image_decode.h>

#include <JSystem/JUtility/JUTPalette.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>

#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace sb {

native_render::DecodedImageView CapturedNativeTexture::image_view() const noexcept {
    return {texture.resource, texture.revision, texture.width, texture.height, rgba8};
}

bool capture_native_jut_texture(const JUTTexture& source, CapturedNativeTexture& capture,
                                const char*& error) {
    if (source.mTexInfo == nullptr || source.mTexData == nullptr ||
        source.mFormat > std::numeric_limits<std::uint8_t>::max() || source.mWidth == 0 ||
        source.mHeight == 0) {
        error = "missing texture metadata";
        return false;
    }

    CapturedNativeTexture result{};
    auto& texture = result.texture;
    texture.resource = reinterpret_cast<std::uintptr_t>(source.mTexInfo);
    texture.width = source.mWidth;
    texture.height = source.mHeight;
    texture.hasAlpha = source.mAlphaEnabled != 0;
    if (!native_render::decode_address_mode(source.mWrapS, texture.addressU) ||
        !native_render::decode_address_mode(source.mWrapT, texture.addressV) ||
        !native_render::decode_min_filter(source.mMinFilter, texture.minFilter,
                                          texture.mipFilter) ||
        !native_render::decode_mag_filter(source.mMagFilter, texture.magFilter)) {
        error = "unsupported sampler state";
        return false;
    }
    if (texture.mipFilter != native_render::MipFilter::None) {
        error = "mipmapped semantic texture resource is not implemented";
        return false;
    }

    native_render::EncodedImageFormat format{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (!native_render::decode_image_format(static_cast<std::uint8_t>(source.mFormat), format) ||
        !native_render::encoded_image_data_size(source.mWidth, source.mHeight, format,
                                                sourceBytes) ||
        !native_render::decoded_image_data_size(source.mWidth, source.mHeight, outputBytes)) {
        error = "unsupported texture encoding or extent";
        return false;
    }

    native_render::PaletteFormat paletteFormat = native_render::PaletteFormat::Rgb5A3;
    std::uint32_t paletteEntries = 0;
    std::span<const std::uint8_t> palette{};
    if (format == native_render::EncodedImageFormat::Indexed4 ||
        format == native_render::EncodedImageFormat::Indexed8 ||
        format == native_render::EncodedImageFormat::Indexed14) {
        const JUTPalette* activePalette = source.field_0x2c;
        if (activePalette == nullptr || activePalette->getColorTable() == nullptr ||
            !native_render::decode_palette_format(activePalette->getFormat(), paletteFormat)) {
            error = "missing or unsupported active palette";
            return false;
        }
        paletteEntries = activePalette->getNumColors();
        palette = {reinterpret_cast<const std::uint8_t*>(activePalette->getColorTable()),
                   static_cast<std::size_t>(paletteEntries) * 2U};
    }

    const native_render::EncodedImageView encoded{
        format,         source.mWidth,
        source.mHeight, {static_cast<const std::uint8_t*>(source.mTexData), sourceBytes},
        paletteFormat,  paletteEntries,
        palette,
    };
    result.rgba8.resize(outputBytes);
    const native_render::ImageDecodeError decodeError =
        native_render::decode_image_rgba8(encoded, result.rgba8);
    if (decodeError != native_render::ImageDecodeError::None ||
        !native_render::image_content_revision(encoded, texture.revision)) {
        error = native_render::image_decode_error_name(decodeError);
        return false;
    }
    capture = std::move(result);
    return true;
}

} // namespace sb
