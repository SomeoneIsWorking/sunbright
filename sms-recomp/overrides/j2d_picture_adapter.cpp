#include "j2d_picture_adapter.h"

#include <sunbright/native_render/image_decode.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace sb::recomp {
namespace {

class Reader {
  public:
    explicit Reader(const GuestByteReader& reader) noexcept : reader_(reader) {}

    bool bytes(std::uint32_t address, void* destination, std::size_t size) const noexcept {
        return reader_.read != nullptr && reader_.read(reader_.context, address, destination, size);
    }

    bool u8(std::uint32_t address, std::uint8_t& value) const noexcept {
        return bytes(address, &value, sizeof(value));
    }

    bool u16(std::uint32_t address, std::uint16_t& value) const noexcept {
        std::array<std::uint8_t, 2> data{};
        if (!bytes(address, data.data(), data.size()))
            return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
        return true;
    }

    bool u32(std::uint32_t address, std::uint32_t& value) const noexcept {
        std::array<std::uint8_t, 4> data{};
        if (!bytes(address, data.data(), data.size()))
            return false;
        value = (static_cast<std::uint32_t>(data[0]) << 24U) |
                (static_cast<std::uint32_t>(data[1]) << 16U) |
                (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
        return true;
    }

    bool s32(std::uint32_t address, std::int32_t& value) const noexcept {
        std::uint32_t bits = 0;
        if (!u32(address, bits))
            return false;
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }

    bool f32(std::uint32_t address, float& value) const noexcept {
        std::uint32_t bits = 0;
        if (!u32(address, bits))
            return false;
        value = std::bit_cast<float>(bits);
        return true;
    }

  private:
    const GuestByteReader& reader_;
};

bool read_matrix(const Reader& reader, std::uint32_t address,
                 native_render::Matrix3x4& matrix) noexcept {
    for (std::size_t index = 0; index < matrix.value.size(); ++index) {
        if (!reader.f32(address + static_cast<std::uint32_t>(index * sizeof(float)),
                        matrix.value[index]))
            return false;
    }
    return true;
}

native_render::Color color(std::uint32_t rgba) noexcept {
    constexpr float kScale = 1.0f / 255.0f;
    return {static_cast<float>((rgba >> 24U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 16U) & 0xffU) * kScale,
            static_cast<float>((rgba >> 8U) & 0xffU) * kScale,
            static_cast<float>(rgba & 0xffU) * kScale};
}

bool read_texture(const Reader& reader, std::uint32_t textureAddress, std::size_t textureIndex,
                  std::uint32_t colorBlend, std::uint32_t alphaBlend,
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

} // namespace

J2DContextCaptureResult capture_j2d_context(const GuestByteReader& byteReader, std::uint32_t screen,
                                            std::uint32_t grafContext,
                                            native_render::PictureContext& context) noexcept {
    if (screen == 0)
        return J2DContextCaptureResult::Invalid;
    const Reader reader(byteReader);
    std::uint8_t clipEnabled = 0;
    if (!reader.u8(screen + 0xec, clipEnabled))
        return J2DContextCaptureResult::Invalid;

    native_render::PictureContext result{};
    result.clipEnabled = clipEnabled != 0;
    if (grafContext == 0) {
        result.canvas = {.origin = {0, 0}, .extent = {640, 480}, .viewport = {0, 0, 640, 480}};
    } else {
        std::uint32_t type = 0;
        if (!reader.u32(grafContext + 0x04, type))
            return J2DContextCaptureResult::Invalid;
        if (type != 1)
            return J2DContextCaptureResult::NonOrthographic;

        std::int32_t viewportX1 = 0;
        std::int32_t viewportY1 = 0;
        std::int32_t viewportX2 = 0;
        std::int32_t viewportY2 = 0;
        std::int32_t logicalX1 = 0;
        std::int32_t logicalY1 = 0;
        std::int32_t logicalX2 = 0;
        std::int32_t logicalY2 = 0;
        if (!reader.s32(grafContext + 0x08, viewportX1) ||
            !reader.s32(grafContext + 0x0c, viewportY1) ||
            !reader.s32(grafContext + 0x10, viewportX2) ||
            !reader.s32(grafContext + 0x14, viewportY2) ||
            !reader.s32(grafContext + 0xd8, logicalX1) ||
            !reader.s32(grafContext + 0xdc, logicalY1) ||
            !reader.s32(grafContext + 0xe0, logicalX2) ||
            !reader.s32(grafContext + 0xe4, logicalY2) || viewportX2 <= viewportX1 ||
            viewportY2 <= viewportY1 || logicalX2 <= logicalX1 || logicalY2 <= logicalY1) {
            return J2DContextCaptureResult::Invalid;
        }
        result.canvas = {.origin = {static_cast<float>(logicalX1), static_cast<float>(logicalY1)},
                         .extent = {static_cast<float>(logicalX2 - logicalX1),
                                    static_cast<float>(logicalY2 - logicalY1)},
                         .viewport = {viewportX1, viewportY1,
                                      static_cast<std::uint32_t>(viewportX2 - viewportX1),
                                      static_cast<std::uint32_t>(viewportY2 - viewportY1)}};
    }
    if (!native_render::valid(result.canvas))
        return J2DContextCaptureResult::Invalid;
    context = result;
    return J2DContextCaptureResult::Success;
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
    const Reader reader(byteReader);

    CapturedPicture result{};
    result.command.instance = self;

    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
    std::uint8_t textureCount = 0;
    std::uint8_t transpose = 0;
    std::uint8_t opacity = 0;
    std::uint32_t binding = 0;
    std::uint32_t mirror = 0;
    std::int32_t horizontalWrap = 0;
    std::int32_t verticalWrap = 0;
    std::uint32_t white = 0;
    std::uint32_t black = 0;
    std::uint32_t colorBlend = 0;
    std::uint32_t alphaBlend = 0;
    if (!reader.s32(self + 0x14, x1) || !reader.s32(self + 0x18, y1) ||
        !reader.s32(self + 0x1c, x2) || !reader.s32(self + 0x20, y2) ||
        !reader.u8(self + 0xfc, textureCount) || textureCount == 0 || textureCount > 4 ||
        !reader.u8(self + 0x130, transpose) || !reader.u8(self + 0xcd, opacity) ||
        !reader.u32(self + 0x128, binding) || !reader.u32(self + 0x12c, mirror) ||
        !reader.s32(self + 0x134, horizontalWrap) || !reader.s32(self + 0x138, verticalWrap) ||
        !reader.u32(self + 0x13c, white) || !reader.u32(self + 0x140, black) ||
        !reader.u32(self + 0x154, colorBlend) || !reader.u32(self + 0x158, alphaBlend))
        return false;

    result.command.opacity = static_cast<float>(opacity) / 255.0f;
    result.command.material.textureCount = textureCount;
    result.command.material.black = color(black);
    result.command.material.white = color(white);
    for (std::size_t index = 0; index < result.command.corner.size(); ++index) {
        std::uint32_t rgba = 0;
        if (!reader.u32(self + 0x144 + static_cast<std::uint32_t>(index * 4), rgba))
            return false;
        result.command.corner[index] = color(rgba);
    }
    for (std::size_t index = 0; index < textureCount; ++index) {
        std::uint32_t textureAddress = 0;
        if (!reader.u32(self + 0xec + static_cast<std::uint32_t>(index * 4), textureAddress) ||
            !read_texture(reader, textureAddress, index, colorBlend, alphaBlend,
                          result.command.material.textures[index], result.rgba8[index]))
            return false;
    }
    result.imageCount = textureCount;

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

} // namespace sb::recomp
