// tag_shadow.cpp — give the shadow's draws a cross-tick identity so they interpolate.
//
// WHY THE SHADOW SPECIFICALLY. A draw interpolates only if it carries an identity, which
// j3d_capture.cpp emits at J3DShape::draw. Attribution of everything that does NOT
// (frame_interp/tag_gap.cpp, SBR_TAGGAP=1) came back with the entire untagged-DRAW population in
// two places, and both are the shadow:
//
//     J3DShapeDraw::draw                        8,840,210 draws   76.4%
//       ... called 100% from SMS_DrawShape, whose only callers are MarioUtil/ShadowUtil.cpp
//     TModelWaterManager::drawShineShadowVolume 2,733,200 draws   23.6%   (two call sites)
//     SMS_SettingDrawShape                              0 draws    0.0%   (state, no primitives)
//     J3DDisplayListObj::callDL                         0 draws    0.0%   (material DLs)
//
// The last two matter as a warning: counted by CALLS instead of DRAWS, callDL was 61.9% of the gap
// and looked like the thing to fix. It emits no primitives at all. Two populations, one number.
//
// Untagged draws fall through to patch_camera_only and receive the camera delta ALONE. For static
// scenery that is correct. A shadow is the opposite case: it tracks a moving actor, so it followed
// the camera at 60 Hz while its own position stepped at 30 — which is the juddering shadow the user
// reported while the actor above it moved smoothly.
//
// THE IDENTITY, and why it is fp. ShadowUtil draws each shadow from a linked list of
// TAlphaShadowQuad (grp.mFpHead -> mNext), and per entry does
//     PSMTXConcat(view, fp->mMtx, fpMv); GXLoadPosMtxImm(fpMv, GX_PNMTX0); drawShadowVolume(_, fp);
// so `fp` is the per-INSTANCE object and its matrix is a per-instance position matrix in the
// uniform block — exactly the shape patch_draw lerps. Tagging by the shadow MODEL instead would
// collapse every shadow in the scene into one identity and pair instance k with some other
// instance's transform: the same failure j3d_capture.cpp documents for J3DShape, whose signature
// was paired-draw motion of mean 31.9 world units per 1/30 s.
//
// THE PASS INDEX IS PART OF THE KEY. The same fp is drawn again in later passes (the dst-alpha
// mask, then the darken), so fp alone would put several draws of one tick under one identity and
// the pairing would have to guess between them. A per-fp draw counter, reset when the tag is
// cleared, disambiguates them in emission order — which IS stable here, because the passes iterate
// the same list in the same order every tick.
//
// This is a real behaviour change, not a diagnostic: SBR_TAGSHADOW=0 disables it for A/B.

#include "../overrides/overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <unordered_map>

extern "C" void func_802305dc(CPUState&);   // TMBindShadowManager::drawShadowVolume(bool, TAlphaShadowQuad*)
void sbr_gxfifo_draw_tag(uint64_t tag);
bool sbr_lerp_enabled();

namespace {

bool enabled() {
    static const bool v = [] {
        const char* e = std::getenv("SBR_TAGSHADOW");
        return e == nullptr || (e[0] != '0');
    }();
    return v;
}

// How many times each fp has been drawn in the tick so far, so repeated passes over the same list
// get distinct identities. Cleared once per tick by the seam below.
std::unordered_map<u32, u32> g_seenThisTick;
unsigned long g_tagged = 0, g_ticks = 0;

} // namespace

// Called once per tick from the frame seam: a draw ordinal that never resets would grow without
// bound and, worse, would make this tick's tag disagree with the previous tick's for the same
// shadow — so nothing would ever pair and the change would silently do nothing.
void sbr_tag_shadow_begin_tick() {
    g_seenThisTick.clear();
    ++g_ticks;
}

void sbr_tag_shadow_report() {
    if (!enabled() || !sbr_lerp_enabled()) return;
    lucent::info("taggap",
                 "shadow tagging: {} draw(s) given an identity over {} tick(s){}", g_tagged, g_ticks,
                 g_tagged == 0
                     ? "   <-- NONE. Either no shadow drew in this scene, or the hook never fired; "
                       "those are different answers and this line cannot tell them apart, so check "
                       "SBR_TAGGAP=1 for whether the untagged population is still there."
                     : "");
}

namespace {

void ov_draw_shadow_volume(CPUState& cpu) {
    // r4 is the second argument: TAlphaShadowQuad* fp.
    const u32 fp = (u32)cpu.gpr[4];
    const bool tag = enabled() && sbr_lerp_enabled() && fp != 0;
    if (tag) {
        const u32 nth = g_seenThisTick[fp]++;
        // (fp, nth-draw-of-this-fp-this-tick). Both halves are needed; see the header comment.
        sbr_gxfifo_draw_tag(((uint64_t)fp << 32) | (uint64_t)nth);
        ++g_tagged;
    }
    func_802305dc(cpu);
    // Close it, exactly as j3d_capture does: anything drawn after this must not inherit a shadow's
    // identity, which would pair unrelated geometry with a shadow's transform — a wrong answer that
    // renders like a working one.
    if (tag) sbr_gxfifo_draw_tag(0);
}

} // namespace

SB_OVERRIDE(0x802305dcu, ov_draw_shadow_volume, "TMBindShadowManager::drawShadowVolume",
            "60fps: give each shadow instance (TAlphaShadowQuad*) a cross-tick identity so it "
            "interpolates with its caster instead of taking the camera delta alone")
