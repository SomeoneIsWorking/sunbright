// native_vi.cpp — VI (video interface) seams.
//
// Retail drives frame pacing from the VI vertical-retrace interrupt, which increments a
// retrace counter and wakes anything sleeping on the retrace queue. This runtime has no
// interrupt source, so the counter never moves and the waiter parks forever.
//
// The decomp+Aurora runtime already settled the shape here (CLAUDE.md, frame seam):
// VIWaitForRetrace is a PURE COUNTER, because the game spins on it from load loops where
// presenting would be wrong. The actual present happens once per frame further up, at
// JDrama::TVideo::waitForRetrace. This override reproduces the counter half; the present
// half belongs with the GX device when it lands.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

namespace {

// VIWaitForRetrace @0x8034f684. Retail, from the disassembly:
//
//   OSDisableInterrupts()
//   old = __VIRetraceCount                 ; SDA global at r13-22768
//   do { OSSleepThread(&__VIRetraceQueue) } ; queue at r13-22760
//   while (__VIRetraceCount == old)
//   OSRestoreInterrupts()
//
// It waits for the counter to CHANGE, so advancing it by one field per call is exactly one
// retrace elapsed — which is what the caller asked for.
void vi_wait_for_retrace(CPUState& cpu) {
    // Addressed off the small-data base rather than hardcoded, so it survives relinking.
    const u32 count = cpu.gpr[13] - 22768;
    if (!sb_ram_fast(count)) {
        lucent::error("vi", "__VIRetraceCount at 0x{:08x} (r13=0x{:08x}) is not in MEM1 — "
                            "r13 is not the small-data base here", count, cpu.gpr[13]);
        std::abort();
    }
    const u32 n = sb_r32(count) + 1;
    sb_w32(count, n);

    // Frame progress is otherwise only inferable from stack sampling. One line per second
    // of emulated video is cheap and tells you immediately whether the game is advancing
    // or spinning in one place.
    if (n % 60 == 0) lucent::debug("vi", "retrace {} ({} s of video)", n, n / 60);
}

} // namespace

SB_OVERRIDE(0x8034f684u, vi_wait_for_retrace, "VIWaitForRetrace",
            "no retrace interrupt source; a pure counter, matching the decomp runtime")
