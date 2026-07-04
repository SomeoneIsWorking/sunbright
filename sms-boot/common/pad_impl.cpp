// pad_impl.cpp — native PC implementation of the GameCube PAD (controller) seam (E8).
//
// Implements the 8 unresolved PAD* symbols. Host input arrives via pad_input.h
// (sb_pad_set_state, fed by the integration layer at VI-retrace cadence); PADRead
// copies the latest snapshot into the game's PADStatus array. PADClamp is the REAL
// GC stick dead-zone / octagonal clamp + trigger clamp, ported verbatim from the
// decomp (src/dolphin/pad/Padclamp.c) — pure and unit-tested. Init/Reset/spec/motor
// are host bookkeeping (no controller HW to drive).

#include <dolphin/pad.h>
#include "pad_input.h"
#include <cstring>
#include <mutex>

namespace {

std::mutex g_pad_mu;
PADStatus g_state[PAD_MAX_CONTROLLERS];   // latest fed snapshot per channel
bool g_inited = false;

// Clamp region constants — identical to the decomp's static ClampRegion.
struct ClampRegionT {
    u8 minTrigger, maxTrigger;
    s8 minStick, maxStick, xyStick;
    s8 minSubstick, maxSubstick, xySubstick;
};
const ClampRegionT kClamp = { 30, 180, 15, 72, 40, 15, 59, 31 };

void clamp_stick(s8* px, s8* py, s8 max, s8 xy, s8 min) {
    int x = *px, y = *py, signX, signY, d;
    if (x >= 0) signX = 1; else { signX = -1; x = -x; }
    if (y >= 0) signY = 1; else { signY = -1; y = -y; }
    x = (x <= min) ? 0 : x - min;
    y = (y <= min) ? 0 : y - min;
    if (x == 0 && y == 0) { *px = *py = 0; return; }
    if ((xy * y) <= (xy * x)) {
        d = xy * x + y * (max - xy);
        if (xy * max < d) { x = (s8)((xy * max * x) / d); y = (s8)((xy * max * y) / d); }
    } else {
        d = xy * y + x * (max - xy);
        if (xy * max < d) { x = (s8)((xy * max * x) / d); y = (s8)((xy * max * y) / d); }
    }
    *px = (s8)(signX * x);
    *py = (s8)(signY * y);
}

void clamp_trigger(u8* t) {
    if (*t <= kClamp.minTrigger) { *t = 0; return; }
    if (kClamp.maxTrigger < *t) *t = kClamp.maxTrigger;
    *t = (u8)(*t - kClamp.minTrigger);
}

} // namespace

extern "C" {

// ---- host-input feed (pad_input.h) ----------------------------------------
void sb_pad_set_state(int chan, const PADStatus* status) {
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS || !status) return;
    std::lock_guard<std::mutex> lk(g_pad_mu);
    g_state[chan] = *status;
}
void sb_pad_set_absent(int chan) {
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS) return;
    std::lock_guard<std::mutex> lk(g_pad_mu);
    std::memset(&g_state[chan], 0, sizeof(PADStatus));
    g_state[chan].err = PAD_ERR_NO_CONTROLLER;
}

// ---- SDK PAD --------------------------------------------------------------
BOOL PADInit(void) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    if (!g_inited) {
        for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
            std::memset(&g_state[i], 0, sizeof(PADStatus));
            g_state[i].err = (i == 0) ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
        }
        g_inited = true;
    }
    return TRUE;
}

int  PADReset(unsigned long /*mask*/) { return TRUE; }
BOOL PADRecalibrate(u32 /*mask*/) { return TRUE; }
void PADSetSpec(u32 /*spec*/) {}
void PADSetAnalogMode(u32 /*mode*/) {}
void PADControlMotor(s32 /*chan*/, u32 /*command*/) {}  // no rumble HW

// Copy the latest fed snapshot for all channels. Returns a connected-channel mask
// (bit per channel, MSB = chan0), matching the SDK's "valid pads" convention.
u32 PADRead(PADStatus* status) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    u32 mask = 0;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        status[i] = g_state[i];
        if (status[i].err == PAD_ERR_NONE) mask |= (0x80000000u >> i);
    }
    return mask;
}

// Real GC PADClamp (Padclamp.c verbatim): clamps all 4 channels' sticks + triggers.
void PADClamp(PADStatus* status) {
    for (int i = 0; i < 4; ++i, ++status) {
        if (status->err == PAD_ERR_NONE) {
            clamp_stick(&status->stickX, &status->stickY, kClamp.maxStick,
                        kClamp.xyStick, kClamp.minStick);
            clamp_stick(&status->substickX, &status->substickY, kClamp.maxSubstick,
                        kClamp.xySubstick, kClamp.minSubstick);
            clamp_trigger(&status->triggerLeft);
            clamp_trigger(&status->triggerRight);
        }
    }
}

} // extern "C"
