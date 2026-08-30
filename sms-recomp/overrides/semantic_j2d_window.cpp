#include "semantic_j2d_window.h"

#include "j2d_window_adapter.h"
#include "semantic_j2d_context.h"

#include "../runtime/sb_assert.h"

#include <sunbright/native_render/semantic_sink.h>

#include <intrinsics.h>

#include <array>
#include <cstdint>
#include <span>

namespace {

sb::native_render::ClipRect active_window_clip(std::uint32_t self) {
    const std::int32_t x1 = static_cast<std::int32_t>(sb_r32(self + 0x34));
    const std::int32_t y1 = static_cast<std::int32_t>(sb_r32(self + 0x38));
    const std::int32_t x2 = static_cast<std::int32_t>(sb_r32(self + 0x3c));
    const std::int32_t y2 = static_cast<std::int32_t>(sb_r32(self + 0x40));
    SB_ASSERT(x2 > x1 && y2 > y1,
              "semantic J2DWindow has empty active clip: self=%08x clip=%d,%d..%d,%d", self, x1, y1,
              x2, y2);
    return {.enabled = true,
            .x = x1,
            .y = y1,
            .width = static_cast<std::uint32_t>(x2 - x1),
            .height = static_cast<std::uint32_t>(y2 - y1)};
}

} // namespace

void submit_semantic_j2d_window(CPUState& cpu) {
    if (!sb::native_render::has_semantic_sink())
        return;

    sb::recomp::CapturedWindow capture{};
    const sb::recomp::WindowCaptureResult result =
        sb::recomp::capture_j2d_window(sb::recomp::live_guest_byte_reader(), cpu.gpr[3], cpu.gpr[4],
                                       cpu.gpr[5], cpu.gpr[6], capture);
    if (result == sb::recomp::WindowCaptureResult::Culled)
        return;
    SB_ASSERT(result == sb::recomp::WindowCaptureResult::Visible,
              "semantic J2DWindow capture failed: self=%08x outer=%08x contents=%08x "
              "parent_matrix=%08x",
              cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);

    const sb::native_render::PictureContext* context = sb::recomp::current_semantic_j2d_context();
    SB_ASSERT(context != nullptr,
              "semantic J2DWindow has no enclosing orthographic J2D screen: self=%08x", cpu.gpr[3]);
    const sb::native_render::ClipRect clip =
        context->clipEnabled ? active_window_clip(cpu.gpr[3]) : sb::native_render::ClipRect{};

    if (capture.hasContents) {
        capture.contents.clip = clip;
        const sb::native_render::SolidRectangleDraw draw{context->canvas, capture.contents};
        SB_ASSERT(sb::native_render::submit_solid_rectangle(draw),
                  "semantic J2DWindow sink rejected contents fill: self=%08x", cpu.gpr[3]);
    }
    for (sb::recomp::CapturedWindowPicture& picture :
         std::span(capture.pictures).first(capture.pictureCount)) {
        picture.command.clip = clip;
        const sb::native_render::DecodedImageView image = capture.image_for(picture);
        const sb::native_render::PictureDraw draw{context->canvas, picture.command};
        SB_ASSERT(sb::native_render::submit_picture(draw, std::span(&image, 1)),
                  "semantic J2DWindow sink rejected textured part: self=%08x texture=%u",
                  cpu.gpr[3], static_cast<unsigned>(picture.textureIndex));
    }
}
