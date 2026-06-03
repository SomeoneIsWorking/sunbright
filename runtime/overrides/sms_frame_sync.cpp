// PC-native replication of SMS's frame-sync waits.
//
// The render loop blocks the main thread on two HW waits, observed via SUNBRIGHT_OSWATCH:
//   VIWaitForRetrace (0x8034f684, lr into it at 8034f6ac) — wait for the next vsync
//   GXDrawDone        (0x8035dae8, lr 8035db40)           — wait for the GPU to finish drawing
// Both park the thread with OSSleepThread on a queue woken by a hardware ISR. Running that on the
// GC scheduler in the recomp hybrid is exactly what we must NOT do (it deadlocks — the scheduler
// can't switch back to the woken main thread; see memory render-state-2026-06 / port-not-emulate).
//
// On a PC port the VI and GP hardware are Dolphin's, so the wait is satisfied by advancing Dolphin's
// CoreTiming by one VI field: the VI presents the frame and the GP FIFO drains. No OSSleepThread, no
// context switch — the main thread just continues. sunbright_wait_vi_field() does the advance.

#include "../overrides.h"

#ifdef HAVE_DOLPHIN_CORE
extern void sunbright_wait_vi_field(CPUState& cpu);   // dolphin_hook.cpp

// VIWaitForRetrace(): replace the retrace-count spin + OSSleepThread with one VI field advanced
// under Dolphin, DELIVERING the interrupts that fire (the VI retrace ISR — which advances the scene
// and bumps the guest retrace counter — plus audio/DSP). See sunbright_wait_vi_field.
SUNBRIGHT_OVERRIDE(ov_VIWaitForRetrace, 0x8034f684u) {
    sunbright_wait_vi_field(cpu);
}

// GXDrawDone(): waits for the GPU to finish. The GP FIFO is drained by the VIWaitForRetrace field
// advance that follows each frame, so return; no OSSleepThread-park on the draw-done token IRQ.
SUNBRIGHT_OVERRIDE(ov_GXDrawDone, 0x8035dae8u) {
    (void)cpu;
}
#endif
