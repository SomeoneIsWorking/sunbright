// Full-screen 2D screenspace effects — widescreen fix (GMSE01).
//
// Same problem class as the fader (fader_widescreen.cpp): these effects fill the 4:3
// display/viewport rect (0..640) with a GX quad under the CURRENT 2D ortho. Our
// widescreen scheme pre-squeezes every 2D ortho so 0..640 presents as the centre 4:3
// of the 16:9 frame — so the effect leaves the side thirds uncovered.
//
//   0x8022d4f8 TAfterEffect::perform(u32, JDrama::TGraphics*)
//       (this=r3, flags=r4, gfx=r5) — the dash-blur / screen-flash overlay
//       (MarioUtil/ScreenUtil.cpp): draws an 8-vert fan over gfx->getViewport()
//       in screen coords, textured with the half-res screen capture.
//
// Fix (the proven fader pattern): scope the draw with ws_2d_suspend_begin/end
// (fader_widescreen.cpp), which re-issues the last 2D ortho UNSQUEEZED so 0..640
// spans the whole 16:9 present, then re-squeezes on exit. No rect edit needed:
// the quad already covers the full viewport, and stretching the screen-capture
// texture across the full width is CORRECT — the capture is of the anamorphic
// 16:9 EFB. Disabled when SUNBRIGHT_WIDESCREEN=0.
//
// (TSunGlass 0x8017d354, the full-screen tint/darken curtain, is NOT here — it is
// already widened by the LR-keyed GXLoadPosMtxImm matrix un-squeeze in hud.cpp,
// TSUNGLASS_POSMTX_LR; adding a second adjustment would double-apply.)

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdio>
#include <cstdlib>

// fader_widescreen.cpp
void ws_2d_suspend_begin(CPUState& cpu);
void ws_2d_suspend_end(CPUState& cpu);

namespace {

constexpr u32 kAfterEffectPerf = 0x8022d4f8u;

bool widescreen_on() {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    return on;
}

void run_orig(CPUState& cpu, u32 addr) {
    if (RecompFunc orig = recomp_raw(addr)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
}

SUNBRIGHT_OVERRIDE(ov_aftereffect_perform_ws, kAfterEffectPerf) {
    const u32 self  = cpu.gpr[3];
    const u32 flags = cpu.gpr[4];
    // Mirror the early-outs (ScreenUtil.cpp): only the draw pass (0x10) with the
    // effect enabled (unk14 bit 0) emits the quad — don't churn projections otherwise.
    const bool draws = widescreen_on() && (flags & 0x10) &&
                       self >= 0x80000000u && (mem_r8(self + 0x14) & 1);
    if (!draws) { run_orig(cpu, kAfterEffectPerf); return; }
    static bool once = false;
    if (!once) { once = true; std::fprintf(stderr, "[screenfx] TAfterEffect draw pass reached — 2D-squeeze suspend active\n"); }
    ws_2d_suspend_begin(cpu);
    run_orig(cpu, kAfterEffectPerf);
    ws_2d_suspend_end(cpu);
}

} // namespace
