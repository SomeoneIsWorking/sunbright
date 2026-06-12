// JDrama EFB→texture passes — keep them out of the widescreen 2D squeeze (GMSE01).
//
// The scene graph runs OFFSCREEN render-to-EFB-then-copy passes bracketed by
// JDrama::TEfbCtrlTex::perform (JDREfbCtrl.cpp):
//
//   (efbTex, 0x80)  pixel-format + setDisplayRect        → pass START
//   (viewport, 0x8) TViewport: GXSetViewport+Scissor in texture pixels
//   (ortho,   0x10) TOrthoProj: C_MTXOrtho(0..w, 0..h) → GXSetProjection ORTHO
//   (content, ...)  the pass body (drawn in texture-pixel space)
//   (efbTex,  0x8)  GXSetTexCopySrc(rect) + GXCopyTex     → pass END
//
// Concrete instances (System/MarDirectorInitECT.cpp): the GRAFFITI/POLLUTION counting
// passes ("落書きグループ": ortho 0..512 + per-layer ortho 0..img size), which draw the
// pollution maps and count lit pixels with GXReadPixMetric / copy the EFB region back
// into the layer texture; and the mirror pre-render (initECTMir).
//
// These orthos are in TEXTURE pixel space, 1:1 with the GXCopyTex source rect — but
// our widescreen scheme squeezes EVERY 2D ortho by 0.75 once 3D has been seen
// (scene_render.cpp), so the pass content rendered ×0.75 narrower than the copy/count
// region: pollution pixel COUNTS shrink ~25% (goop coverage % wrong → event triggers
// wrong) and each copy-back smears the layer texture. TOrthoProj itself canNOT be
// blanket-exempted — the same class is the real screen 2D camera (MenuDir/InitECT
// ortho(0,16,600,464)) which MUST stay squeezed.
//
// Fix at the bracket: while a TEfbCtrlTex pass is open (0x80 seen, until its 0x8
// closes it), suspend the 2D squeeze flag (fader_widescreen.cpp) so every ortho the
// pass issues goes to the GP verbatim. Perspective squeezing is untouched by the flag
// (scene_render checks `is2d && g_ws_2d_suspend`), so EFB-tex passes that capture the
// 3D scene stay consistent with the main render. TEfbCtrlDisp::perform(0x80) — the
// start of the SCREEN display pass — force-clears the flag as a safety net so a pass
// that never closes can't leak the suspend into visible 2D.
//
//   0x802f8bac JDrama::TEfbCtrlTex::perform(u32, TGraphics*)   (this=r3, flags=r4)
//   0x802f8904 JDrama::TEfbCtrlDisp::perform(u32, TGraphics*)

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdio>
#include <cstdlib>

extern bool g_ws_2d_suspend;     // fader_widescreen.cpp
extern bool g_ws_persp_suspend;  // fader_widescreen.cpp

namespace {

constexpr u32 kEfbCtrlTexPerform  = 0x802f8bacu;
constexpr u32 kEfbCtrlDispPerform = 0x802f8904u;
constexpr u32 kMirrorCamPerform   = 0x80193fbcu;

bool widescreen_on() {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    return on;
}

void run_orig(CPUState& cpu, u32 addr) {
    if (RecompFunc orig = recomp_raw(addr)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
}

SUNBRIGHT_OVERRIDE(ov_efbtex_perform_ws, kEfbCtrlTexPerform) {
    const u32 flags = cpu.gpr[4];
    if (!widescreen_on()) { run_orig(cpu, kEfbCtrlTexPerform); return; }
    if (flags & 0x80) {                 // pass START: orthos until the close are texture-space
        g_ws_2d_suspend = true;
        static bool once = false;
        if (!once) { once = true; std::fprintf(stderr,
            "[screenfx] TEfbCtrlTex pass reached — 2D squeeze suspended for EFB-tex passes\n"); }
    }
    run_orig(cpu, kEfbCtrlTexPerform);
    if (flags & 0x8)                    // pass END: EFB region copied, squeeze resumes
        g_ws_2d_suspend = false;
}

SUNBRIGHT_OVERRIDE(ov_efbdisp_perform_ws, kEfbCtrlDispPerform) {
    // Screen display pass begins — never let a dangling EFB-tex suspend leak into it.
    if (widescreen_on() && (cpu.gpr[4] & 0x80)) { g_ws_2d_suspend = false; g_ws_persp_suspend = false; }
    run_orig(cpu, kEfbCtrlDispPerform);
}

// ── TMirrorCamera::perform 0x80193fbc — offscreen mirror perspective, no squeeze ────
// (Map/MapMirror.cpp) The mirror pre-render ("鏡描画ステージ", an EfbCtrlTex pass into
// a 256x256 texture) issues C_MTXPerspective(fovy*k, mAspect 4:3) → GXSetProjection
// PERSPECTIVE, and the texture is later sampled in the MAIN pass via the projective
// texture matrix drawSetting() builds with C_MTXLightPerspective from the SAME
// unsqueezed camera params. Squeezing the render but not the lookup shifts the
// reflection horizontally by ~25% toward the texture centre. Offscreen perspective
// renders consumed through camera-derived texture matrices must stay 4:3: suspend the
// perspective squeeze for the projection this perform issues.
SUNBRIGHT_OVERRIDE(ov_mirrorcam_perform_ws, kMirrorCamPerform) {
    if (!widescreen_on()) { run_orig(cpu, kMirrorCamPerform); return; }
    const bool prev = g_ws_persp_suspend;
    g_ws_persp_suspend = true;
    run_orig(cpu, kMirrorCamPerform);
    g_ws_persp_suspend = prev;
}

} // namespace
