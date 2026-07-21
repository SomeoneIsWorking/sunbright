// native_gx.cpp — GX seams where retail waits on the GPU.
//
// The GameCube GPU runs asynchronously: the CPU pushes commands into a FIFO and the
// pipeline drains them behind it, so the SDK has blocking primitives that park the calling
// thread until the GPU catches up. This port renders SYNCHRONOUSLY — the FIFO is drained
// and presented inside the frame seam, the same model the decomp runtime uses — so by the
// time the game asks "is the GPU done?", it is. There is nothing to wait for.
//
// This is the same reasoning as synchronous ARAM DMA (dev_aram.cpp): the host has no
// latency to hide, so inventing some and then building machinery to wait for it would be
// backwards.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

namespace {

// GXDrawDone @0x8035dae8. Retail, from the disassembly:
//
//   OSDisableInterrupts()
//   *(u8*)0xCC008000  = 0x61            ; PE draw-sync token, via the write-gather pipe
//   *(u32*)0xCC008000 = 0x45000002
//   GXFlush()
//   __GXDrawDoneFlag = 0                ; SDA global at r13-22432
//   OSRestoreInterrupts()
//   while (__GXDrawDoneFlag == 0)       ; set by the PE token interrupt handler
//       OSSleepThread(&__GXDrawDoneQueue)
//
// Without a GPU or an interrupt path the flag is never set and the main thread parks
// forever — this is where boot stopped.
//
// The token is still pushed, byte-for-byte as retail does, so the FIFO command stream stays
// faithful for when the GX device lands and aurora consumes it. What we skip is only the
// WAIT, and we set the flag the token interrupt would have set. Once a real GX device and
// interrupt delivery exist this override should be re-evaluated: at that point the honest
// implementation is for the token to complete the moment the FIFO is drained.
void gx_draw_done(CPUState& cpu) {
    // The flag is addressed off the small-data base rather than hardcoded, so this stays
    // correct regardless of where the linker put it.
    const u32 flag = cpu.gpr[13] - 22432;
    if (!sb_ram_fast(flag)) {
        lucent::error("gx", "__GXDrawDoneFlag at 0x{:08x} (r13=0x{:08x}) is not in MEM1 — "
                            "r13 is not the small-data base here", flag, cpu.gpr[13]);
        std::abort();
    }

    sb_w8 (0xCC008000u, 0x61u);          // PE draw-sync token opcode
    sb_w32(0xCC008000u, 0x45000002u);

    sb_w8(flag, 1);                      // what the PE token interrupt handler would do
}

} // namespace

SB_OVERRIDE(0x8035dae8u, gx_draw_done, "GXDrawDone",
            "rendering is synchronous in this port, so the GPU is already done; retail parks "
            "until a PE token interrupt that has no source here")
