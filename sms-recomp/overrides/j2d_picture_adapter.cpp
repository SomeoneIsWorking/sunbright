#include "j2d_picture_adapter.h"

#include <sunbright/native_render/image_decode.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace sb::recomp {
namespace {

bool read_matrix(const BigEndianGuestReader& reader, std::uint32_t address,
                 native_render::Matrix3x4& matrix) noexcept {
    for (std::size_t index = 0; index < matrix.value.size(); ++index) {
        if (!reader.f32(address + static_cast<std::uint32_t>(index * sizeof(float)),
                        matrix.value[index]))
            return false;
    }
    return true;
}

bool read_texture(const BigEndianGuestReader& reader, std::uint32_t textureAddress,
                  std::size_t textureIndex, std::uint32_t colorBlend, std::uint32_t alphaBlend,
                  native_render::PictureTexture& texture,
                  std::vector<std::uint8_t>& rgba8) noexcept {
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
        !reader.u8(textureAddress + 0x43, mag) || width == 0 || height == 0)
        return false;

    texture.resource = resource;
    texture.width = width;
    texture.height = height;
    texture.hasAlpha = hasAlpha != 0;
    if (!native_render::decode_address_mode(wrapU, texture.addressU) ||
        !native_render::decode_address_mode(wrapV, texture.addressV) ||
        !native_render::decode_min_filter(min, texture.minFilter, texture.mipFilter) ||
        !native_render::decode_mag_filter(mag, texture.magFilter))
        return false;
    // The current semantic GPU resource has one decoded level. Refuse authored mip sampling rather
    // than silently advertising a chain that was not captured.
    if (texture.mipFilter != native_render::MipFilter::None)
        return false;
    if (textureIndex != 0 &&
        (!native_render::decode_blend_factor(colorBlend, textureIndex, texture.colorMix) ||
         !native_render::decode_blend_factor(alphaBlend, textureIndex, texture.alphaMix)))
        return false;

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
    rgba8.resize(outputBytes);
    if (native_render::decode_image_rgba8(source, rgba8) != native_render::ImageDecodeError::None) {
        return false;
    }
    if (!native_render::image_content_revision(source, texture.revision))
        return false;
    return true;
}

bool read_picture_material(const BigEndianGuestReader& reader, std::uint32_t self,
                           CapturedPicture& result) noexcept {
    std::uint8_t textureCount = 0;
    std::uint8_t opacity = 0;
    std::uint32_t white = 0;
    std::uint32_t black = 0;
    std::uint32_t colorBlend = 0;
    std::uint32_t alphaBlend = 0;
    if (!reader.u8(self + 0xfc, textureCount) || textureCount == 0 || textureCount > 4 ||
        !reader.u8(self + 0xcd, opacity) || !reader.u32(self + 0x13c, white) ||
        !reader.u32(self + 0x140, black) || !reader.u32(self + 0x154, colorBlend) ||
        !reader.u32(self + 0x158, alphaBlend)) {
        return false;
    }

    result.command.instance = self;
    result.command.opacity = static_cast<float>(opacity) / 255.0f;
    result.command.material.textureCount = textureCount;
    result.command.material.black = native_render::color_from_rgba8(black);
    result.command.material.white = native_render::color_from_rgba8(white);
    for (std::size_t index = 0; index < result.command.corner.size(); ++index) {
        std::uint32_t rgba = 0;
        if (!reader.u32(self + 0x144 + static_cast<std::uint32_t>(index * 4), rgba))
            return false;
        result.command.corner[index] = native_render::color_from_rgba8(rgba);
    }
    for (std::size_t index = 0; index < textureCount; ++index) {
        std::uint32_t textureAddress = 0;
        if (!reader.u32(self + 0xec + static_cast<std::uint32_t>(index * 4), textureAddress) ||
            !read_texture(reader, textureAddress, index, colorBlend, alphaBlend,
                          result.command.material.textures[index], result.rgba8[index])) {
            return false;
        }
    }
    result.imageCount = textureCount;
    return true;
}

J2DContextCaptureResult capture_graph_context(const BigEndianGuestReader& reader,
                                              std::uint32_t grafContext, bool clipEnabled,
                                              native_render::PictureContext& context) noexcept {
    native_render::PictureContext result{};
    result.clipEnabled = clipEnabled;
    if (grafContext == 0) {
        result.canvas = {.origin = {0, 0}, .extent = {640, 480}, .viewport = {0, 0, 640, 480}};
    } else {
        // GMSE01's J2DOrthoGraph constructor installs this vtable and writes type 1. The base
        // J2DGrafContext constructors leave the type word untouched, so requiring both prevents an
        // indeterminate base-context word from being mistaken for the larger ortho layout.
        constexpr std::uint32_t kJ2DOrthoGraphVtable = 0x803e14b0;
        std::uint32_t vtable = 0;
        std::uint32_t type = 0;
        if (!reader.u32(grafContext, vtable) || !reader.u32(grafContext + 0x04, type))
            return J2DContextCaptureResult::Invalid;
        if (vtable != kJ2DOrthoGraphVtable || type != 1)
            return J2DContextCaptureResult::NonOrthographic;

        std::int32_t viewportX1 = 0;
        std::int32_t viewportY1 = 0;
        std::int32_t viewportX2 = 0;
        std::int32_t viewportY2 = 0;
        std::int32_t logicalX1 = 0;
        std::int32_t logicalY1 = 0;
        std::int32_t logicalX2 = 0;
        std::int32_t logicalY2 = 0;
        std::int32_t scissorX1 = 0;
        std::int32_t scissorY1 = 0;
        std::int32_t scissorX2 = 0;
        std::int32_t scissorY2 = 0;
        if (!reader.s32(grafContext + 0x08, viewportX1) ||
            !reader.s32(grafContext + 0x0c, viewportY1) ||
            !reader.s32(grafContext + 0x10, viewportX2) ||
            !reader.s32(grafContext + 0x14, viewportY2) ||
            !reader.s32(grafContext + 0xd8, logicalX1) ||
            !reader.s32(grafContext + 0xdc, logicalY1) ||
            !reader.s32(grafContext + 0xe0, logicalX2) ||
            !reader.s32(grafContext + 0xe4, logicalY2) ||
            !reader.s32(grafContext + 0x18, scissorX1) ||
            !reader.s32(grafContext + 0x1c, scissorY1) ||
            !reader.s32(grafContext + 0x20, scissorX2) ||
            !reader.s32(grafContext + 0x24, scissorY2) || viewportX2 <= viewportX1 ||
            viewportY2 <= viewportY1 || logicalX2 <= logicalX1 || logicalY2 <= logicalY1) {
            return J2DContextCaptureResult::Invalid;
        }
        result.canvas = {.origin = {static_cast<float>(logicalX1), static_cast<float>(logicalY1)},
                         .extent = {static_cast<float>(logicalX2 - logicalX1),
                                    static_cast<float>(logicalY2 - logicalY1)},
                         .viewport = {viewportX1, viewportY1,
                                      static_cast<std::uint32_t>(viewportX2 - viewportX1),
                                      static_cast<std::uint32_t>(viewportY2 - viewportY1)}};
        result.scissor =
            native_render::j2d_target_scissor(scissorX1, scissorY1, scissorX2, scissorY2);
    }
    if (!native_render::valid(result.canvas))
        return J2DContextCaptureResult::Invalid;
    context = result;
    return J2DContextCaptureResult::Success;
}

} // namespace

J2DContextCaptureResult capture_j2d_context(const GuestByteReader& byteReader, std::uint32_t screen,
                                            std::uint32_t grafContext,
                                            native_render::PictureContext& context) noexcept {
    if (screen == 0)
        return J2DContextCaptureResult::Invalid;
    const BigEndianGuestReader reader(byteReader);
    std::uint8_t clipEnabled = 0;
    if (!reader.u8(screen + 0xec, clipEnabled))
        return J2DContextCaptureResult::Invalid;

    return capture_graph_context(reader, grafContext, clipEnabled != 0, context);
}

J2DContextCaptureResult capture_j2d_graph_context(const GuestByteReader& byteReader,
                                                  std::uint32_t grafContext,
                                                  native_render::PictureContext& context) noexcept {
    if (grafContext == 0)
        return J2DContextCaptureResult::Invalid;
    return capture_graph_context(BigEndianGuestReader(byteReader), grafContext, false, context);
}

void CapturedPicture::refresh_image_views() noexcept {
    for (std::size_t index = 0; index < imageCount; ++index) {
        const native_render::PictureTexture& texture = command.material.textures[index];
        images[index] = {texture.resource, texture.revision, texture.width, texture.height,
                         rgba8[index]};
    }
}

std::span<const native_render::DecodedImageView> CapturedPicture::image_views() const noexcept {
    return std::span(images).first(imageCount);
}

bool capture_j2d_picture(const GuestByteReader& byteReader, std::uint32_t self,
                         std::uint32_t parentMatrix, CapturedPicture& capture) noexcept {
    if (self == 0 || parentMatrix == 0)
        return false;
    const BigEndianGuestReader reader(byteReader);

    CapturedPicture result{};

    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
    std::uint8_t transpose = 0;
    std::uint32_t binding = 0;
    std::uint32_t mirror = 0;
    std::int32_t horizontalWrap = 0;
    std::int32_t verticalWrap = 0;
    if (!reader.s32(self + 0x14, x1) || !reader.s32(self + 0x18, y1) ||
        !reader.s32(self + 0x1c, x2) || !reader.s32(self + 0x20, y2) ||
        !reader.u8(self + 0x130, transpose) || !reader.u32(self + 0x128, binding) ||
        !reader.u32(self + 0x12c, mirror) || !reader.s32(self + 0x134, horizontalWrap) ||
        !reader.s32(self + 0x138, verticalWrap) || !read_picture_material(reader, self, result))
        return false;

    native_render::PictureLayout layout{};
    layout.width = x2 - x1;
    layout.height = y2 - y1;
    layout.textureWidth = result.command.material.textures[0].width;
    layout.textureHeight = result.command.material.textures[0].height;
    layout.binding = binding;
    layout.mirror = mirror;
    layout.transpose = transpose != 0;
    layout.horizontalWrap = horizontalWrap;
    layout.verticalWrap = verticalWrap;
    if (!read_matrix(reader, parentMatrix, layout.parentTransform) ||
        !read_matrix(reader, self + 0x84, layout.globalTransform) ||
        !native_render::resolve_picture_layout(layout, result.command.positions,
                                               result.command.uv) ||
        !native_render::valid(result.command))
        return false;

    capture = std::move(result);
    capture.refresh_image_views();
    return true;
}

bool capture_j2d_direct_picture(const GuestByteReader& byteReader, std::uint32_t self,
                                std::uint32_t positionMatrix, std::int32_t width,
                                std::int32_t height, bool mirrorHorizontal, bool mirrorVertical,
                                bool transpose, CapturedPicture& capture) noexcept {
    if (self == 0 || positionMatrix == 0)
        return false;
    const BigEndianGuestReader reader(byteReader);
    CapturedPicture result{};
    if (!read_picture_material(reader, self, result))
        return false;

    native_render::DirectPictureLayout layout{};
    layout.width = width;
    layout.height = height;
    layout.mirrorHorizontal = mirrorHorizontal;
    layout.mirrorVertical = mirrorVertical;
    layout.transpose = transpose;
    if (!read_matrix(reader, positionMatrix, layout.transform) ||
        !native_render::resolve_direct_picture_layout(layout, result.command.positions,
                                                      result.command.uv) ||
        !native_render::valid(result.command)) {
        return false;
    }

    capture = std::move(result);
    capture.refresh_image_views();
    return true;
}

} // namespace sb::recomp
