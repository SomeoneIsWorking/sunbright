// afterimage.cpp — tell aurora which EFB copy feeds the dash-blur trail, so interpolated 60fps
// advances it once per game tick instead of once per present.
//
// THE EFFECT (RE: decomp/sms/src/MarioUtil/ScreenUtil.cpp, TAfterEffect). A temporal feedback blur.
// TScreenTexture holds an EFB copy of the frame (half render size, RGB565); TAfterEffect::perform
// draws an 8-vertex GX_TRIANGLEFAN over the viewport textured with that copy, sampled slightly
// scaled and offset —
//
//     fVar4 = unk40 * -0.5f + 0.5f + unk38;    // unk38/3C = offset, unk40/44 = scale
//
// — and alpha-blended over the new frame, so each frame smears the last one outward. That is the
// ghost trail behind Mario when he dashes. Its parameters are exponentially smoothed once per frame
// (unk20 += 0.05f * (target - unk20)) and decay per frame in calcDashBlurValue.
//
// WHY 60fps BREAKS IT. Interpolation presents each tick twice, and both emissions replay the same
// recorded pass list — including its EFB copies. So the screen texture is written TWICE per tick,
// from two different images: the interpolated pose at t-0.5 and the true pose at t. The trail
// therefore samples a half-tick-old image on one present and a full-tick-old image on the next, and
// the feedback advances at double rate. That alternation is the jitter.
//
// Re-running EFB copies per emission is RIGHT for an intra-frame copy, where a later pass of the
// same emission samples what that emission wrote (the sea reflection, indirect passes). It is wrong
// only for a copy whose consumer is the NEXT frame. Nothing structural distinguishes the two — the
// difference is who reads the result and when — so the host has to say which is which, and the
// authority on that is the effect itself: whatever texture TAfterEffect SAMPLES is by definition
// the feedback texture.
//
// The smoothing constants are deliberately NOT rescaled for 60fps. Game logic still ticks at 30 Hz
// under render interpolation — that is the whole point of the design — so the effect's own state
// advances at exactly the rate it was written for. Only the capture rate was wrong.
//
// THIS FILE DOES NOT REGISTER AN OVERRIDE. TAfterEffect::perform already has one (widescreen_effects
// .cpp, which unsqueezes the quad's ortho), and one guest address gets exactly one override — a
// second registration used to silently replace the first, so whichever TU's static initializer ran
// last won and the other's work vanished with no diagnostic. override_register now refuses. The
// identification below is called FROM that existing override instead.

#include "../overrides/overrides.h"

#include "stream_interp.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

namespace {

// TAfterEffect (include/MarioUtil/ScreenUtil.hpp) and JUTTexture (JUtility/JUTTexture.hpp).
constexpr u32 AFTEREFFECT_TEXTURE = 0x10;   // JUTTexture* unk10, the screen texture it samples
constexpr u32 JUTTEXTURE_TEXDATA  = 0x24;   // void* mTexData — the image buffer the EFB copies into

long g_calls = 0;
bool g_drewThisTick = false;
long g_drawCalls = 0;
} // namespace

// POSITIVE CONTROL. The dash trail only draws while the player is dashing, so no automated run ever
// exercises the cross-frame-feedback case, and a classifier that has only been shown not to
// misfire is not a classifier that has been shown to FIRE. SBR_FORCE_DASHBLUR=1 forces the real
// effect to draw every frame — state 2 is the calcDashBlurValue branch, bit 0 of unk14 is its
// enable — so the copy it samples becomes a genuine feedback copy on the real code path.
//
// Diagnostic only, and it MUTATES GUEST STATE: never leave it on for a normal run.
void sbr_afterimage_force(u32 self) {
    static const bool on = [] {
        const char* e = std::getenv("SBR_FORCE_DASHBLUR");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    if (!on || !sb_ram_fast(self + 0x50)) return;
    sb_w8(self + 0x14, (u8)(sb_r8(self + 0x14) | 1));   // enabled
    sb_w8(self + 0x15, 2);                              // calcDashBlurValue branch
    // A dash amount that never runs down, so the effect keeps drawing for the whole run.
    const float amount = 0.5f;
    u32 bits;
    __builtin_memcpy(&bits, &amount, sizeof bits);
    sb_w32(self + 0x50, bits);
}

void sbr_afterimage_tick() { g_drewThisTick = false; }

void sbr_afterimage_note_texture(u32 self, bool drawing) {
    (void)self;
    ++g_calls;
    if (drawing) {
        ++g_drawCalls;
        g_drewThisTick = true;
    }
}

void sbr_afterimage_report() {
    if (!sbr_lerp_enabled()) return;
    lucent::info("lerp60", "afterimage: perform ran {} times, of which {} actually DREW the trail. "
                           "Zero draws means the cross-frame feedback case was never exercised, so "
                           "a copy classifier reporting no feedback copies has proved nothing.",
                 g_calls, g_drawCalls);
    if (g_calls == 0) {
        lucent::info("lerp60", "afterimage: TAfterEffect::perform was never called, so no feedback "
                               "copy was identified. That is expected in a scene with no dash blur "
                               "— it is NOT evidence that the once-per-tick rule is working.");
    }
}
