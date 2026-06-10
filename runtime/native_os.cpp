#include "native_os.h"
#include "dolphin_hook.h"      // nthrt_spawn_guest / nthrt_make_ready / nthrt_block_current
#include "native_threads.h"    // nthr::ready_count

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>

extern u32  mem_r32(u32 ea);          // from memory_bridge
extern u16  mem_r16(u32 ea);
extern void mem_w32(u32 ea, u32 v);
extern void mem_w16(u32 ea, u16 v);
extern void call_ppc(CPUState& cpu, u32 address);   // intrinsics.h (run a recomp callee inline)
extern bool g_in_poll_yield;          // spin-loop detector suppressor (memory_bridge)

namespace {
// Runtime-internal guest-RAM reads must not feed the guest spin-loop detector: the retrace
// wakeup reads the SAME queue-head EA every frame, accumulating "consecutive identical reads"
// across calls until sb_poll_fire ran the IRQ pump mid-primitive on an interp thread's context
// (wild branch to 0x58, 2026-06-10). Same defect class as the nthr ctx_save raw-read fix.
struct PollSuppress {
    bool prev;
    PollSuppress() : prev(g_in_poll_yield) { g_in_poll_yield = true; }
    ~PollSuppress() { g_in_poll_yield = prev; }
};
}  // namespace

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
    PollSuppress ps;
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
extern void sunbright_dbg_log_active_threads(const char* tag);   // dolphin_hook.cpp
static void os_get_current_thread(CPUState& cpu) {
    cpu.gpr[3] = mem_r32(0x800000E4u);
    // Lazy takeover-time adoption: the adopt point runs before OS thread-init (queue empty), so the
    // first time the active-thread queue is populated, register all the GC threads the OS already
    // created into the nthr registry. Idempotent (std::call_once inside); inert until the native
    // scheduling primitives are enabled.
    sunbright_adopt_all_gc_threads();
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
    // Spawn the matching nthr host thread, PARKED (created threads start suspend=1; OSResumeThread
    // makes it Ready). Registers OSThread*↔nthr in g_os_to_gt (so the lazy adoption re-scan skips it).
    nthrt_spawn_guest(os_thread, entry, param, stack, priority);
    static const bool dbg = getenv("SUNBRIGHT_DBG_ADOPT") != nullptr;
    if (dbg) fprintf(stderr,
        "[native_os] OSCreateThread #%zu  OSThread=%08x entry=%08x param=%08x stack=%08x prio=%d\n",
        g_guest_threads.size(), os_thread, entry, param, stack, priority);
}

// OSResumeThread (0x80348ee8): decrement the suspend count; when it reaches 0 and the thread is
// READY, make the mapped host thread runnable. (Genuinely native — the recomp body reschedules
// via SelectThread, which we never run.) Returns the previous suspend count in r3.
static void os_resume_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    const s32 old_suspend = (s32)mem_r32(thread + T_SUSPEND);
    const s32 suspend = old_suspend - 1;
    mem_w32(thread + T_SUSPEND, (u32)suspend);
    const bool made_ready = (suspend <= 0 && r_state(thread) == 1 /*READY*/);
    if (made_ready)
        nthrt_make_ready(thread);
    fprintf(stderr, "[native_os] OSResumeThread %08x suspend %d->%d state=%u%s\n",
            thread, old_suspend, suspend, r_state(thread),
            made_ready ? " -> READY" : "");
    cpu.gpr[3] = (u32)old_suspend;
    // Priority preemption (GC __OSReschedule semantics, run inside OSResumeThread): if the thread
    // we just made runnable is STRICTLY higher priority than the caller, the GC scheduler would
    // switch to it now. Our scheduler is otherwise cooperative (no preemption), so the caller would
    // race ahead — exactly the JASystem audio-thread bug: AudioThread::start creates+resumes the
    // higher-priority audio thread (which runs Driver::init -> initBuffer to ALLOCATE the DSP FX
    // buffers) and returns WITHOUT waiting, relying on this preemption; without it the main thread
    // reaches JAIData::initData and configures FX lines that were never allocated -> null write.
    // Yield (stay Ready); the scheduler picks the higher-prio thread, and the token returns here
    // when it next blocks. Lower priority NUMBER = higher priority (GC convention). [[native-threading-plan]]
    if (made_ready) {
        const u32 cur = mem_r32(OS_CURRENT_THREAD);
        if (cur && cur != thread &&
            (s32)mem_r32(thread + T_PRIO) < (s32)mem_r32(cur + T_PRIO))
            nthrt_yield_current(&cpu);
    }
}

// OSSuspendThread (0x80349170): increment the suspend count; suspending SELF parks the host
// thread until OSResumeThread drops the count back and makes it Ready. This is the GX FIFO
// pacing contract: at the CP hi watermark __GXOverflowHandler suspends the PUSHING thread, and
// the lo-watermark (underflow) handler resumes it once the GPU drains. Without this port the
// suspend was a recomp no-op (its reschedule never ran), the pusher never stopped, and the FIFO
// overflowed by a full lap ("FIFO is overflowed by GatherPipe!", 2026-06-10). Returns the
// previous suspend count in r3 like the real function.
static void os_suspend_thread(CPUState& cpu) {
    const u32 thread = cpu.gpr[3];
    const s32 old_suspend = (s32)mem_r32(thread + T_SUSPEND);
    mem_w32(thread + T_SUSPEND, (u32)(old_suspend + 1));
    const u32 cur = mem_r32(OS_CURRENT_THREAD);
    static long logs = 0;
    if (logs++ < 64)
        fprintf(stderr, "[native_os] OSSuspendThread %08x suspend %d->%d (cur=%08x)\n",
                thread, old_suspend, old_suspend + 1, cur);
    cpu.gpr[3] = (u32)old_suspend;
    if (thread == cur && old_suspend + 1 > 0) {
        nthrt_bind_current(thread);
        mem_w16(thread + T_STATE, 1);      // READY: runnable again the moment the count drops
        nthrt_block_current();             // park until OSResumeThread make_ready's us
    }
    // Suspending ANOTHER thread: cooperative nthr cannot preempt it mid-run; the recorded count
    // takes effect at its next scheduling point (the GX use-case is always self-suspend).
}

// OSSleepThread (0x803492e0): enqueue the current thread on wait-queue r3 (set state=WAITING,
// thread->queue), then park until OSWakeupThread wakes it. Replaces the recomp body's
// SelectThread reschedule with a native host-thread park.
unsigned long g_ds_sleeps = 0, g_ds_wakes = 0;   // drawsync diag: the TDrawSyncManager queue
static constexpr u32 kDrawSyncQueue = 0x805761E0u; // (stable heap addr this build; diagnostic only)

extern "C" void sb_trace(const char* tag, u32 a, u32 b, u32 c, u32 d);
static void os_sleep_thread(CPUState& cpu) {
    // Always native-park (the GC-scheduler fallback is gone — it was the "two schedulers" conflict).
    // When this is the sole runnable context (a hardware wait), nthr's idle handler advances device
    // timing so the waking IRQ's ISR calls native OSWakeupThread → make_ready (the native idle/IRQ
    // driver). [[done-right-over-working]]: one path, no recompiled-scheduler fallback.
    const u32 queue  = cpu.gpr[3];
    const u32 thread = mem_r32(OS_CURRENT_THREAD);
    static const bool dbg = getenv("SUNBRIGHT_DBG_SCHED") != nullptr;
    if (dbg) fprintf(stderr, "[sched] SLEEP  thread=%08x on queue=%08x (lr=%08x) ready=%d\n",
                     thread, queue, cpu.lr, nthr::ready_count());
    if (queue == kDrawSyncQueue) g_ds_sleeps++;
    static const bool trace_q2 = getenv("SUNBRIGHT_DBG_SCHED") != nullptr;
    if (trace_q2) sb_trace("sleepq", queue, thread, cpu.lr, 0);
    nthrt_bind_current(thread);            // ensure OSWakeupThread can resolve us (esp. thread 0)
    mem_w16(thread + T_STATE, 4);          // WAITING
    mem_w32(thread + T_QUEUE, queue);
    wait_queue_insert(queue, thread);
    nthrt_block_current();                 // yield to another ready thread (Undeclare/park/Declare)
    // Woken: OSWakeupThread already dequeued us and set state=READY. Return (void).
}

// Dequeue every thread on wait-queue `queue`, set state=READY, and make the runnable ones
// (suspend<=0) Ready in nthr — the body of OSWakeupThread, factored so the thread-exit
// bookkeeping can also wake a thread's join queue.
static void wake_queue(u32 queue) {
    if (queue == kDrawSyncQueue) g_ds_wakes++;
    static const bool trace_q = getenv("SUNBRIGHT_DBG_SCHED") != nullptr;   // hot path: gated
    if (trace_q) sb_trace("wakeq", queue, mem_r32(queue + Q_HEAD), 0, 0);
    PollSuppress ps;   // see PollSuppress: internal queue walks must not feed the poll detector
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

// OSWakeupThread (0x803493cc): dequeue every thread on wait-queue r3, set state=READY, and make
// the runnable ones (suspend<=0) Ready in nthr. Replaces the recomp body's run-queue insert +
// SelectThread with native make_ready.
static void os_wakeup_thread(CPUState& cpu) {
    const u32 queue = cpu.gpr[3];
    static const bool dbg = getenv("SUNBRIGHT_DBG_SCHED") != nullptr;
    if (dbg) fprintf(stderr, "[sched] WAKEUP queue=%08x head=%08x (lr=%08x)\n",
                     queue, mem_r32(queue + Q_HEAD), cpu.lr);
    wake_queue(queue);
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

// OSYieldThread (0x8034890c): give up the CPU to the next runnable thread of equal-or-higher
// priority (round-robin among equals), then resume. The recomp/JIT body does this via the GC
// SelectThread — which, under native threading, finds NO runnable GC thread (nthr owns them; the GC
// run-queue is empty), sets OS_CURRENT_THREAD=0, and spins the GC idle loop (SelectThread+0x138)
// forever. Engine code calls it: the audio DSP init `802c6830` (JIT-only, run under run_jit_sync)
// yields here after kicking the DSP, and the GC idle spin is what the run_jit_sync step-budget abort
// reports. Native equivalent: yield the token but stay Ready (nthr's pick_next then picks the
// highest-priority Ready thread, FIFO among ties = round-robin; returns to us when we're highest).
// This keeps engine code OUT of run_jit_sync scheduler spins WITHOUT recompiling it. [[no-run_jit_sync-crutch]]
static void os_yield_thread(CPUState& cpu) {
    nthrt_yield_current(&cpu);
}

// __OSReschedule (0x803488dc): the GC scheduler's "switch to the highest-priority ready thread"
// entry. With nthr owning scheduling, this must NOT context-switch (two schedulers over one global
// ppc = the documented conflict). No-op → returns to the caller; nthr switches at native block
// points instead (cooperative). The run-queue bookkeeping it skips is unused (nthr owns readiness).
static void os_reschedule_noop(CPUState& /*cpu*/) {}

// ── Native OSExitThread bookkeeping (the piece nthr was skipping) ─────────────────────────────
// When a guest thread function returns, nthr stops AT the exit trampoline (0x80348a68 =
// OSExitThread) without running it, so the OSThread was never marked MORIBUND and its join queue
// never woken — so OSIsThreadTerminated / OSJoinThread (still recompiled, both correct once the
// state is set) never saw the thread finish. The boot sequencer (802a5f50) waits on exactly that:
// OSIsThreadTerminated(loader) -> OSJoinThread, gating the per-scene director creation; without it
// the state machine never advances and a later state derefs the null/garbage director -> crash.
//
// This mirrors OSExitThread (func_80348a68) MINUS its final GC SelectThread reschedule — nthr owns
// scheduling, so we must not run the GC scheduler (the two-schedulers conflict). The sub-calls we
// keep are pure cleanup: 0x803440c4 resets the thread's owned-mutex queue; __OSUnlockAllMutex
// (0x803468b4) releases its mutexes (waking waiters via the now-native OSWakeupThread). Operates on
// the explicit os_thread (not OS_CURRENT_THREAD) since that is who exited.
namespace {
constexpr u32 T_DETACH   = 714;   // u16: bit0 = detached
constexpr u32 T_VAL      = 728;   // void* exit value
constexpr u32 T_JOINQ    = 744;   // OSThreadQueue queueJoin (head@+0, tail@+4)
constexpr u32 T_ACT_PREV = 764;   // active-thread-queue link toward head
constexpr u32 T_ACT_NEXT = 768;   // active-thread-queue link toward tail
constexpr u32 ACT_HEAD   = 0x800000E0u;  // OS active-thread-queue head ptr
constexpr u32 ACT_TAIL   = 0x800000DCu;  // OS active-thread-queue tail ptr
}
void native_os_thread_exit(CPUState& cpu, u32 thread, u32 exit_val) {
    // 0x803440c4(thread): reset the dying thread's owned-mutex queue (faithful pre-step).
    cpu.gpr[3] = thread; cpu.lr = 0x80348a8cu; call_ppc(cpu, 0x803440c4u);

    if (mem_r16(thread + T_DETACH) & 1) {
        // Detached: nobody will join — unlink from the active-thread queue and free (state=0).
        u32 prev = mem_r32(thread + T_ACT_PREV), next = mem_r32(thread + T_ACT_NEXT);
        if (prev == 0) mem_w32(ACT_HEAD, next); else mem_w32(prev + T_ACT_NEXT, next);
        if (next == 0) mem_w32(ACT_TAIL, prev); else mem_w32(next + T_ACT_PREV, prev);
        mem_w16(thread + T_STATE, 0);
    } else {
        // Joinable: mark MORIBUND and publish the exit value; the joiner reaps it (OSJoinThread).
        mem_w16(thread + T_STATE, 8);          // MORIBUND
        mem_w32(thread + T_VAL, exit_val);
    }

    // __OSUnlockAllMutex(thread): release held mutexes (wakes waiters via native OSWakeupThread).
    cpu.gpr[3] = thread; cpu.lr = 0x80348b00u; call_ppc(cpu, 0x803468b4u);

    // OSWakeupThread(&thread->queueJoin): release anyone blocked in OSJoinThread on this thread.
    wake_queue(thread + T_JOINQ);

    static const bool dbg = getenv("SUNBRIGHT_DBG_SCHED") != nullptr;
    if (dbg) fprintf(stderr, "[sched] EXIT   thread=%08x val=%08x detach=%u -> state=%u\n",
                     thread, exit_val, mem_r16(thread + T_DETACH) & 1, r_state(thread));
}

// Direct OSExitThread(r3 = exit value) calls — e.g. JASystem::AudioThread::audioproc on the THP
// movie's stop message. The recompiled body must NOT run: it stores through real-mode low memory
// (ea 0xE4 → wild-write trap) and ends in the GC SelectThread reschedule (the two-schedulers
// conflict). Do the native bookkeeping, then park this host thread forever — the guest thread is
// dead and never made ready again. (A later AudioThread::start() re-creates via OSCreateThread,
// which spawns its own fresh host thread.)
static void os_exit_thread_direct(CPUState& cpu) {
    const u32 cur = mem_r32(OS_CURRENT_THREAD);
    fprintf(stderr, "[native_os] OSExitThread(thread=%08x val=%08x) — native exit + park\n",
            cur, cpu.gpr[3]);
    native_os_thread_exit(cpu, cur, cpu.gpr[3]);
    for (;;) nthrt_block_current();        // dead: parked, never woken
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
    // ── Native threading: nthr OWNS the GC scheduler (docs/native_threading.md). ──────────────
    // The lifecycle + blocking primitives run on the nthr host-thread substrate (one host thread per
    // guest thread, single CPU token, condvar park). Registered on the native_os seam so they
    // intercept on BOTH the recomp call path and the run_jit_sync interpreter.
    native_os_register(0x80348948u, os_create_thread);   // OSCreateThread → init + spawn parked nthr
    native_os_register(0x80348ee8u, os_resume_thread);   // OSResumeThread → make_ready when runnable
    native_os_register(0x80349170u, os_suspend_thread);  // OSSuspendThread → self-suspend parks (GX FIFO pacing)
    native_os_register(0x803492e0u, os_sleep_thread);    // OSSleepThread  → enqueue + nthr park
    native_os_register(0x803493ccu, os_wakeup_thread);   // OSWakeupThread → dequeue + make_ready
    // Neutralize the GC scheduler's context-switch: nthr switches at native block points, so the
    // recompiled __OSReschedule must NOT also switch (the "two schedulers over one ppc" conflict).
    // No-op = cooperative (never preempt); a woken higher-prio thread runs at the next nthr yield.
    native_os_register(0x803488dcu, os_reschedule_noop); // __OSReschedule → no switch
    native_os_register(0x8034890cu, os_yield_thread);    // OSYieldThread → nthr yield (stay Ready)
    native_os_register(0x80348a68u, os_exit_thread_direct); // OSExitThread → native exit + park
    // ── Native DVD read service: own the file-read path (docs/native_threading.md frontier). ──
    // The GC DVD command FSM stalls after the first transfer under cooperative native scheduling;
    // service reads directly from our own DiscIO::Volume instead. Registered on this same seam so
    // it fires for JIT-only threads running under the interpreter (e.g. the audio thread).
    extern void native_dvd_register();
    native_dvd_register();
    // ── Native ARAM transfer service: synchronous ARQPostRequest (boot determinism — every
    // archive/audio-wave ARAM wait completes immediately; no AR DMA interrupt, no timing). ──
    extern void native_aram_register();
    native_aram_register();
    // ── Native memory-card service: own the CARD hardware layer (EXI DMA completions were lost
    // under the hybrid timing → CARDMount wedge → dead file-select menu, 2026-06-10). ──
    extern void native_card_register();
    native_card_register();
    // ── Native PPCSync: `sc; blr` store barrier → no-op (gather-pipe writes are synchronous
    // here; the interpreted syscall round-trip was the GXFlush per-frame choke). ──
    extern void native_ppcsync_register();
    native_ppcsync_register();
    // ── Native DVD→ARAM ripper: JKRDvdAramRipper::loadToAram served host-side (read + Yaz0 +
    // guest-heap alloc + ARAM memcpy) — the stream-thread pipeline never completed under nthr. ──
    extern void native_aram_ripper_register();
    native_aram_ripper_register();
    // ── Diagnostics (env-gated, kept permanently — see memory keep-diagnostics) ──
    if (getenv("SUNBRIGHT_DBG_CONSOLE"))
        native_os_register(0x8033ba90u, dbg_write_console);   // surface GC console / crash report
    (void)&os_resume_thread_sync; (void)&is_guest_worker;   // old dormant-worker path, kept for ref
    fprintf(stderr, "[native_os] registered %zu native OS primitive(s) over [%08x,%08x)\n",
            g_native_os.size(), g_os_lo, g_os_hi);
}
