// Loss-proof draw-sync token delivery — native wrapper over TDrawSyncManager::drawSyncCallback.
//
// SMS paces its render pipeline with sequential per-frame GX draw-sync tokens:
// TDrawSyncManager::threadFunc (reference/sms/src/System/DrawSyncManager.cpp) advances its frame
// FIFO and moves the CP breakpoint by COUNTING token-callback messages. Dolphin's PixelEngine
// COALESCES token interrupts — m_token keeps only the LATEST value when several tokens are
// processed before the CPU services the event (free-running dual-core GPU; SyncGPU can batch
// several per slice). One swallowed token permanently wedges the pipeline: breakpoint never
// moves, GPU parks on it, main thread spins in backpressure — the title-screen freeze
// (verified live: BP == next queued boundary, message queue empty, 12 frames outstanding,
// PE token sequential ≈ frame counter; 2026-06-10).
//
// Fix at the seam we own: tokens are sequential u16s, so when the callback observes a JUMP it
// synthesizes the missed intermediate callbacks by super-calling the recomp body once per missed
// value — exactly the calls real hardware would have delivered. Resyncs (scene resets jumping
// backwards/far) fall through after a bounded catch-up and just deliver the current token.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>

extern "C" void func_802a8db8(CPUState&);   // recomp TDrawSyncManager::drawSyncCallback(u16)

SUNBRIGHT_OVERRIDE(ov_drawsync_lossproof, 0x802a8db8u) {
    static u16  last = 0;
    static bool have_last = false;
    const u16 tok = (u16)cpu.gpr[3];
    if (have_last) {
        u16 expect = (u16)(last + 1);
        for (int guard = 0; expect != tok && guard < 64; ++guard, ++expect) {
            CPUState synth = cpu;               // same register file; only the token differs
            synth.gpr[3] = expect;
            func_802a8db8(synth);
            static long logs = 0;
            if (logs++ < 32)
                fprintf(stderr, "[drawsync] synthesized missed token %u (real=%u)\n", expect, tok);
        }
    }
    last = tok;
    have_last = true;
    func_802a8db8(cpu);                         // the real delivery
}
