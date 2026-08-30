#include <sunbright/native_render/semantic_sink.h>
#include <sunbright/native_render/solid_rectangle.h>

#include "host_allocation_scope.h"
#include "native_j2d_context.h"

#include <JSystem/J2D/J2DGrafContext.hpp>
#include <JSystem/JUtility/JUTRect.hpp>
#include <dolphin/os.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" void sb_native_j2d_fill_box_submit(const void* contextPointer, const void* rectPointer) {
    if (!sb::native_render::has_semantic_sink())
        return;
    if (contextPointer == nullptr || rectPointer == nullptr) {
        OSPanic(__FILE__, __LINE__,
                "semantic J2DGrafContext::fillBox capture received null input: context=%p rect=%p",
                contextPointer, rectPointer);
        return;
    }
    const sb::native_render::PictureContext* pictureContext = sb::current_native_j2d_context();
    if (pictureContext == nullptr) {
        OSPanic(__FILE__, __LINE__,
                "semantic J2DGrafContext::fillBox has no active orthographic context: context=%p "
                "rect=%p",
                contextPointer, rectPointer);
        return;
    }

    const sb::HostAllocationScope hostAllocations;
    const auto& context = *static_cast<const J2DGrafContext*>(contextPointer);
    const auto& rect = *static_cast<const JUTRect*>(rectPointer);
    sb::native_render::TransformedS16RectangleLayout layout{rect.x1, rect.y1, rect.x2, rect.y2, {}};
    std::copy_n(&context.mPosMtx[0][0], layout.transform.value.size(),
                layout.transform.value.begin());

    sb::native_render::SolidRectangleDraw draw{};
    draw.canvas = pictureContext->canvas;
    draw.rectangle.instance = reinterpret_cast<std::uintptr_t>(contextPointer);
    draw.rectangle.source = sb::native_render::SolidRectangleSource::J2dGrafContextFillBox;
    draw.rectangle.clip = pictureContext->scissor;
    draw.rectangle.corner = {
        sb::native_render::color_from_rgba8(static_cast<u32>(context.mColorTL)),
        sb::native_render::color_from_rgba8(static_cast<u32>(context.mColorTR)),
        sb::native_render::color_from_rgba8(static_cast<u32>(context.mColorBR)),
        sb::native_render::color_from_rgba8(static_cast<u32>(context.mColorBL)),
    };
    if (!sb::native_render::resolve_transformed_s16_rectangle(layout, draw.rectangle.positions) ||
        !sb::native_render::valid(draw) || !sb::native_render::submit_solid_rectangle(draw)) {
        OSPanic(__FILE__, __LINE__,
                "semantic J2DGrafContext::fillBox sink rejected context=%p rect=%d,%d..%d,%d",
                contextPointer, rect.x1, rect.y1, rect.x2, rect.y2);
    }
}
