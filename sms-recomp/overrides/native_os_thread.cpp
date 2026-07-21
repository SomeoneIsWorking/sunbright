// native_os_thread.cpp — OS threading overrides (increment 1 of the OSThread arc).
//
// The GameCube OS multiplexes software threads onto one CPU by context switching:
// SelectThread picks the next thread and OSLoadContext `rfi`s into it, resuming at an
// arbitrary saved PC. A recompiler that emits one C function per guest function cannot
// express that — there is no way to re-enter func_803486dc partway through its body. The
// standalone boot aborts on exactly this: OSResumeThread -> SelectThread -> resume at
// SelectThread+0x104.
//
// The port's answer is the ONE RUNTIME rule (CLAUDE.md): a GC thread's work runs INLINE at
// its enqueue site, so the thread itself is never needed and the guest scheduler is never
// entered. That is the same reasoning as synchronous ARAM DMA and synchronous GXDrawDone —
// the host has no latency to hide, so there is nothing to overlap and nothing to schedule.
// (An earlier draft of this file proposed porting a cooperative token scheduler instead.
// That was wrong and inconsistent with every other seam in this runtime.)
//
// THIS INCREMENT DOES NOT SCHEDULE ANYTHING YET. It installs the seam — recording created
// threads and keeping the guest's own bookkeeping exact — and makes a resume return
// without rescheduling. That is enough for boot, where the only thread created is
// JUTException's, whose body parks forever waiting for a fault that a PC port surfaces
// natively anyway. It is NOT enough in general: a worker whose body never runs is a real
// behavioural gap, so every skipped resume says so out loud rather than looking like it
// worked. When one is, the fix is to make that work synchronous at its enqueue site.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <unordered_map>
#include <vector>

// Recompiled bodies we super-call for faithful guest-struct init. Calling them directly
// (not through call_ppc) is deliberate: it bypasses override_lookup, so there is no
// recursion back into us.
extern "C" void func_80348948(CPUState&);   // OSCreateThread
extern "C" void func_80348ee8(CPUState&);   // OSResumeThread

namespace {

// OSThread layout. VERIFIED against this DOL by disassembling OSResumeThread @0x80348ee8:
//   lwz r4,716(r29) / stw r0,716(r29)   -> suspend count is a 32-bit field at +716
//   lhz r0,712(r29) / cmpwi r0,4        -> state is a 16-bit field at +712, WAITING == 4
constexpr u32 T_STATE   = 712;   // u16: 1=READY 2=RUNNING 4=WAITING 8=MORIBUND
constexpr u32 T_SUSPEND = 716;   // s32: suspend count; > 0 means not runnable
constexpr u32 T_PRIO    = 720;   // s32: effective priority (LOWER number = higher priority)

struct ThreadRec {
    u32 entry, param, stack, stack_size;
    int priority;
    bool body_ran;
};

// Keyed by guest OSThread*. Not a set: the entry point identifies WHICH worker went
// unscheduled, which is what names the enqueue site that has to become synchronous.
std::unordered_map<u32, ThreadRec>& threads() {
    static std::unordered_map<u32, ThreadRec> t;
    return t;
}

// OSCreateThread @0x80348948:
//   BOOL OSCreateThread(OSThread* r3, void* entry r4, void* param r5, void* stack r6,
//                       u32 stackSize r7, OSPriority r8, u16 attr r9)
// It does not reschedule, so the recompiled body runs to completion and returns normally.
// Super-calling it keeps the guest OSThread struct byte-exact (state, priority, links,
// context, stack canary, active-thread list) — guest code walks those, so fabricating them
// here would be a slow-burning source of wrong behaviour.
void os_create_thread(CPUState& cpu) {
    const u32 os_thread = cpu.gpr[3];
    const ThreadRec rec{cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7],
                        (int)(s32)cpu.gpr[8], false};

    func_80348948(cpu);
    if (cpu.gpr[3] == 0) return;   // creation failed (bad priority) — nothing to record

    threads()[os_thread] = rec;
    lucent::info("osthread", "OSCreateThread OSThread=0x{:08x} entry=0x{:08x} param=0x{:08x} "
                             "stack=0x{:08x} prio={}",
                 os_thread, rec.entry, rec.param, rec.stack, rec.priority);
}

// OSResumeThread @0x80348ee8: s32 OSResumeThread(OSThread*) — returns the PREVIOUS suspend
// count. Retail decrements, clamps at 0, and when the count reaches 0 makes the thread
// runnable via the scheduler. We reproduce the bookkeeping exactly (verified against the
// disassembly, including the clamp) and stop before the scheduling.
void os_resume_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];

    auto it = threads().find(thread);
    if (it == threads().end()) {
        // Not a thread the game created — the default/main thread. It is already running,
        // so a faithful resume is safe and is what retail does.
        func_80348ee8(cpu);
        return;
    }

    const s32 old_suspend = (s32)sb_r32(thread + T_SUSPEND);
    s32 suspend = old_suspend - 1;
    if (suspend < 0) suspend = 0;          // retail clamps; see 0x80348f20..0x80348f2c
    sb_w32(thread + T_SUSPEND, (u32)suspend);
    cpu.gpr[3] = (u32)old_suspend;

    if (suspend == 0 && !it->second.body_ran) {
        it->second.body_ran = true;   // announce once per thread, not once per resume
        lucent::warn("osthread",
                     "thread 0x{:08x} (entry 0x{:08x}, prio {}) became runnable but is NOT "
                     "being scheduled — its body will not run. Fine for a thread that only "
                     "parks (JUTException). If a worker's output is ever actually needed, the "
                     "fix is to make its ENQUEUE point synchronous, not to add a scheduler.",
                     thread, it->second.entry, it->second.priority);
    }
}

} // namespace

SB_OVERRIDE(0x80348948u, os_create_thread, "OSCreateThread",
            "record the thread so an unscheduled worker is identifiable; guest struct init "
            "is super-called")
SB_OVERRIDE(0x80348ee8u, os_resume_thread, "OSResumeThread",
            "retail reschedules via SelectThread, which resumes mid-function and cannot be "
            "recompiled; bookkeeping is reproduced, scheduling is not")
