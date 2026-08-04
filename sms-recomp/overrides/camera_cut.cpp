// camera_cut.cpp — the GAME's own signal that this tick's camera has no in-between.
//
// WHY THIS IS NOT A THRESHOLD. Interpolated 60fps renders a moment halfway between two ticks. That
// moment is only meaningful when the two ticks are on a continuous path; when the camera CUTS —
// warped to a new place — the halfway pose is a viewpoint the game never simulated, and lerping
// across it is not smoothing, it is inventing a frame. So a cut tick must SNAP.
//
// The obvious discriminator is "the eye moved more than N units this tick", and it is
// unfalsifiable: the measured distribution of camera step has a populated middle (34 ticks in
// [10,100), 36 in [100,1k) over a 898-tick plaza run, see
// debug_journal/2026-08-04_interp60_pairing_attribution.md) with no gap to put a threshold in.
// Any N either cuts genuinely fast camera motion or misses genuinely small warps, and which one it
// does is invisible in the output.
//
// The game already knows. CPolarSubCamera::warpPosAndAt (CameraWarp.cpp) sets mPosition/mTarget
// outright AND does `mInbetween->mFramesRemaining = 0`, i.e. it explicitly kills the
// interpolation the game itself would otherwise run. That is a discontinuity declared by the code
// that caused it, not inferred from its magnitude.
//
// Both overloads are hooked. The (f32, s16) one computes a position and tail-calls the
// (Vec&, Vec&) one, so hooking only the latter would already catch it — it is hooked anyway so the
// signal does not depend on that call staying a tail call through a recompiler change.
//
// OBSERVE-ONLY: the real body always runs. These are not replacements (see overrides.h); they are
// the same observing-wrapper shape as J3DShape::draw in j3d_capture.cpp.

#include "overrides.h"

#include "../runtime/lerp60.h"

#include <intrinsics.h>

#include <lucent/log.h>

extern "C" void func_800335d4(CPUState&);   // CPolarSubCamera::warpPosAndAt(const Vec&, const Vec&)
extern "C" void func_80033390(CPUState&);   // CPolarSubCamera::warpPosAndAt(f32, s16)

namespace {

// Set when the game warps the camera; read and cleared once per tick by the frame seam.
bool g_cutThisTick = false;
long g_cutTicks = 0;
long g_warpCalls = 0;

void note_warp() {
    ++g_warpCalls;
    // Once per warp, not once per tick: two warps in a tick are one cut for interpolation purposes
    // but two events worth seeing, and collapsing them here would hide a double-warp.
    g_cutThisTick = true;
}

void ov_warp_pos_at(CPUState& cpu) {
    note_warp();
    func_800335d4(cpu);
}

void ov_warp_ratio_yaw(CPUState& cpu) {
    note_warp();
    func_80033390(cpu);
}

} // namespace

bool sbr_camera_cut_take() {
    const bool cut = g_cutThisTick;
    g_cutThisTick = false;
    if (cut) {
        ++g_cutTicks;
    }
    return cut;
}

// ---- CAMERA MODE, measured before it is trusted -------------------------------------------------
// warpPosAndAt does not fire on the plaza run, yet the camera demonstrably cuts (ticks 11, 165, 263,
// each jumping and STAYING — see debug_journal/2026-08-04_interp60_pairing_attribution.md). The next
// candidate signal is CPolarSubCamera::mMode: a mode change is a behavioural discontinuity the game
// declares, and CAMERA_MODE_REPRODUCE_DEMO is a distinct mode in which the camera does not compute
// its own lookat at all.
//
// This MEASURES it and does not act on it. A signal is only a discriminator if it fires on the cut
// ticks AND stays quiet on the rest; asserting that from the class name is how a signal that fires
// every tick gets shipped as a cut detector.
namespace {
constexpr u32 kGpCamera = 0x8040D0A8;   // CPolarSubCamera* gpCamera (US)
constexpr u32 kCamMode = 0x50;          // int mMode
constexpr u32 kCamPrevMode = 0x54;      // int mPrevMode
constexpr u32 kCamInbetween = 0x6C;     // TCameraInbetween*

int g_lastMode = -1;
long g_modeChanges = 0;
long g_ticksObserved = 0;
long g_camMissing = 0;
} // namespace

void sbr_camera_mode_tick(long tick) {
    const u32 camPtr = sb_ram_fast(kGpCamera) ? sb_r32(kGpCamera) : 0;
    if (camPtr == 0 || !sb_ram_fast(camPtr + kCamInbetween)) {
        // Counted, not ignored: a run where the camera pointer is never readable would otherwise
        // report "0 mode changes" and read as "the mode never changed".
        ++g_camMissing;
        return;
    }
    ++g_ticksObserved;
    const int mode = (int)sb_r32(camPtr + kCamMode);
    if (g_lastMode >= 0 && mode != g_lastMode) {
        ++g_modeChanges;
        lucent::info("lerp60", "tick {}: camera mode {} -> {} (prevMode field {})", tick, g_lastMode,
                     mode, (int)sb_r32(camPtr + kCamPrevMode));
    }
    g_lastMode = mode;
}

void sbr_camera_cut_report() {
    if (g_ticksObserved == 0) {
        lucent::warn("lerp60", "camera mode was NEVER readable ({} ticks with no camera pointer), so "
                               "the mode-change count says nothing about the scene.",
                     g_camMissing);
    } else {
        lucent::info("lerp60", "camera mode changed {} times over {} observed ticks ({} ticks had no "
                               "readable camera). Compare these tick numbers against the large "
                               "camera steps: a signal that fires on the cuts AND on many other "
                               "ticks is not a cut detector.",
                     g_modeChanges, g_ticksObserved, g_camMissing);
    }
    // A zero here is a real answer only with its denominator: "no cuts" and "the hook never ran"
    // are the same silence otherwise, and the hook not running is by far the more likely defect.
    lucent::info("lerp60", "camera cuts: {} warpPosAndAt calls produced {} snapped ticks. ZERO calls "
                           "does NOT mean the camera never cut — it means this hook never fired, "
                           "which for a run that moved the camera at all is a defect in the hook, "
                           "not a property of the scene.",
                 g_warpCalls, g_cutTicks);
}

SB_OVERRIDE(0x800335d4u, ov_warp_pos_at, "CPolarSubCamera::warpPosAndAt(const Vec&, const Vec&)",
            "interp60: observe the game's own camera-discontinuity signal so a cut tick snaps "
            "instead of lerping through a viewpoint the game never simulated")
SB_OVERRIDE(0x80033390u, ov_warp_ratio_yaw, "CPolarSubCamera::warpPosAndAt(f32, s16)",
            "interp60: observe the game's own camera-discontinuity signal (ratio/yaw overload)")
