#include <sunbright/native_render/semantic_sink.h>
#include <sunbright/native_render/window.h>

#include "host_allocation_scope.h"
#include "native_j2d_context.h"
#include "native_jut_texture_adapter.h"

#include <JSystem/J2D/J2DWindow.hpp>
#include <dolphin/os.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

constexpr std::size_t kContentsTexture = 0;
constexpr std::size_t kTopLeftTexture = 1;

std::size_t texture_index(sb::native_render::WindowTextureRole role) {
    switch (role) {
    case sb::native_render::WindowTextureRole::Contents:
        return kContentsTexture;
    case sb::native_render::WindowTextureRole::TopLeft:
        return kTopLeftTexture;
    case sb::native_render::WindowTextureRole::TopRight:
        return 2;
    case sb::native_render::WindowTextureRole::BottomLeft:
        return 3;
    case sb::native_render::WindowTextureRole::BottomRight:
        return 4;
    }
    return 5;
}

sb::native_render::Color window_color(std::uint32_t rgba, std::uint8_t opacity) {
    sb::native_render::Color result = sb::native_render::color_from_rgba8(rgba);
    result.a = static_cast<float>(((rgba & 0xffU) * opacity) / 0xffU) / 255.0f;
    return result;
}

} // namespace

extern "C" void sb_native_window_submit(const void* windowPointer, const void* outerPointer,
                                        const void* contentsPointer,
                                        const void* parentMatrixPointer) {
    if (!sb::native_render::has_semantic_sink())
        return;
    const auto fail = [&](const char* reason) {
        OSPanic(__FILE__, __LINE__,
                "semantic J2DWindow capture failed: %s window=%p outer=%p contents=%p matrix=%p",
                reason, windowPointer, outerPointer, contentsPointer, parentMatrixPointer);
    };
    if (windowPointer == nullptr || outerPointer == nullptr || contentsPointer == nullptr ||
        parentMatrixPointer == nullptr) {
        fail("null input");
        return;
    }
    const sb::native_render::PictureContext* context = sb::current_native_j2d_context();
    if (context == nullptr) {
        fail("missing or non-orthographic J2D screen context");
        return;
    }

    const sb::HostAllocationScope hostAllocations;
    const auto& window = *static_cast<const J2DWindow*>(windowPointer);
    const auto& outer = *static_cast<const JUTRect*>(outerPointer);
    const auto& contents = *static_cast<const JUTRect*>(contentsPointer);
    const auto& parentMatrix = *static_cast<const Mtx*>(parentMatrixPointer);

    const std::array<const J2DWindow::Texture*, 4> frameTextures{
        window.getFrameTextureTopLeft(), window.getFrameTextureTopRight(),
        window.getFrameTextureBottomLeft(), window.getFrameTextureBottomRight()};
    sb::native_render::WindowLayout layout{};
    layout.outerWidth = outer.getWidth();
    layout.outerHeight = outer.getHeight();
    layout.minimumWidth = window.getMinimumWidth();
    layout.minimumHeight = window.getMinimumHeight();
    layout.contentsX1 = contents.x1;
    layout.contentsY1 = contents.y1;
    layout.contentsX2 = contents.x2;
    layout.contentsY2 = contents.y2;
    layout.mirror = window.getMirrorFlags();
    layout.hasFrame = std::ranges::all_of(
        frameTextures, [](const J2DWindow::Texture* texture) { return texture != nullptr; });
    layout.hasContentsTexture = window.getContentsTexture() != nullptr;
    std::copy_n(&parentMatrix[0][0], 12, layout.parentTransform.value.begin());
    std::copy_n(&window.mGlobalMtx[0][0], 12, layout.globalTransform.value.begin());
    if (sb::native_render::window_size_is_culled(layout))
        return;

    std::array<sb::CapturedNativeTexture, 5> capturedTextures{};
    const char* textureError = "unknown texture error";
    if (layout.hasFrame) {
        for (std::size_t index = 0; index < frameTextures.size(); ++index) {
            const std::size_t captureIndex = index + kTopLeftTexture;
            if (!sb::capture_native_jut_texture(*frameTextures[index],
                                                capturedTextures[captureIndex], textureError)) {
                fail(textureError);
                return;
            }
            layout.frameTextures[index] = {capturedTextures[captureIndex].texture.width,
                                           capturedTextures[captureIndex].texture.height};
        }
    }
    if (layout.hasContentsTexture) {
        if (!sb::capture_native_jut_texture(*window.getContentsTexture(),
                                            capturedTextures[kContentsTexture], textureError)) {
            fail(textureError);
            return;
        }
        layout.contentsTexture = {capturedTextures[kContentsTexture].texture.width,
                                  capturedTextures[kContentsTexture].texture.height};
    }

    sb::native_render::WindowGeometry geometry{};
    const sb::native_render::WindowLayoutResult layoutResult =
        sb::native_render::resolve_window_layout(layout, geometry);
    if (layoutResult == sb::native_render::WindowLayoutResult::Culled)
        return;
    if (layoutResult != sb::native_render::WindowLayoutResult::Visible) {
        fail("layout resolver rejected window state");
        return;
    }

    sb::native_render::ClipRect clip{};
    if (context->clipEnabled) {
        if (window.mClipRect.isEmpty()) {
            fail("empty active clip rectangle");
            return;
        }
        clip = {.enabled = true,
                .x = window.mClipRect.x1,
                .y = window.mClipRect.y1,
                .width = static_cast<std::uint32_t>(window.mClipRect.getWidth()),
                .height = static_cast<std::uint32_t>(window.mClipRect.getHeight())};
    }

    const std::uintptr_t instance = reinterpret_cast<std::uintptr_t>(&window);
    if (geometry.contentsVisible) {
        const std::array<sb::native_render::Color, 4> colors{
            window_color(static_cast<u32>(window.getContentsColorTopLeft()), window.mColorAlpha),
            window_color(static_cast<u32>(window.getContentsColorTopRight()), window.mColorAlpha),
            window_color(static_cast<u32>(window.getContentsColorBottomLeft()), window.mColorAlpha),
            window_color(static_cast<u32>(window.getContentsColorBottomRight()),
                         window.mColorAlpha)};
        sb::native_render::SolidRectangleCommand command{};
        if (!sb::native_render::make_window_contents_command(geometry, instance, colors, command)) {
            fail("contents command assembly failed");
            return;
        }
        command.clip = clip;
        if (!sb::native_render::submit_solid_rectangle({context->canvas, command})) {
            fail("sink rejected contents fill");
            return;
        }
    }

    std::uint64_t partInstance = instance;
    const auto submit_part = [&](const sb::native_render::WindowTexturedPart& part,
                                 sb::native_render::Color black, sb::native_render::Color white) {
        if (!part.visible)
            return true;
        const std::size_t index = texture_index(part.texture);
        sb::native_render::PictureCommand command{};
        if (!sb::native_render::make_window_picture_command(
                part, ++partInstance, capturedTextures[index].texture,
                static_cast<float>(window.mColorAlpha) / 255.0f, black, white, command)) {
            return false;
        }
        command.clip = clip;
        const sb::native_render::DecodedImageView image = capturedTextures[index].image_view();
        return sb::native_render::submit_picture({context->canvas, command}, std::span(&image, 1));
    };
    if (geometry.contentsTexture.visible &&
        !submit_part(geometry.contentsTexture, {}, {1, 1, 1, 1})) {
        fail("sink rejected contents texture");
        return;
    }
    const sb::native_render::Color black =
        sb::native_render::color_from_rgba8(static_cast<u32>(window.getFrameBlack()));
    const sb::native_render::Color white =
        sb::native_render::color_from_rgba8(static_cast<u32>(window.getFrameWhite()));
    for (const sb::native_render::WindowTexturedPart& part : geometry.frame) {
        if (!submit_part(part, black, white)) {
            fail("sink rejected frame texture");
            return;
        }
    }
}
