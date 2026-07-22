// diag_phases.cpp — measure how often the game's per-frame PHASES actually run.
//
// The JDrama perform loop dispatches each view object once per phase, with a bitmask
// selecting the phase (bit 0 movement, bit 3 draw). How many times a phase runs per rendered
// frame is a property of the game loop, and a divergence there advances every actor's update
// at the wrong rate while leaving all rendering state perfectly correct — which is exactly
// what the sea's scroll rate exposed.
//
// Counting is gated on SBR_PHASE_COUNT so this costs nothing unless asked for. It measures
// rather than replaces: the recompiled body is always called.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

extern "C" void func_801dcdd8(CPUState&);   // TMapObjWave::perform(u32 param, TGraphics*)
extern "C" unsigned VIGetRetraceCount(void);   // this runtime's present counter

namespace {

bool counting() {
    static const bool on = std::getenv("SBR_PHASE_COUNT") != nullptr;
    return on;
}

// TMapObjWave::perform @0x801dcdd8. Chosen as the probe because its movement branch advances
// an accumulating texture offset, so its call count is independently checkable against the
// UV values the renderer actually receives.
void wave_perform(CPUState& cpu) {
    if (counting()) {
        const u32 param = cpu.gpr[4];
        static unsigned long move = 0, draw = 0, other = 0, lastFrame = 0;
        if (param & 0x1) ++move;
        else if (param & 0x8) ++draw;
        else ++other;

        // Report per rendered frame, so the numbers are directly comparable with the
        // per-frame UV deltas measured through SB_UV_PROBE.
        const unsigned frame = VIGetRetraceCount();
        if (frame != lastFrame && frame % 100 == 0) {
            lastFrame = frame;
            lucent::info("phase", "after {} presents: wave perform movement={} draw={} other={} "
                                  "({:.2f} movement/frame, {:.2f} draw/frame)",
                         frame, move, draw, other,
                         frame ? (double)move / frame : 0.0,
                         frame ? (double)draw / frame : 0.0);
        }
    }
    func_801dcdd8(cpu);
}

} // namespace

SB_OVERRIDE(0x801dcdd8u, wave_perform, "TMapObjWave::perform (phase counter)",
            "diagnostic only: counts phase dispatches, always calls the real body")
