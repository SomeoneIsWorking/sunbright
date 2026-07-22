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
#include <guest_sched.h>

extern void gxfifo_stats(u64&, u64&, u64&);

// Super-called so the guest still programs its own VI exactly as retail does.
extern "C" void func_8034fb4c(CPUState&);   // VIConfigure

// Narrow aurora entry point: tell it the framebuffer size directly. Passing aurora a
// GXRenderModeObj instead would couple this runtime to that struct's layout, and aurora's
// dolphin headers cannot be included here anyway (they redefine the PPC intrinsics).
extern "C" void aurora_vi_set_fb_size(uint32_t width, uint32_t height);

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

    // Retail SLEEPS here for a whole field, and every other runnable thread gets to run
    // during it regardless of priority. Without that the frame loop never yields and the
    // lower-priority setup/loader threads never run at all.
    gsched_drain();

    // Frame progress is otherwise only inferable from stack sampling. One line per second
    // of emulated video is cheap and tells you immediately whether the game is advancing
    // or spinning in one place.
    if (n % 60 == 0) {
        u64 draws = 0, verts = 0, bytes = 0;
        gxfifo_stats(draws, verts, bytes);
        lucent::debug("vi", "retrace {} — GX: {} draws, {} verts, {} KB submitted",
                      n, draws, verts, bytes >> 10);
    }
}

// VIConfigure(const GXRenderModeObj*) @0x8034fb4c.
//
// Aurora keeps its own notion of the configured framebuffer size, and `logical_fb_size()`
// feeds `map_logical_viewport` — every viewport and every EFB-copy rectangle is scaled by
// target/logical. The decomp runtime drives aurora's VI directly, so aurora learns the real
// mode there. This runtime has its OWN VI device (the guest programs MMIO registers), so
// aurora's VI was never configured and silently fell back to its 640x480 default while the
// game renders 640x448 — scaling everything by 960/480 = 2.0 where the oracle uses
// 960/448 = 2.14. Measured as our display copy resolving 1280x896 against the oracle's
// 1280x960.
//
// Nothing here replaces the guest's own VI programming: the recompiled body still runs and
// still drives our VI device. This only forwards the mode to aurora, which has no other way
// to learn it.
void vi_configure(CPUState& cpu) {
    const u32 rm = cpu.gpr[3];

    // Let the guest configure its own VI exactly as retail does.
    func_8034fb4c(cpu);

    if (rm == 0) {
        lucent::error("vi", "VIConfigure(nullptr) — the render mode is required to size the "
                            "framebuffer");
        std::abort();
    }

    // GXRenderModeObj is big-endian in guest memory: +0x04 fbWidth, +0x06 efbHeight.
    // These are exactly the two fields aurora derives its logical framebuffer from.
    const u16 fb_width   = sb_r16(rm + 0x04);
    const u16 efb_height = sb_r16(rm + 0x06);

    if (fb_width == 0 || efb_height == 0) {
        lucent::error("vi", "VIConfigure: render mode at 0x{:08x} has a degenerate framebuffer "
                            "{}x{} — aurora would size every viewport from this",
                      rm, fb_width, efb_height);
        std::abort();
    }

    static u32 last_w = 0, last_h = 0;
    if (fb_width != last_w || efb_height != last_h) {
        last_w = fb_width; last_h = efb_height;
        lucent::info("vi", "render mode {}x{} -> aurora", fb_width, efb_height);
    }
    aurora_vi_set_fb_size(fb_width, efb_height);
}

} // namespace

SB_OVERRIDE(0x8034f684u, vi_wait_for_retrace, "VIWaitForRetrace",
            "no retrace interrupt source; a pure counter, matching the decomp runtime")
SB_OVERRIDE(0x8034fb4cu, vi_configure, "VIConfigure",
            "aurora keeps its own framebuffer size and cannot learn it from our VI device")
