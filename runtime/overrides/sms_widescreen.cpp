// Rendering code we own for 16:9 widescreen.
//
// Core aspect: patched as a .data constant in widescreen_patch_tick()
// (runtime/main_sdl.cpp) — the game then computes the projection, culling, and
// projection-derived effects at 16:9 from the source.
//
// Heat-haze: removed here (it glitches under 16:9 and is unwanted anyway).
//
// The 2D-overlay offsets (title text, fade curtain, HUD) remain: the gamemasterplc
// Gecko code fixes them with per-element edits across several J2D functions, but
// measurement showed those edits target panes/screens not exercised the way a
// post-hoc override could reach — porting them faithfully needs per-screen RE of
// conditionally-built 2D layout, a larger effort tracked separately.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>

// ── Heat-haze removal ────────────────────────────────────────────────────────
// TShimmer::perform (USA 0x8019f83c): full-screen heat-distortion. Gecko
// "0419f83c 4e800020" disables it by writing blr to its entry — we return
// immediately. SUNBRIGHT_KEEP_SHIMMER=1 keeps it.
static void ov_heatwave_skip(CPUState& cpu) { call_ppc(cpu, cpu.lr); }
static const bool s_heatwave_registered = [] {
    if (!getenv("SUNBRIGHT_KEEP_SHIMMER")) register_override(0x8019f83c, &ov_heatwave_skip);
    return true;
}();
