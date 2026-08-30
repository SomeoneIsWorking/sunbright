#include "j2d_window_adapter.h"

#include <sunbright/native_render/window.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace sb::recomp {
namespace {

constexpr std::uint8_t kContentsTexture = 0;
constexpr std::uint8_t kTopLeftTexture = 1;
constexpr std::uint8_t kTopRightTexture = 2;
constexpr std::uint8_t kBottomLeftTexture = 3;
constexpr std::uint8_t kBottomRightTexture = 4;

bool read_matrix(const BigEndianGuestReader& reader, std::uint32_t address,
                 native_render::Matrix3x4& matrix) noexcept {
    for (std::size_t index = 0; index < matrix.value.size(); ++index) {
        if (!reader.f32(address + static_cast<std::uint32_t>(index * sizeof(float)),
                        matrix.value[index]))
            return false;
    }
    return true;
}

bool read_rect(const BigEndianGuestReader& reader, std::uint32_t address, std::int32_t& x1,
               std::int32_t& y1, std::int32_t& x2, std::int32_t& y2) noexcept {
    return reader.s32(address, x1) && reader.s32(address + 4, y1) && reader.s32(address + 8, x2) &&
           reader.s32(address + 12, y2);
}

std::uint8_t texture_index(native_render::WindowTextureRole role) noexcept {
    switch (role) {
    case native_render::WindowTextureRole::Contents:
        return kContentsTexture;
    case native_render::WindowTextureRole::TopLeft:
        return kTopLeftTexture;
    case native_render::WindowTextureRole::TopRight:
        return kTopRightTexture;
    case native_render::WindowTextureRole::BottomLeft:
        return kBottomLeftTexture;
    case native_render::WindowTextureRole::BottomRight:
        return kBottomRightTexture;
    }
    return 0xff;
}

native_render::Color window_color(std::uint32_t rgba, std::uint8_t opacity) noexcept {
    native_render::Color result = native_render::color_from_rgba8(rgba);
    const std::uint32_t alpha = rgba & 0xffU;
    result.a = static_cast<float>((alpha * opacity) / 0xffU) / 255.0f;
    return result;
}

bool append_picture(CapturedWindow& capture, std::uint32_t self,
                    const native_render::WindowTexturedPart& part, float opacity,
                    native_render::Color black, native_render::Color white) noexcept {
    if (!part.visible)
        return true;
    const std::uint8_t index = texture_index(part.texture);
    auto& output = capture.pictures[capture.pictureCount++];
    output.textureIndex = index;
    return native_render::make_window_picture_command(
        part, (static_cast<std::uint64_t>(self) << 8U) | capture.pictureCount,
        capture.textures[index].texture, opacity, black, white, output.command);
}

} // namespace

native_render::DecodedImageView
CapturedWindow::image_for(const CapturedWindowPicture& picture) const noexcept {
    const CapturedGuestTexture& source = textures[picture.textureIndex];
    return {source.texture.resource, source.texture.revision, source.texture.width,
            source.texture.height, source.rgba8};
}

WindowCaptureResult capture_j2d_window(const GuestByteReader& byteReader, std::uint32_t self,
                                       std::uint32_t outerRect, std::uint32_t contentsRect,
                                       std::uint32_t parentMatrix,
                                       CapturedWindow& capture) noexcept {
    if (self == 0 || outerRect == 0 || contentsRect == 0 || parentMatrix == 0)
        return WindowCaptureResult::Invalid;
    const BigEndianGuestReader reader(byteReader);
    CapturedWindow result{};
    native_render::WindowLayout layout{};

    std::int32_t outerX1 = 0;
    std::int32_t outerY1 = 0;
    std::int32_t outerX2 = 0;
    std::int32_t outerY2 = 0;
    if (!read_rect(reader, outerRect, outerX1, outerY1, outerX2, outerY2) ||
        !read_rect(reader, contentsRect, layout.contentsX1, layout.contentsY1, layout.contentsX2,
                   layout.contentsY2) ||
        !reader.u32(self + 0x114, layout.mirror) ||
        !reader.s32(self + 0x130, layout.minimumWidth) ||
        !reader.s32(self + 0x134, layout.minimumHeight) ||
        !read_matrix(reader, parentMatrix, layout.parentTransform) ||
        !read_matrix(reader, self + 0x84, layout.globalTransform)) {
        return WindowCaptureResult::Invalid;
    }
    layout.outerWidth = outerX2 - outerX1;
    layout.outerHeight = outerY2 - outerY1;
    if (native_render::window_size_is_culled(layout))
        return WindowCaptureResult::Culled;

    std::array<std::uint32_t, 4> frameAddresses{};
    for (std::size_t index = 0; index < frameAddresses.size(); ++index) {
        if (!reader.u32(self + 0x100 + static_cast<std::uint32_t>(index * 4),
                        frameAddresses[index]))
            return WindowCaptureResult::Invalid;
    }
    layout.hasFrame =
        std::ranges::all_of(frameAddresses, [](std::uint32_t address) { return address != 0; });
    if (layout.hasFrame) {
        for (std::size_t index = 0; index < frameAddresses.size(); ++index) {
            const std::size_t captureIndex = index + kTopLeftTexture;
            if (!capture_guest_jut_texture(reader, frameAddresses[index],
                                           result.textures[captureIndex]))
                return WindowCaptureResult::Invalid;
            layout.frameTextures[index] = {result.textures[captureIndex].texture.width,
                                           result.textures[captureIndex].texture.height};
        }
    }

    std::uint32_t contentsTextureAddress = 0;
    if (!reader.u32(self + 0x110, contentsTextureAddress))
        return WindowCaptureResult::Invalid;
    layout.hasContentsTexture = contentsTextureAddress != 0;
    if (layout.hasContentsTexture) {
        if (!capture_guest_jut_texture(reader, contentsTextureAddress,
                                       result.textures[kContentsTexture]))
            return WindowCaptureResult::Invalid;
        layout.contentsTexture = {result.textures[kContentsTexture].texture.width,
                                  result.textures[kContentsTexture].texture.height};
    }

    native_render::WindowGeometry geometry{};
    const native_render::WindowLayoutResult layoutResult =
        native_render::resolve_window_layout(layout, geometry);
    if (layoutResult == native_render::WindowLayoutResult::Culled)
        return WindowCaptureResult::Culled;
    if (layoutResult == native_render::WindowLayoutResult::Invalid)
        return WindowCaptureResult::Invalid;

    std::uint8_t opacity = 0;
    std::array<std::uint32_t, 4> contentColors{};
    std::uint32_t whiteRgba = 0;
    std::uint32_t blackRgba = 0;
    if (!reader.u8(self + 0xcd, opacity) || !reader.u32(self + 0x118, contentColors[0]) ||
        !reader.u32(self + 0x11c, contentColors[1]) ||
        !reader.u32(self + 0x120, contentColors[2]) ||
        !reader.u32(self + 0x124, contentColors[3]) || !reader.u32(self + 0x128, whiteRgba) ||
        !reader.u32(self + 0x12c, blackRgba)) {
        return WindowCaptureResult::Invalid;
    }

    if (geometry.contentsVisible) {
        std::array<native_render::Color, 4> colors{};
        for (std::size_t index = 0; index < contentColors.size(); ++index)
            colors[index] = window_color(contentColors[index], opacity);
        if (!native_render::make_window_contents_command(
                geometry, (static_cast<std::uint64_t>(self) << 8U) | 0xfeU, colors,
                result.contents))
            return WindowCaptureResult::Invalid;
        result.hasContents = true;
    }

    const float normalizedOpacity = static_cast<float>(opacity) / 255.0f;
    if (geometry.contentsTexture.visible) {
        if (!append_picture(result, self, geometry.contentsTexture, normalizedOpacity,
                            native_render::Color{}, native_render::Color{1, 1, 1, 1}))
            return WindowCaptureResult::Invalid;
    }
    const native_render::Color black = native_render::color_from_rgba8(blackRgba);
    const native_render::Color white = native_render::color_from_rgba8(whiteRgba);
    for (const native_render::WindowTexturedPart& part : geometry.frame) {
        if (!append_picture(result, self, part, normalizedOpacity, black, white))
            return WindowCaptureResult::Invalid;
    }

    capture = std::move(result);
    return WindowCaptureResult::Visible;
}

} // namespace sb::recomp
