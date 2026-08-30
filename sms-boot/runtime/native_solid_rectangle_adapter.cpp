#include <sunbright/native_render/semantic_sink.h>
#include <sunbright/native_render/solid_rectangle.h>

#include "host_allocation_scope.h"

#include <JSystem/JDrama/JDRRect.hpp>
#include <dolphin/os.h>

#include <cstdint>

extern "C" void sb_native_solid_rectangle_submit(const void* rectPointer, std::uint32_t rgba) {
    if (!sb::native_render::has_semantic_sink())
        return;
    if (rectPointer == nullptr) {
        OSPanic(__FILE__, __LINE__, "semantic fill_rect capture received a null rectangle");
        return;
    }

    const sb::HostAllocationScope hostAllocations;
    const auto& rect = *static_cast<const JDrama::TRect*>(rectPointer);
    const sb::native_render::Color color = sb::native_render::color_from_rgba8(rgba);
    const sb::native_render::SolidRectangleDraw draw{
        {{0.0f, 0.0f}, {640.0f, 480.0f}, {0, 0, 640, 480}},
        {.instance = reinterpret_cast<std::uintptr_t>(rectPointer),
         .source = sb::native_render::SolidRectangleSource::Gc2dFillRect,
         .positions =
             {sb::native_render::Vec2{static_cast<float>(rect.x1), static_cast<float>(rect.y1)},
              sb::native_render::Vec2{static_cast<float>(rect.x2), static_cast<float>(rect.y1)},
              sb::native_render::Vec2{static_cast<float>(rect.x1), static_cast<float>(rect.y2)},
              sb::native_render::Vec2{static_cast<float>(rect.x2), static_cast<float>(rect.y2)}},
         .corner = {color, color, color, color}}};
    if (!sb::native_render::valid(draw) || !sb::native_render::submit_solid_rectangle(draw)) {
        OSPanic(__FILE__, __LINE__, "semantic fill_rect sink rejected rect=%d,%d..%d,%d rgba=%08x",
                rect.x1, rect.y1, rect.x2, rect.y2, rgba);
    }
}
