#include <sunbright/native_render/jut_texture.h>

#include <limits>
#include <new>
#include <utility>

namespace sb::native_render {

const char* jut_texture_error_name(JutTextureError error) noexcept {
    switch (error) {
    case JutTextureError::None:
        return "none";
    case JutTextureError::EmptyExtent:
        return "empty_extent";
    case JutTextureError::UnsupportedSampler:
        return "unsupported_sampler";
    case JutTextureError::MipmappedNotImplemented:
        return "mipmapped_not_implemented";
    case JutTextureError::UnsupportedFormat:
        return "unsupported_format";
    case JutTextureError::MissingPalette:
        return "missing_palette";
    case JutTextureError::UnsupportedPaletteFormat:
        return "unsupported_palette_format";
    case JutTextureError::EncodedBytesTooShort:
        return "encoded_bytes_too_short";
    case JutTextureError::PaletteBytesTooShort:
        return "palette_bytes_too_short";
    case JutTextureError::AllocationFailure:
        return "allocation_failure";
    case JutTextureError::DecodeFailed:
        return "decode_failed";
    }
    return "unknown";
}

JutTextureError plan_jut_texture(const JutTextureFields& fields, JutTexturePlan& plan) noexcept {
    if (fields.width == 0 || fields.height == 0) {
        return JutTextureError::EmptyExtent;
    }
    if (fields.format > std::numeric_limits<std::uint8_t>::max()) {
        return JutTextureError::UnsupportedFormat;
    }

    JutTexturePlan value{};
    value.texture.width = fields.width;
    value.texture.height = fields.height;
    value.texture.hasAlpha = fields.alphaEnabled;
    if (!decode_address_mode(fields.wrapS, value.texture.addressU) ||
        !decode_address_mode(fields.wrapT, value.texture.addressV) ||
        !decode_min_filter(fields.minFilter, value.texture.minFilter, value.texture.mipFilter) ||
        !decode_mag_filter(fields.magFilter, value.texture.magFilter)) {
        return JutTextureError::UnsupportedSampler;
    }
    // A mip chain would need every level fetched and uploaded, and nothing downstream carries one
    // for a 2D layer yet. Refusing names the gap; sampling level zero under a mip filter would
    // quietly render at the wrong sharpness and look like a decode defect.
    if (value.texture.mipFilter != MipFilter::None) {
        return JutTextureError::MipmappedNotImplemented;
    }

    if (!decode_image_format(static_cast<std::uint8_t>(fields.format), value.format) ||
        !encoded_image_data_size(fields.width, fields.height, value.format, value.encodedBytes) ||
        !decoded_image_data_size(fields.width, fields.height, value.decodedBytes)) {
        return JutTextureError::UnsupportedFormat;
    }

    value.indexed = value.format == EncodedImageFormat::Indexed4 ||
                    value.format == EncodedImageFormat::Indexed8 ||
                    value.format == EncodedImageFormat::Indexed14;
    if (value.indexed) {
        if (!fields.hasPalette) {
            return JutTextureError::MissingPalette;
        }
        if (fields.paletteFormat > std::numeric_limits<std::uint8_t>::max() ||
            !decode_palette_format(static_cast<std::uint8_t>(fields.paletteFormat),
                                   value.paletteFormat)) {
            return JutTextureError::UnsupportedPaletteFormat;
        }
        value.paletteBytes = static_cast<std::size_t>(fields.paletteEntries) * 2U;
    }

    value.texture.hasAlpha = fields.alphaEnabled;
    plan = value;
    return JutTextureError::None;
}

JutTextureError decode_jut_texture(const JutTextureFields& fields,
                                   std::span<const std::uint8_t> encoded,
                                   std::span<const std::uint8_t> palette,
                                   DecodedTexture& decoded) noexcept {
    JutTexturePlan plan{};
    const JutTextureError planned = plan_jut_texture(fields, plan);
    if (planned != JutTextureError::None) {
        return planned;
    }
    if (encoded.size() < plan.encodedBytes) {
        return JutTextureError::EncodedBytesTooShort;
    }
    if (palette.size() < plan.paletteBytes) {
        return JutTextureError::PaletteBytesTooShort;
    }

    const EncodedImageView source{
        plan.format,
        fields.width,
        fields.height,
        encoded.first(plan.encodedBytes),
        plan.paletteFormat,
        plan.indexed ? fields.paletteEntries : 0U,
        plan.indexed ? palette.first(plan.paletteBytes) : std::span<const std::uint8_t>{},
    };

    DecodedTexture result{};
    result.texture = plan.texture;
    try {
        result.rgba8.resize(plan.decodedBytes);
    } catch (const std::bad_alloc&) {
        return JutTextureError::AllocationFailure;
    }
    if (decode_image_rgba8(source, result.rgba8) != ImageDecodeError::None ||
        !image_content_revision(source, result.texture.revision)) {
        return JutTextureError::DecodeFailed;
    }
    decoded = std::move(result);
    return JutTextureError::None;
}

} // namespace sb::native_render
