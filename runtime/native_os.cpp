#include "native_os.h"
#include "dolphin_hook.h"      // nthrt_spawn_guest / nthrt_make_ready / nthrt_block_current
#include "native_threads.h"    // nthr::ready_count

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

extern u32  mem_r32(u32 ea);          // from memory_bridge
extern void mem_w32(u32 ea, u32 v);
extern void mem_w16(u32 ea, u16 v);

// Reschedule-free recompiled OSCreateThread, super-called for faithful guest-struct init (see
// docs/native_threading.md: it never calls SelectThread/__OSReschedule, so it returns).
extern "C" void func_80348948(CPUState& cpu);
// Recompiled OSSleepThread, used as the hardware-wait fallback when this is the sole runnable
// context: its SelectThread reschedule runs the idle thread under the interpreter, which
// single-steps + delivers the waking IRQ (the proven pre-native-threading path).
extern "C" void func_803492e0(CPUState& cpu);

// ── Guest OSThread / OSThreadQueue layout (confirmed from the recomp) ─────────
namespace {
constexpr u32 OS_CURRENT_THREAD = 0x800000E4u;
// OSThread fields:
constexpr u32 T_STATE   = 712;   // u16: 1=READY, 2=RUNNING, 4=WAITING, 8=MORIBUND
constexpr u32 T_SUSPEND = 716;   // s32: suspend count (>0 ⇒ not runnable)
constexpr u32 T_PRIO    = 720;   // s32: effective priority (lower = higher)
constexpr u32 T_QUEUE   = 732;   // OSThreadQueue* the thread is currently queued on
constexpr u32 T_NEXT    = 736;   // link toward tail
constexpr u32 T_PREV    = 740;   // link toward head
// OSThreadQueue fields: head @+0, tail @+4
constexpr u32 Q_HEAD = 0, Q_TAIL = 4;

inline u16 r_state(u32 th)   { return (u16)(mem_r32(th + T_STATE) >> 16); }  // halfword at +712

// Insert `th` into thread-wait queue `q` ordered by priority — faithful to OSSleepThread's
// insert (same links/head/tail), so guest code that walks the queue still sees the truth.
void wait_queue_insert(u32 q, u32 th) {
    const s32 prio = (s32)mem_r32(th + T_PRIO);
    u32 n = mem_r32(q + Q_HEAD);
    while (n != 0 && (s32)mem_r32(n + T_PRIO) <= prio) n = mem_r32(n + T_NEXT);
    if (n == 0) {                                   // append at tail
        u32 tail = mem_r32(q + Q_TAIL);
        if (tail == 0) mem_w32(q + Q_HEAD, th); else mem_w32(tail + T_NEXT, th);
        mem_w32(th + T_PREV, tail);
        mem_w32(th + T_NEXT, 0);
        mem_w32(q + Q_TAIL, th);
    } else {                                        // insert before n
        u32 prev = mem_r32(n + T_PREV);
        mem_w32(th + T_NEXT, n);
        mem_w32(th + T_PREV, prev);
        mem_w32(n + T_PREV, th);
        if (prev == 0) mem_w32(q + Q_HEAD, th); else mem_w32(prev + T_NEXT, th);
    }
}
}  // namespace

namespace {
std::unordered_map<u32, NativeOSFn> g_native_os;
u32 g_os_lo = 0xFFFFFFFFu;     // [lo, hi) fast-reject range — native_os_lookup is consulted
u32 g_os_hi = 0;              // on every call_ppc and every interpreter step, so reject early

// Native record of every guest OS thread created (step 4a). The matching nthr host thread is
// spawned from this in step 4b (together with OSResumeThread + the blocking primitives, where
// a thread can actually be scheduled and the path is end-to-end testable).
struct GuestThreadRec {
    u32 os_thread;   // guest OSThread* (also the OSCreateThread `thread` arg)
    u32 entry;       // thread function
    u32 param;       // its argument
    u32 stack;       // stack top
    u32 stack_size;
    int priority;
};
std::vector<GuestThreadRec> g_guest_threads;
}  // namespace

void native_os_register(u32 addr, NativeOSFn fn) {
    g_native_os[addr] = fn;
    g_os_lo = std::min(g_os_lo, addr);
    g_os_hi = std::max(g_os_hi, addr + 4);
}

NativeOSFn native_os_lookup(u32 addr) {
    if (addr < g_os_lo || addr >= g_os_hi) return nullptr;   // cheap reject (hot path)
    auto it = g_native_os.find(addr);
    return it == g_native_os.end() ? nullptr : it->second;
}

// ── Native OS primitives (GMSE01) ────────────────────────────────────────────
// OSGetCurrentThread (0x80348368): r3 = *(0x800000E4). Behaviour-identical to the
// recompiled/interpreted version (`lwz r3, 0xE4(0); blr`), so boot is unchanged. This is the
// first genuinely-native primitive — its job is to prove the native-OS set is consulted on
// BOTH the recomp call path and the run_jit_sync interpreter loop, before the load-bearing
// scheduling primitives (OSCreateThread/Resume/Sleep/Wakeup/message queues) are added.
static void os_get_current_thread(CPUState& cpu) {
    cpu.gpr[3] = mem_r32(0x800000E4u);
}

// Is this OSThread one the game created via OSCreateThread (i.e. a worker), as opposed to the
// main/default thread? Used to decide whether a resume must actually schedule it.
static bool is_guest_worker(u32 thread) {
    for (const auto& r : g_guest_threads) if (r.os_thread == thread) return true;
    return false;
}

// OSResumeThread (0x80348ee8) — PC-port replication. Observation (SUNBRIGHT_OSWATCH): every
// OSCreateThread'd worker (JKRThread decomp/stream pool, audio) immediately OSReceiveMessage-waits
// for work and otherwise does nothing; its work is replicated SYNCHRONOUSLY on the caller
// (sms_jkrthread.cpp). So a worker never needs to run: drop its suspend count (keep the game's
// bookkeeping consistent) and return WITHOUT scheduling — no GC scheduler, no context switch. The
// main/default thread (0x800000E4 = 80402aa8) is never an OSCreateThread'd worker, so it is left
// entirely alone and keeps running. (This is why blanket NORESCHED nulled 0x800000E4 and this does
// not: we only skip workers.)
extern "C" void func_80348ee8(CPUState& cpu);   // recomp OSResumeThread (for the non-worker case)
static void os_resume_thread_sync(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    if (is_guest_worker(thread)) {
        const s32 old = (s32)mem_r32(thread + T_SUSPEND);
        mem_w32(thread + T_SUSPEND, (u32)(old - 1));
        cpu.gpr[3] = (u32)old;
        return;
    }
    func_80348ee8(cpu);   // not a worker (not expected during boot) → faithful resume
}

// OSCreateThread (0x80348948): BOOL OSCreateThread(OSThread* r3, func r4, param r5, stack r6,
// stackSize r7, priority r8, attr r9). Reschedule-free, so we super-call the recomp body to
// init the guest struct faithfully (state/priority/links/context/canary + active-thread list),
// then record the thread natively. The host thread itself is spawned in step 4b.
static void os_create_thread(CPUState& cpu) {
    const u32 os_thread = cpu.gpr[3];
    const u32 entry     = cpu.gpr[4];
    const u32 param     = cpu.gpr[5];
    const u32 stack     = cpu.gpr[6];
    const u32 stack_sz  = cpu.gpr[7];
    const int priority  = (int)(s32)cpu.gpr[8];

    func_80348948(cpu);                 // faithful guest-struct init (returns normally)
    if (cpu.gpr[3] == 0) return;        // creation failed (bad priority) — nothing to record

    g_guest_threads.push_back({os_thread, entry, param, stack, stack_sz, priority});
    fprintf(stderr,
        "[native_os] OSCreateThread #%zu  OSThread=%08x entry=%08x param=%08x stack=%08x "
        "size=%u prio=%d  (cur=%08x)\n",
        g_guest_threads.size(), os_thread, entry, param, stack, stack_sz, priority,
        mem_r32(OS_CURRENT_THREAD));
}

// OSResumeThread (0x80348ee8): decrement the suspend count; when it reaches 0 and the thread is
// READY, make the mapped host thread runnable. (Genuinely native — the recomp body reschedules
// via SelectThread, which we never run.) Returns the previous suspend count in r3.
static void os_resume_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    const s32 old_suspend = (s32)mem_r32(thread + T_SUSPEND);
    const s32 suspend = old_suspend - 1;
    mem_w32(thread + T_SUSPEND, (u32)suspend);
    if (suspend <= 0 && r_state(thread) == 1 /*READY*/)
        nthrt_make_ready(thread);
    fprintf(stderr, "[native_os] OSResumeThread %08x suspend %d->%d state=%u%s\n",
            thread, old_suspend, suspend, r_state(thread),
            (suspend <= 0 && r_state(thread) == 1) ? " -> READY" : "");
    cpu.gpr[3] = (u32)old_suspend;
}

// OSSleepThread (0x803492e0): enqueue the current thread on wait-queue r3 (set state=WAITING,
// thread->queue), then park until OSWakeupThread wakes it. Replaces the recomp body's
// SelectThread reschedule with a native host-thread park.
static void os_sleep_thread(CPUState& cpu) {
    if (nthr::ready_count() == 0) {
        // Sole runnable context → this is a hardware wait, not a thread-to-thread switch. Defer
        // to the proven path: the recomp body reschedules to the idle thread, which the
        // interpreter single-steps, delivering the waking IRQ. (docs step 6 replaces this with a
        // native idle/driver so even hardware waits never touch the interpreter.)
        func_803492e0(cpu);
        return;
    }
    const u32 queue  = cpu.gpr[3];
    const u32 thread = mem_r32(OS_CURRENT_THREAD);
    nthrt_bind_current(thread);            // ensure OSWakeupThread can resolve us (esp. thread 0)
    mem_w16(thread + T_STATE, 4);          // WAITING
    mem_w32(thread + T_QUEUE, queue);
    wait_queue_insert(queue, thread);
    nthrt_block_current();                 // yield to another ready thread (Undeclare/park/Declare)
    // Woken: OSWakeupThread already dequeued us and set state=READY. Return (void).
}

// OSWakeupThread (0x803493cc): dequeue every thread on wait-queue r3, set state=READY, and make
// the runnable ones (suspend<=0) Ready in nthr. Replaces the recomp body's run-queue insert +
// SelectThread with native make_ready.
static void os_wakeup_thread(CPUState& cpu) {
    const u32 queue = cpu.gpr[3];
    for (;;) {
        u32 th = mem_r32(queue + Q_HEAD);
        if (th == 0) break;
        u32 next = mem_r32(th + T_NEXT);   // dequeue head
        if (next != 0) mem_w32(next + T_PREV, 0); else mem_w32(queue + Q_TAIL, 0);
        mem_w32(queue + Q_HEAD, next);
        mem_w16(th + T_STATE, 1);          // READY
        if ((s32)mem_r32(th + T_SUSPEND) <= 0)
            nthrt_make_ready(th);
    }
}

// DIAGNOSTIC (SUNBRIGHT_DBG_CONSOLE): surface the GC debug console __write_console(?, buf=r4,
// len=r5) — where the game / JUTException prints its crash report; Dolphin doesn't capture it.
// OBSERVES (super-calls the real recompiled body), so behaviour is unchanged. Kept permanently.
extern u8 mem_r8(u32 ea);
extern "C" void func_8033ba90(CPUState& cpu);   // __write_console (recomp body)
extern u32 mem_r32(u32 ea);
static void dbg_write_console(CPUState& cpu) {
    // __write_console(chan=r3, buf=r4, len*=r5): r5 points to the byte count (writes char-by-char).
    const u32 buf = cpu.gpr[4];
    const u32 len = cpu.gpr[5] ? mem_r32(cpu.gpr[5]) : 0;
    static char s[1024];
    u32 n = len < sizeof(s) - 1 ? len : sizeof(s) - 1;
    for (u32 i = 0; i < n; i++) s[i] = (char)mem_r8(buf + i);
    s[n] = 0;
    fprintf(stderr, "%s", s);   // raw console stream (noise OK; grep/python post-process)
    func_8033ba90(cpu);         // run the real __write_console (observe, don't replace)
}

void native_os_init() {
    static bool done = false;
    if (done) return;
    done = true;
    // The native-OS *interception seam* (native_os_lookup, consulted by call_ppc AND under the
    // run_jit_sync interpreter) is kept — it's how a native override reaches a function even when
    // it runs under the interpreter. OSGetCurrentThread is behaviour-identical, harmless, and
    // exercises the seam.
    native_os_register(0x80348368u, os_get_current_thread);
    // PC-port threading replication: track the game's worker threads at creation, and keep them
    // dormant at resume (their work is done synchronously — see sms_jkrthread.cpp). No GC scheduler,
    // no context switch; the main/default thread keeps running. See os_resume_thread_sync above.
    native_os_register(0x80348948u, os_create_thread);     // record workers (super-calls real body)
    native_os_register(0x80348ee8u, os_resume_thread_sync); // dormant worker / faithful main resume
    // ── Diagnostics (env-gated, kept permanently — see memory keep-diagnostics) ──
    if (getenv("SUNBRIGHT_DBG_CONSOLE"))
        native_os_register(0x8033ba90u, dbg_write_console);   // surface GC console / crash report
    // The GC-thread SCHEDULER emulation (OSCreateThread/Resume/Sleep/Wakeup + nthr) is the
    // wrong layer for a PORT (it reimplements emulator internals). Disabled — see
    // memory port-not-emulate + docs/native_threading.md. The game runs the GC threading
    // faithfully (as before this effort) and stalls at audio init, which is fixed instead by a
    // native subsystem override (the PC-native replicate-the-behaviour approach), registered below
    // as it is built. (void)s keep the implementations linked for reference / future reuse.
    (void)&os_create_thread; (void)&os_resume_thread;
    (void)&os_sleep_thread;  (void)&os_wakeup_thread;
    fprintf(stderr, "[native_os] registered %zu native OS primitive(s) over [%08x,%08x)\n",
            g_native_os.size(), g_os_lo, g_os_hi);
}
