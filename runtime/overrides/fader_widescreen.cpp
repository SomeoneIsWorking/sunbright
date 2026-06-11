// TSMSFader widescreen fix (GMSE01).
//
// The fader (GC2D/ScrnFader.cpp) fills the caller's JDrama::TRect — the 4:3 display
// rect (0,0,640,480) — with a GX quad. Under our default widescreen config the 2D
// ortho plane keeps 0..640 mapped to the CENTER 4:3 of the 16:9 frame (that's why the
// HUD elements had to be shifted out to the edges), so screen fades/wipe fills leave
// the side thirds unfaded.
//
// Fix: wrap the fader draw entries; widen the incoming rect's x extents to the 16:9
// overscan (±640/6 ≈ ±107) for the duration of the recompiled call, then restore the
// caller's rect. y is untouched. Disabled when SUNBRIGHT_WIDESCREEN=0 (4:3 mode).
//
//   0x8013fa54 TSMSFader::drawFadeinout(const TRect&)   (this=r3, rect=r4)
//   0x8013fc88 TSMSFader::draw(const TRect&)            (this=r3, rect=r4)
//
// TRect = JUTRect: s32 x1,y1,x2,y2.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdlib>

extern "C" void func_8013fa54(CPUState&);
extern "C" void func_8013fc88(CPUState&);

// Fader scope: suspend the global 2D widescreen squeeze (scene_render.cpp reads these) so
// fade fills AND the hx_wiper circle-wipe curtain — which draw 0..640 geometry with the
// current ortho — span the full 16:9 present instead of the centre 4:3 (the "wipe pillars"
// frame, 2026-06-11). The last-loaded 2D ortho is re-issued unsqueezed on entry and
// re-squeezed on exit.
bool g_ws_2d_suspend = false;
u32  g_ws_last_ortho = 0;

namespace {

constexpr u32 kGXSetProjection = 0x80362c34u;

void reload_ortho(CPUState& cpu) {
    if (!g_ws_last_ortho) return;
    CPUState c = cpu;                       // scratch: callee-preserved regs are enough
    c.gpr[3] = g_ws_last_ortho;
    c.gpr[4] = 1;                           // GX_ORTHOGRAPHIC
    call_ppc(c, kGXSetProjection);
}

bool widescreen_on() {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    return on;
}

// 16:9 frame shows 4/3 the horizontal range of the 4:3 ortho plane: extra = w/6 per side.
void with_widened_rect(CPUState& cpu, void (*real)(CPUState&)) {
    const u32 rect = cpu.gpr[4];
    if (!widescreen_on() || rect < 0x80000000u) { real(cpu); return; }
    const s32 x1 = (s32)mem_r32(rect), x2 = (s32)mem_r32(rect + 8);
    const s32 extra = (x2 - x1) / 6 + 1;
    mem_w32(rect,     (u32)(x1 - extra));
    mem_w32(rect + 8, (u32)(x2 + extra));
    real(cpu);
    mem_w32(rect,     (u32)x1);
    mem_w32(rect + 8, (u32)x2);
}

SUNBRIGHT_OVERRIDE(ov_fader_drawFadeinout, 0x8013fa54u) {
    with_widened_rect(cpu, func_8013fa54);
}

SUNBRIGHT_OVERRIDE(ov_fader_draw, 0x8013fc88u) {
    if (!widescreen_on()) { func_8013fc88(cpu); return; }
    g_ws_2d_suspend = true;
    reload_ortho(cpu);                       // unsqueezed: 0..640 now spans the 16:9 present
    with_widened_rect(cpu, func_8013fc88);   // rect widening is harmless overshoot here
    g_ws_2d_suspend = false;
    reload_ortho(cpu);                       // re-apply the squeeze for whatever draws next
}

} // namespace
