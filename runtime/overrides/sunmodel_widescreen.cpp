// TSunModel EFB occlusion probes — widescreen fix (GMSE01).
//
// The sun visibility pipeline (Camera/sunmodel.cpp, verified by disasm of 0x8002ea70):
//   1. TSunModel::calcDispRatioAndScreenPos_ projects the sun (+ a 16-point disc ring)
//      with the CAMERA's stored projection matrix → normalized screen coords unkF8[17]
//      (±1 = screen edge at the game's own 4:3 aspect), then CLBScreenFPosToSPos maps
//      them to EFB pixels unkB4[17] (640x448, SMSGetGameRenderWidth/Height), with
//      (-1,-1) for off-screen points.
//   2. TSunMgr::drawSyncCallback → TSunModel::getZBufValue (0x8002ea70) GXPeekZ's the
//      EFB at each unkB4 pixel; Z == 0xffffff ⇒ that sun point is unoccluded (unk180).
//      unk191/unk194 (visible count / ratio) and calcHiddenRatio drive the sun glow
//      (TLensGlow), the lens flare alpha (TLensFlare), and TSunMgr's screen whiteout.
//
// Under our widescreen scheme (scene_render.cpp) every projection is pre-squeezed by
// ws_squeeze_scale() = 0.75 at GXSetProjection, so the EFB is anamorphic: a point the
// game computes at 4:3 NDC x actually rasterizes at NDC 0.75*x. The game's unkB4
// pixels are therefore WRONG in x (up to ~80px at the screen edge): the Z probes
// sample the wrong EFB column (occlusion ratio garbage when the sun is off-centre),
// and points in the 16:9 side thirds (|x43| in 1..4/3, on screen and rasterized) are
// rejected as off-screen outright.
//
// NOTE the contrast with TMario::drawSyncCallback's GXPeekARGB occlusion probe: that
// one gets its screen pos from GXProject(GXGetProjectionv(...)) — the LIVE GX
// projection, which holds our squeezed values — so it is already EFB-correct and
// needs no fix (checked, no action; see docs/widescreen_effects.md).
//
// Fix: before running the recompiled getZBufValue, recompute unkB4 from unkF8 with
// the squeeze applied to x (exactly CLBScreenFPosToSPos's mapping, cameralib.hpp:281,
// with x_efb = ws_squeeze_scale()*x and the reject bound therefore widening to the
// true visible |x43| <= 1/scale). The original then probes the right pixels, and
// calcHiddenRatio (which also reads unkB4 validity) stays consistent. unkB4 is
// rewritten from scratch by calcDispRatioAndScreenPos_ every frame, so no restore is
// needed. Disabled when SUNBRIGHT_WIDESCREEN=0.
//
//   0x8002ea70 TSunModel::getZBufValue()            (this=r3)
//   0x802a8bd0 SMSGetGameRenderWidth  → 640
//   0x802a8bc8 SMSGetGameRenderHeight → 448
//   this+0xB4: TVec2<s16> unkB4[17]; this+0xF8: TVec2<f32> unkF8[17]

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdlib>
#include <cstdio>

float ws_squeeze_scale();   // scene_render.cpp

namespace {

constexpr u32 kGetZBufValue       = 0x8002ea70u;
constexpr u32 kGetGameRenderW     = 0x802a8bd0u;
constexpr u32 kGetGameRenderH     = 0x802a8bc8u;

bool widescreen_on() {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    return on;
}

// CLBRoundf<s16> (cameralib.hpp:60): round-half-away-from-zero.
s16 clb_round(f32 v) { return (s16)(v + (v > 0.0f ? 0.5f : -0.5f)); }

void run_orig(CPUState& cpu, u32 addr) {
    if (RecompFunc orig = recomp_raw(addr)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
}

SUNBRIGHT_OVERRIDE(ov_sunmodel_getzbuf_ws, kGetZBufValue) {
    const u32 self = cpu.gpr[3];
    if (!widescreen_on() || self < 0x80000000u) { run_orig(cpu, kGetZBufValue); return; }

    // Game render dims, from the game's own getters (cached — they return constants).
    static s32 W = 0, H = 0;
    if (!W) {
        CPUState c = cpu; call_ppc(c, kGetGameRenderW); W = (s32)(u16)c.gpr[3];
        c = cpu;          call_ppc(c, kGetGameRenderH); H = (s32)(u16)c.gpr[3];
        if (W <= 1 || H <= 1) { W = H = 0; run_orig(cpu, kGetZBufValue); return; }
    }

    static const bool dbg = getenv("SUNBRIGHT_DBG_WS") != nullptr;
    static int dbg_left = 8;
    const f32 scale = ws_squeeze_scale();
    for (int i = 0; i < 17; ++i) {
        const u32 f = self + 0xF8u + 8u * (u32)i;
        const u32 p = self + 0xB4u + 4u * (u32)i;
        const f32 xe = scale * mem_rf32(f);       // where the point really rasterized
        const f32 y  = mem_rf32(f + 4);
        // CLBScreenFPosToSPos semantics, per-axis reject, against the EFB-true x.
        s16 sx = -1, sy = -1;
        if (xe >= -1.0f && xe <= 1.0f) sx = clb_round((1.0f + xe) * (0.5f * (f32)(W - 1)));
        if (y  >= -1.0f && y  <= 1.0f) sy = clb_round((y - 1.0f) * (-0.5f * (f32)(H - 1)));
        if (dbg && i == 0 && dbg_left > 0) {
            --dbg_left;
            std::fprintf(stderr, "[ws-sun] x43=%.3f y=%.3f game=(%d,%d) efb=(%d,%d)\n",
                         mem_rf32(f), y, (s16)mem_r16(p), (s16)mem_r16(p + 2), sx, sy);
        }
        mem_w16(p,     (u16)sx);
        mem_w16(p + 2, (u16)sy);
    }
    run_orig(cpu, kGetZBufValue);
}

} // namespace
