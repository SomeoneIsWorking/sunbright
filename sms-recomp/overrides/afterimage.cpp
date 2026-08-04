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

#include "overrides.h"

#include "../runtime/lerp60.h"

#include <intrinsics.h>
#include <lucent/log.h>

namespace aurora::gfx {
void set_feedback_copy_dest(const void* dest);
}

namespace {

// TAfterEffect (include/MarioUtil/ScreenUtil.hpp) and JUTTexture (JUtility/JUTTexture.hpp).
constexpr u32 AFTEREFFECT_TEXTURE = 0x10;   // JUTTexture* unk10, the screen texture it samples
constexpr u32 JUTTEXTURE_TEXDATA  = 0x24;   // void* mTexData — the image buffer the EFB copies into

const void* g_reported = nullptr;
long g_calls = 0;
} // namespace

void sbr_afterimage_note_texture(u32 self) {
    ++g_calls;
    if (sbr_lerp_enabled() && sb_ram_fast(self + AFTEREFFECT_TEXTURE)) {
        const u32 tex = sb_r32(self + AFTEREFFECT_TEXTURE);
        if (tex != 0 && sb_ram_fast(tex + JUTTEXTURE_TEXDATA)) {
            const u32 data = sb_r32(tex + JUTTEXTURE_TEXDATA);
            // Aurora keys copies by the HOST pointer for the guest MEM1 address, and the fifo
            // builds that as `g_ram_base + phys` (dev_gxfifo.cpp, GX_AURORA_LOAD_COPY_DEST). This
            // must produce the identical pointer or the match silently never fires: the copy would
            // keep running twice per tick and the trail would still jitter, with nothing to say
            // why. sb_ram_fast wants the EFFECTIVE address — masking to a physical one first makes
            // it return null for every address, which is exactly how the first version of this
            // failed (10442 calls, zero identifications).
            if (const u8* host = sb_ram_fast(data)) {
                if (host != g_reported) {
                    g_reported = host;
                    aurora::gfx::set_feedback_copy_dest(host);
                    lucent::info("lerp60", "afterimage: TAfterEffect samples the EFB copy at guest "
                                           "0x{:08x} — that copy now runs once per TICK, not once "
                                           "per present, so the trail stops alternating between a "
                                           "half-tick-old and a full-tick-old image",
                                 data);
                }
            }
        }
    }
}

void sbr_afterimage_report() {
    if (!sbr_lerp_enabled()) return;
    if (g_calls == 0) {
        lucent::info("lerp60", "afterimage: TAfterEffect::perform was never called, so no feedback "
                               "copy was identified. That is expected in a scene with no dash blur "
                               "— it is NOT evidence that the once-per-tick rule is working.");
    } else if (g_reported == nullptr) {
        lucent::warn("lerp60", "afterimage: TAfterEffect::perform ran {} times but its texture was "
                               "never readable, so the feedback copy is UNIDENTIFIED and still runs "
                               "once per present. The trail will jitter.",
                     g_calls);
    }
}
