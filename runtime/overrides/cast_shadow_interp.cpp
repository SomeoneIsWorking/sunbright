// 60 fps interpolation — Mario's REAL cast shadow, IN-PHASE draw (candidate fix).
//
// Default OFF. Enable with SUNBRIGHT_CAST_SHADOW_INPHASE=1 (and SUNBRIGHT_INTERP60=1).
//
// WHY THIS EXISTS (full RE: docs/re_notes/cast_shadow_alpha_chain.md):
//   The cast shadow (TMBindShadowManager) is an EFB DESTINATION-ALPHA STENCIL effect, not a
//   geometry-or-texture draw:
//     Pass 1: render the shadow VOLUME with GXSetColorUpdate(0)/GXSetAlphaUpdate(1)/
//             GXSetDstAlpha -> stamps a per-pixel mask into the EFB ALPHA channel.
//     Pass 2: composite a screen-space dark QUAD with the dst-alpha TEST enabled -> darkens
//             ONLY the masked pixels of the scene already in the EFB.
//   Both passes live inside drawShadowGD (0x8022fa40) / drawShadow (0x8022f014); the effect is
//   single-frame and consumed inside the EFB before the frame's EFB->XFB copy.
//
//   The blink is a DRAW-ORDER (phase) bug, not a geometry bug. On a real frame the shadow runs
//   in the +0x20 mPerformListSilhouette slot: AFTER +0x1C (3D scene fills EFB color/depth/alpha)
//   and BEFORE +0x24 mPerformListGXPost (TEfbCtrlDisp::perform reprograms color/alpha/Z update
//   masks via &0x80 and issues GXCopyDisp EFB->XFB via &0x8). shadow_interp.cpp instead SUPPRESSES
//   the in-between +0x20 perform and replays the shadow AFTER the whole list loop (after +0x24) —
//   so the alpha stencil + composite run out of phase with the EFB they assume. Restoring the
//   draw geometry did nothing because geometry was never the missing ingredient: the PHASE was.
//
// THE FIX (this file, candidate):
//   Draw the cast shadow IN ORDER, at the +0x20 slot, with a mask that DRAWS but does NOT rebuild
//   or finalize: keep &0x8 (DRAW -> drawShadowGD: stamp+composite), drop &0x4 (RESET/calcVtx,
//   which would rebuild the request-driven geometry to empty at 60 Hz) and &0x20000000 (FINALIZE,
//   which would zero the counts). drawCount (mgr+0x20) is restored from the real-field capture.
//
// WIRING (the proper home is shadow_interp.cpp + interp_redraw.cpp, owned by the user — this file
// is the standalone, compile-clean candidate so the build stays green without editing those):
//   In interp_redraw.cpp's in-between path, when sb_cast_shadow_inphase_enabled() is true:
//     (1) let the +0x20 list re-issue normally (do NOT suppress the bind-shadow perform), and
//     (2) intercept TMBindShadowManager::perform (0x80231108) to rewrite the mask + restore count
//         as below, instead of the trailing sb_interp60_draw_shadow() replay.
//   shadow_interp.cpp already captures the real-field drawCount (g_draw_count) and owns the
//   0x80231108 override, so the mask-rewrite belongs there. This file provides the exact logic as
//   a callable helper + the env gate, kept inert by default so nothing collides or regresses.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE

extern bool g_interp60_in_redraw;   // interp_redraw.cpp — true while re-issuing the in-between draw
bool sunbright_interp60();          // interp60.h

namespace {
constexpr u32 BINDSHADOW_MGR_PTR = 0x8040E0C0u;  // -> TMBindShadowManager*  ([r13-0x6100])
constexpr u32 OFF_DRAW_COUNT     = 0x20u;        // built draw-array entry count (drawShadowGD loop bound)

// perform() phase bits (RE'd from the rlwinm. gates in 0x80231108):
constexpr u32 PF_RESET    = 0x00000004u;  // &0x4         -> RESET + calcVtx (rebuilds geometry)
constexpr u32 PF_DRAW     = 0x00000008u;  // &0x8         -> drawShadowGD/drawShadow (stamp+composite)
constexpr u32 PF_FINALIZE = 0x20000000u;  // &0x20000000  -> zero the counts

inline bool valid_ram(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  shadow_mgr() { const u32 m = MEM_R32(BINDSHADOW_MGR_PTR); return valid_ram(m) ? m : 0; }
}  // namespace

// SSOT for the env gate. Default OFF.
extern "C" bool sb_cast_shadow_inphase_enabled() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_CAST_SHADOW_INPHASE") ? 1 : 0;
    return v == 1;
}

extern "C" void func_80231108(CPUState&);   // TMBindShadowManager::perform

// In-phase mask for the in-between bind-shadow perform: DRAW only (no rebuild, no finalize).
// Returns the rewritten mask the caller should put in cpu.gpr[4] before calling the real perform.
extern "C" u32 sb_cast_shadow_inphase_mask(u32 incoming) {
    return (incoming & ~PF_RESET & ~PF_FINALIZE) | PF_DRAW;
}

// Draw the cast shadow at the correct +0x20 phase during the in-between re-issue.
// `gfx` = the in-between TGraphics* (the interpolated-camera snapshot interp_redraw builds).
// `real_field_draw_count` = the drawCount captured on the real field (shadow_interp.cpp's
// g_draw_count). Restores the count, then performs DRAW-only (&0x8) so drawShadowGD replays the
// EFB-alpha stencil + composite over the intact geometry, in order, against this field's EFB.
//
// Intended call site: a tee on TMBindShadowManager::perform (0x80231108) during the in-between,
// in place of the trailing replay — see WIRING above.
extern "C" void sb_cast_shadow_inphase(CPUState& cpu, u32 gfx, u32 real_field_draw_count) {
    if (!sb_cast_shadow_inphase_enabled() || !sunbright_interp60()) return;
    if (!real_field_draw_count) return;
    const u32 m = shadow_mgr();
    if (!m) return;
    MEM_W32(m + OFF_DRAW_COUNT, real_field_draw_count);   // restore the count finalize would zero
    cpu.gpr[3] = m;
    cpu.gpr[4] = PF_DRAW;       // DRAW only: no &0x4 rebuild-to-empty, no &0x20000000 finalize
    cpu.gpr[5] = gfx;
    func_80231108(cpu);
}

#else  // !HAVE_DOLPHIN_CORE — keep symbols so anything that links against them still builds.
#include "../cpu_state.h"
extern "C" bool sb_cast_shadow_inphase_enabled() { return false; }
extern "C" unsigned sb_cast_shadow_inphase_mask(unsigned incoming) { return incoming; }
extern "C" void sb_cast_shadow_inphase(CPUState&, unsigned, unsigned) {}
#endif
