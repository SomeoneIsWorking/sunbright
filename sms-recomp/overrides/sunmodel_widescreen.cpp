// sunmodel_widescreen.cpp — the sun's EFB occlusion probes, corrected for the anamorphic frame.
//
// Ported from the retired runtime/overrides/sunmodel_widescreen.cpp (git 9283f44^), where the RE
// below was done.
//
// THE SUN VISIBILITY PIPELINE (Camera/sunmodel.cpp, verified there by disassembly of 0x8002ea70):
//   1. TSunModel::calcDispRatioAndScreenPos_ projects the sun plus a 16-point disc ring using the
//      CAMERA's stored projection -> normalized screen coords unkF8[17] (+-1 = screen edge at the
//      game's own 4:3 aspect), then CLBScreenFPosToSPos maps those to EFB pixels unkB4[17]
//      (640x448), writing (-1,-1) for anything it considers off-screen.
//   2. TSunModel::getZBufValue GXPeekZ's the EFB at each of those pixels; Z == 0xffffff means that
//      sample of the sun is unoccluded. The visible count and ratio drive the sun glow (TLensGlow),
//      the lens flare's alpha (TLensFlare) and TSunMgr's screen whiteout.
//
// WHY WIDESCREEN BREAKS IT. widescreen.cpp squeezes every projection at GXSetProjection, so the EFB
// is anamorphic: a point the game computes at 4:3 NDC x actually rasterizes at 0.75*x. The game's
// own unkB4 pixels are therefore wrong in x by up to ~80px at the screen edge — the probes sample
// the WRONG EFB COLUMN, so the occlusion ratio is garbage whenever the sun is off-centre — and
// points in the new 16:9 side thirds (|x| between 1 and 4/3, genuinely on screen and rasterized)
// are rejected as off-screen outright.
//
// THE FIX: recompute unkB4 from unkF8 with the squeeze applied, which is exactly
// CLBScreenFPosToSPos's own mapping with x_efb = scale*x, and the reject bound therefore widening
// to the truly visible |x| <= 1/scale. The game's own body then probes the right pixels, and
// calcHiddenRatio (which also reads unkB4's validity) stays consistent. unkB4 is rewritten from
// scratch by calcDispRatioAndScreenPos_ every frame, so nothing needs restoring.
//
// NOT THE SAME as TMario::drawSyncCallback's GXPeekARGB occlusion probe: that one takes its screen
// position from GXProject(GXGetProjectionv(...)) — the LIVE GX projection, which already holds our
// squeezed values — so it is EFB-correct as-is and must be left alone.
//
//   0x8002ea70 TSunModel::getZBufValue()   (this=r3)
//   this+0xB4: TVec2<s16> unkB4[17]   this+0xF8: TVec2<f32> unkF8[17]

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

extern "C" void func_8002ea70(CPUState&);   // TSunModel::getZBufValue
extern "C" void func_802a8bd0(CPUState&);   // SMSGetGameRenderWidth
extern "C" void func_802a8bc8(CPUState&);   // SMSGetGameRenderHeight

float sbr_ws_squeeze_scale();   // widescreen.cpp
int   sbr_ws_pillar();          // widescreen.cpp — 0 when widescreen is off

namespace {

constexpr u32 GET_ZBUF_VALUE = 0x8002ea70u;
constexpr u32 SUN_SPOS = 0xB4u;   // TVec2<s16>[17], EFB pixels
constexpr u32 SUN_FPOS = 0xF8u;   // TVec2<f32>[17], normalized screen coords

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

// CLBRoundf<s16> (cameralib.hpp): round half away from zero.
s16 clb_round(f32 v) { return (s16)(v + (v > 0.0f ? 0.5f : -0.5f)); }

void ov_sun_get_zbuf_value(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    if (sbr_ws_pillar() == 0 || !sb_ram_fast(self)) { func_8002ea70(cpu); return; }

    // The game's own render dimensions, from its own getters, so this cannot drift from them.
    static s32 W = 0, H = 0;
    if (W == 0) {
        CPUState c = cpu;
        func_802a8bd0(c);
        W = (s32)(u16)c.gpr[3];
        c = cpu;
        func_802a8bc8(c);
        H = (s32)(u16)c.gpr[3];
        if (W <= 1 || H <= 1) {
            lucent::error("widescreen", "sun probe: implausible render size {}x{}", W, H);
            W = H = 0;
            func_8002ea70(cpu);
            return;
        }
        lucent::debug("widescreen", "sun occlusion probes corrected for a {}x{} anamorphic EFB", W, H);
    }

    const f32 scale = (f32)sbr_ws_squeeze_scale();
    for (int i = 0; i < 17; ++i) {
        const u32 f = self + SUN_FPOS + 8u * (u32)i;
        const u32 p = self + SUN_SPOS + 4u * (u32)i;
        const f32 xe = scale * guest_f32(f);   // where the point REALLY rasterized
        const f32 y  = guest_f32(f + 4);
        // CLBScreenFPosToSPos's mapping, rejecting per axis against the EFB-true x.
        s16 sx = -1, sy = -1;
        if (xe >= -1.0f && xe <= 1.0f) sx = clb_round((1.0f + xe) * (0.5f * (f32)(W - 1)));
        if (y  >= -1.0f && y  <= 1.0f) sy = clb_round((y - 1.0f) * (-0.5f * (f32)(H - 1)));
        sb_w16(p + 0, (u16)sx);
        sb_w16(p + 2, (u16)sy);
    }

    func_8002ea70(cpu);
}

} // namespace

SB_OVERRIDE(GET_ZBUF_VALUE, ov_sun_get_zbuf_value, "TSunModel::getZBufValue",
            "widescreen: the sun's occlusion probes address the anamorphic EFB, so their pixel "
            "columns must be recomputed or the glow and lens flare read the wrong pixels")
