// mark_exact.cpp — the draws that must be presented EXACTLY on an interpolated frame.
//
// SMS_FillScreenAlpha (decomp/sms src/MarioUtil/ScreenUtil.cpp:236) loads an IDENTITY position
// matrix and emits a ±1000-unit quad at z = -200 with colour writes off and alpha writes on: a
// screen-covering destination-alpha mask that gates what later passes are allowed to draw. Its
// vertices are already in eye space, so it is not a model at a place in the world — it is a shape
// nailed to the display.
//
// The interpolator could not know that. The draw is under the scene's PERSPECTIVE projection, so
// the orthographic test that correctly protects the HUD does not see it, and it fell through to
// patch_camera_only — which left-multiplies the camera delta into its identity matrix and slides
// the mask by a fraction of the camera's motion on every in-between frame. A mask that gates
// rendering must not move at all.
//
// This is NOT interpolation and it is not a snap-because-we-cannot: it is a declaration, from the
// side that knows the geometry, that the correct in-between image of this draw is the draw itself.
// The audit files it under its own outcome (snap:EXACT) so it is never counted as a shortfall.
//
// WHY A SEAM RATHER THAN A HEURISTIC. "Identity position matrix" would be a plausible automatic
// test and it is wrong: plenty of world geometry is drawn with an identity model matrix when the
// view is folded in elsewhere, and mis-detecting one of those would freeze real geometry in place
// while the camera moved. The game knows which draws are screen furniture; this asks it.

#include "../overrides/overrides.h"

#include <intrinsics.h>

extern "C" void func_8022d1a4(CPUState&);   // SMS_FillScreenAlpha(u8)

void sbr_gxfifo_draw_exact(bool on);
bool sbr_lerp_enabled();

namespace {

struct Exact {
    bool on;
    Exact() : on(sbr_lerp_enabled()) {
        if (on) sbr_gxfifo_draw_exact(true);
    }
    ~Exact() {
        if (on) sbr_gxfifo_draw_exact(false);
    }
};

void ov_fill_screen_alpha(CPUState& cpu) {
    Exact e;
    func_8022d1a4(cpu);
}

} // namespace

SB_OVERRIDE(0x8022d1a4u, ov_fill_screen_alpha, "SMS_FillScreenAlpha",
            "60fps: a screen-space dst-alpha mask under a perspective projection — declared EXACT "
            "so the camera delta cannot slide it")
