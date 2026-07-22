// native_os_thread.cpp — OS threading, backed by the cooperative scheduler (runtime/guest_sched).
//
// The GameCube OS multiplexes threads onto one CPU by context switching: SelectThread picks
// the next thread and OSLoadContext `rfi`s into it, resuming at an arbitrary saved PC. A
// recompiler emitting one C function per guest function cannot re-enter a body partway
// through, so that scheduler can never run here.
//
// Instead each guest thread gets a real host thread and its own CPUState, with exactly one
// token so only one runs at a time — the GameCube is single-core and the game is written for
// it. Blocking becomes a token hand-off, which is what lets a thread park mid-function and
// resume exactly where it left off.
//
// The primitives below are the complete interception surface. Message queues need no
// override of their own: OSSendMessage/OSReceiveMessage block through OSSleepThread and
// OSWakeupThread, which are intercepted here.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>
#include <guest_sched.h>

#include <cstdlib>

// Super-called for faithful guest-struct init. Called directly (not through call_ppc) so it
// bypasses override_lookup and cannot recurse back into us.
extern "C" void func_80348948(CPUState&);   // OSCreateThread

namespace {

// OSThread layout, verified against this DOL by disassembling OSResumeThread @0x80348ee8:
//   lwz r4,716(r29) / stw r0,716(r29)   -> suspend count, s32 at +716
//   lhz r0,712(r29) / cmpwi r0,4        -> state, u16 at +712, WAITING == 4
constexpr u32 T_SUSPEND = 716;

// OSCreateThread(OSThread* r3, entry r4, param r5, stack r6, stackSize r7, prio r8, attr r9)
// It does not reschedule, so the recompiled body runs to completion. Super-calling keeps the
// guest OSThread struct byte-exact (state, priority, links, context, canary, active list) —
// guest code walks those, so fabricating them here would be a slow-burning source of wrong
// behaviour.
void os_create_thread(CPUState& cpu) {
    const u32 os_thread = cpu.gpr[3];
    const u32 entry = cpu.gpr[4], param = cpu.gpr[5], stack = cpu.gpr[6];
    const int prio  = (int)(s32)cpu.gpr[8];

    func_80348948(cpu);
    if (cpu.gpr[3] == 0) return;   // creation failed (bad priority)

    gsched_create(os_thread, entry, param, stack, prio);
    lucent::debug("osthread", "create 0x{:08x} entry 0x{:08x} prio {}", os_thread, entry,
                  prio);
}

// OSResumeThread(OSThread*) -> previous suspend count. Retail decrements, clamps at 0, and
// makes the thread runnable through the scheduler when it reaches 0.
void os_resume_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    const s32 old_suspend = (s32)sb_r32(thread + T_SUSPEND);
    s32 suspend = old_suspend - 1;
    if (suspend < 0) suspend = 0;      // retail clamps; see 0x80348f20..0x80348f2c
    sb_w32(thread + T_SUSPEND, (u32)suspend);
    cpu.gpr[3] = (u32)old_suspend;

    if (suspend == 0) gsched_make_ready(thread);
}

// OSSuspendThread(OSThread*) -> previous suspend count. Suspending SELF parks until resumed.
void os_suspend_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    const s32 old_suspend = (s32)sb_r32(thread + T_SUSPEND);
    sb_w32(thread + T_SUSPEND, (u32)(old_suspend + 1));
    cpu.gpr[3] = (u32)old_suspend;

    // Suspending another thread takes effect at its next scheduling point; suspending
    // yourself parks immediately, which is the GX FIFO back-pressure contract.
    if (thread == gsched_current_os_thread() && old_suspend + 1 > 0) gsched_block(0);
}

// OSSleepThread(OSThreadQueue*) — park the caller on a queue until OSWakeupThread.
void os_sleep_thread(CPUState& cpu) { gsched_block(cpu.gpr[3]); }

// OSWakeupThread(OSThreadQueue*) — everything parked on the queue becomes runnable.
void os_wakeup_thread(CPUState& cpu) { gsched_wake_queue(cpu.gpr[3]); }

// OSYieldThread() — stay runnable, give an equal-or-higher priority thread a turn.
void os_yield_thread(CPUState& cpu) { (void)cpu; gsched_yield(); }

// OSExitThread() — never returns.
void os_exit_thread(CPUState& cpu) {
    (void)cpu;
    gsched_exit();
    lucent::error("osthread", "OSExitThread returned, which cannot happen");
    std::abort();
}

// OSJoinThread(OSThread*, void** result) -> BOOL. Park on a queue keyed by the target;
// gsched_exit wakes exactly that queue.
void os_join_thread(CPUState& cpu) {
    const u32 target = cpu.gpr[3];
    const u32 out    = cpu.gpr[4];      // void** result
    while (!gsched_is_dead(target)) gsched_block(target);
    // Hand back the body's return value. Callers act on it: gameLoop treats
    // TMarDirector::setupThreadFunc's result as stage-load success/failure.
    if (out) sb_w32(out, gsched_exit_value(target));
    cpu.gpr[3] = 1;
}

// OSCancelThread(OSThread*) — the target never runs again. See gsched_cancel for why the
// retail body cannot run here: it unlinks the thread from scheduler queues this runtime does
// not maintain, and faulted on a null queue pointer when the THP player stopped a movie.
void os_cancel_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    gsched_cancel(thread);
    lucent::debug("osthread", "cancel 0x{:08x}", thread);
}

} // namespace

SB_OVERRIDE(0x80348b4cu, os_cancel_thread,  "OSCancelThread",
            "token hand-off: the target is marked dead rather than unlinked from queues "
            "this runtime does not maintain")
SB_OVERRIDE(0x80348948u, os_create_thread,  "OSCreateThread",
            "register with the cooperative scheduler; guest struct init is super-called")
SB_OVERRIDE(0x80348ee8u, os_resume_thread,  "OSResumeThread",
            "retail reschedules via SelectThread, which resumes mid-function")
SB_OVERRIDE(0x80349170u, os_suspend_thread, "OSSuspendThread", "token hand-off")
SB_OVERRIDE(0x803492e0u, os_sleep_thread,   "OSSleepThread",   "token hand-off")
SB_OVERRIDE(0x803493ccu, os_wakeup_thread,  "OSWakeupThread",  "token hand-off")
SB_OVERRIDE(0x8034890cu, os_yield_thread,   "OSYieldThread",   "token hand-off")
SB_OVERRIDE(0x80348a68u, os_exit_thread,    "OSExitThread",    "token hand-off")
SB_OVERRIDE(0x80348d08u, os_join_thread,    "OSJoinThread",    "token hand-off")
