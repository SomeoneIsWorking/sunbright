#include "j2d_fill_box_adapter.h"

#include "overrides.h"
#include "semantic_j2d_context.h"

#include "../runtime/sb_assert.h"

#include <sunbright/native_render/semantic_sink.h>

extern "C" void func_802eba70(CPUState&); // J2DGrafContext::fillBox

namespace {

void override_j2d_fill_box(CPUState& cpu) {
    if (sb::native_render::has_semantic_sink()) {
        const sb::native_render::PictureContext* context =
            sb::recomp::current_semantic_j2d_context();
        SB_ASSERT(context != nullptr,
                  "semantic J2DGrafContext::fillBox has no active orthographic context: self=%08x "
                  "rect=%08x",
                  cpu.gpr[3], cpu.gpr[4]);
        sb::native_render::SolidRectangleDraw draw{};
        SB_ASSERT(sb::recomp::capture_j2d_fill_box(sb::recomp::live_guest_byte_reader(), cpu.gpr[3],
                                                   cpu.gpr[4], *context, draw),
                  "semantic J2DGrafContext::fillBox capture failed: self=%08x rect=%08x",
                  cpu.gpr[3], cpu.gpr[4]);
        SB_ASSERT(sb::native_render::submit_solid_rectangle(draw),
                  "semantic J2DGrafContext::fillBox sink rejected validated command: self=%08x "
                  "rect=%08x",
                  cpu.gpr[3], cpu.gpr[4]);
    }
    func_802eba70(cpu);
}

} // namespace

SB_OVERRIDE(0x802eba70u, override_j2d_fill_box, "J2DGrafContext::fillBox",
            "publish transformed solid or four-corner-gradient J2D boxes and retain the guest body")
