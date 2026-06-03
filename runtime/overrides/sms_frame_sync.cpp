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
extern void sunbright_wait_vi_field();   // dolphin_hook.cpp

// VIWaitForRetrace(): replace the retrace-count spin + OSSleepThread with a one-field CoreTiming
// advance (Dolphin's VI presents the frame). Returns void.
SUNBRIGHT_OVERRIDE(ov_VIWaitForRetrace, 0x8034f684u) {
    (void)cpu;
    sunbright_wait_vi_field();
}

// GXDrawDone(): the GP draw completes within a field; advance one field so Dolphin drains the FIFO,
// then return instead of OSSleepThread-parking on the draw-done token interrupt.
SUNBRIGHT_OVERRIDE(ov_GXDrawDone, 0x8035dae8u) {
    (void)cpu;
    sunbright_wait_vi_field();
}
#endif
