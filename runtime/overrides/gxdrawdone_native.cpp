// PC-native port of func_8035dae8 — GXDrawDone (GC: "submit a PE draw-done token, then block the
// calling thread until the GPU has processed everything up to it").
//
// THE PROBLEM under native threading: the recomp body writes the PE-finish token to the GP FIFO,
// flushes, then OSSleepThreads on a wait queue ([r13-22428] = 0x8040ea24) until the GPU's PE_FINISH
// interrupt handler sets the done flag ([r13-22432] = 0x8040ea20) and OSWakeupThreads it. nthr owns
// the GC threads and the native idle/IRQ driver does not deliver the Pixel-Engine PE_FINISH
// interrupt — so the flag never flips, the waiter parks forever, and boot deadlocks after the first
// frames (the GXDrawDone in THPPlayerDrawDone's present path).
//
// OWN IT: do the real work — write the same BP-register 0x45 (PE draw-done) token to the gather
// pipe and GXFlush, so the GPU genuinely gets the draw-done — then WAIT for the GPU the PC-native
// way: Dolphin's FifoManager::FlushGpu() drains the GP synchronously (a host-level wait on the real
// GPU thread, NOT a GC OSSleepThread park, so it can't deadlock the cooperative nthr scheduler).
// Then set the done flag (the draw IS done) and return. No GC wait queue, no PE_FINISH interrupt
// dependency. Faithful: same FIFO command submitted, same observable "GPU finished" postcondition.
#include "../overrides.h"
#include "../intrinsics.h"     // MEM_W8/MEM_W32, call_ppc

#ifdef HAVE_DOLPHIN_CORE
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
extern void mem_w8(u32 ea, u8 v);
#endif

namespace {

constexpr u32 GX_FIFO       = 0xCC008000u;   // write-gather pipe
constexpr u32 GXFLUSH       = 0x8035d8f0u;   // GXFlush (recomp)
constexpr u32 DRAWDONE_FLAG = 0x8040ea20u;   // [r13-22432] — 0 while pending, set by the PE-finish ISR

SUNBRIGHT_OVERRIDE(ov_gx_draw_done, 0x8035dae8u) {
    const u32 ret_lr = cpu.lr;

    // Submit the PE draw-done token: BP load of register 0x45 (PE_DONE), value 0x000002 — exactly
    // what the original writes (0x61 = GP "load BP register" opcode, then the 0x45<<24 | 0x2 word).
    MEM_W8 (GX_FIFO, 0x61);
    MEM_W32(GX_FIFO, 0x45000002u);

    // GXFlush — burst the gather pipe into the GP FIFO so the GPU receives the token.
    cpu.lr = 0x8035db1cu;
    call_ppc(cpu, GXFLUSH);

    // Wait for the GPU the PC-native way: drain the GP synchronously (host wait on the GPU thread).
#ifdef HAVE_DOLPHIN_CORE
    Core::System::GetInstance().GetFifo().FlushGpu();
#endif

    // The draw-done is now complete — set the flag the recomp wait loop would have spun on, so any
    // GXDrawDoneEx/poll path elsewhere also sees "done".
    MEM_W8(DRAWDONE_FLAG, 1);

    cpu.lr = ret_lr;   // void return
}

}  // namespace
