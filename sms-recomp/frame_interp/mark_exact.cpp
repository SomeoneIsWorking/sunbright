// mark_exact.cpp — the primitives that must be presented EXACTLY on an interpolated frame.
//
// WHAT THEY ARE. Two functions emit the same shape: an IDENTITY position matrix followed by a
// ±1000-unit quad at z = -200, with colour writes off and alpha writes on — a screen-covering
// destination-alpha mask that gates what later passes are allowed to draw.
//
//   SMS_FillScreenAlpha                        (decomp/sms src/MarioUtil/ScreenUtil.cpp:236)
//   TModelWaterManager::drawShineShadowVolume  (src/Player/ModelWaterManager.cpp, two quads)
//
// Their vertices are already in eye space, so these are not models at a place in the world — they
// are shapes nailed to the display.
//
// The interpolator could not know that. The draws are under the scene's PERSPECTIVE projection, so
// the orthographic test that correctly protects the HUD does not see them, and they fell through to
// patch_camera_only — which left-multiplies the camera delta into an identity matrix and slides a
// mask that gates rendering. This is not interpolation and it is not a snap-because-we-cannot: it
// is a declaration, from the side that knows the geometry, that the correct in-between image of
// this draw is the draw itself. The audit files them as snap:EXACT so they are never counted as a
// shortfall.
//
// ── WHY THIS MARKS PRIMITIVES AND NOT CALLS ─────────────────────────────────────────────────────
//
// drawShineShadowVolume emits its two screen quads AND its sphere-slice display lists from the same
// call, and the slices interpolate correctly today. Wrapping the call in an exact flag would freeze
// the slices, so the flag is one-shot (consumed by the draw it precedes) and set from the GXBegin
// seam — which fires per immediate primitive and never for a display list. The quads are marked;
// the slices are untouched; nothing had to guess which was which.
//
// That is also why drawShineShadowVolume is not overridden here: it already has one, in
// tag_shadow.cpp, and one address gets exactly one override. The marking is installed from that
// hook — a mark is a line of code, not a hook.
//
// WHY A SEAM RATHER THAN A HEURISTIC. "Identity position matrix" would be a plausible automatic
// test and it is wrong: plenty of world geometry is drawn with an identity model matrix when the
// view is folded in elsewhere, and mis-detecting one of those would freeze real geometry in place
// while the camera moved. The game knows which draws are screen furniture; this asks it.

#include "../overrides/overrides.h"
#include "mark_exact.h"

#include <intrinsics.h>
#include <lucent/log.h>

extern "C" void func_8022d1a4(CPUState&);   // SMS_FillScreenAlpha(u8)

void sbr_gxfifo_mark_exact();
bool sbr_lerp_enabled();
void (*sbr_gxbegin_set_hook(void (*fn)()))();

namespace {

unsigned long g_marked = 0;

void on_gx_begin() {
    sbr_gxfifo_mark_exact();
    ++g_marked;
}

void ov_fill_screen_alpha(CPUState& cpu) {
    SbExactScope e;
    func_8022d1a4(cpu);
}

} // namespace

SbExactScope::SbExactScope() : on(sbr_lerp_enabled()), prevHook(nullptr) {
    if (on) prevHook = sbr_gxbegin_set_hook(&on_gx_begin);
}

SbExactScope::~SbExactScope() {
    if (on) sbr_gxbegin_set_hook(prevHook);
}

void sbr_mark_exact_report() {
    if (!sbr_lerp_enabled()) return;
    lucent::info("taggap", "screen-space masks: {} primitive(s) marked EXACT{}", g_marked,
                 g_marked == 0 ? "   <-- NONE, so either no screen mask drew this run or the "
                                 "GXBegin seam is not reaching them. Those are different faults and "
                                 "this count cannot tell them apart."
                              : "");
}

SB_OVERRIDE(0x8022d1a4u, ov_fill_screen_alpha, "SMS_FillScreenAlpha",
            "60fps: a screen-space dst-alpha mask under a perspective projection — each of its "
            "primitives is declared EXACT so the camera delta cannot slide it")
