// Widescreen full-width GX fills (GMSE01).
//
// GC2D's shared `fill_rect(const JDrama::TRect&, TColor)` (0x80140390, anonymous-ns
// static next to the fader) draws solid bands/overlays — including the stage-name
// "telop" banner backdrop (TGCConsole2::startAppearTelop) seen at scene entry. Those
// fills are authored full-display (x1<=0, x2>=640); under widescreen the 2D plane's
// 0..640 maps to the centre 4:3, so the band stopped at the 4:3 edges with the scene
// visible beside it (user report; pixel-verified on the Plaza pan-in).
//
// Fix: wrap fill_rect; when the rect spans the full authored display width (and is
// not already widened), stretch its x extents by w/6+1 per side for the recompiled
// call, then restore. Partial fills (wipe boxes, dialogs) are untouched. Same shape
// as the TSMSFader fix (fader_widescreen.cpp) — that one stays because drawFadeinout
// draws its own quad without going through fill_rect.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdlib>

extern "C" void func_80140390(CPUState&);

namespace {

SUNBRIGHT_OVERRIDE(ov_fill_rect_ws, 0x80140390u) {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    const u32 rect = cpu.gpr[3];
    if (!on || rect < 0x80000000u) { func_80140390(cpu); return; }
    const s32 x1 = (s32)mem_r32(rect), x2 = (s32)mem_r32(rect + 8);
    const bool fullw = (x1 <= 0 && x1 > -40 && x2 >= 600 && x2 < 700);
    if (!fullw) { func_80140390(cpu); return; }
    const s32 extra = (x2 - x1) / 6 + 1;
    mem_w32(rect,     (u32)(x1 - extra));
    mem_w32(rect + 8, (u32)(x2 + extra));
    func_80140390(cpu);
    mem_w32(rect,     (u32)x1);
    mem_w32(rect + 8, (u32)x2);
}

} // namespace
