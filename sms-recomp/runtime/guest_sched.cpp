// guest_sched.cpp — cooperative guest threading. See guest_sched.h.
//
// One token, handed between real host threads. A thread runs only while it holds the token,
// so guest code never executes concurrently and the game's single-core assumptions hold.
// Blocking is a token hand-off, which means a guest thread can park mid-function and resume
// exactly where it was — the thing a function-granular recompiler cannot otherwise do.
//
// Priority follows the GC convention: LOWER number means higher priority.

#include "guest_sched.h"

#include "intrinsics.h"

#include <lucent/log.h>

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

extern void call_ppc(CPUState& cpu, u32 address);

namespace {

// DrainWait: parked until nothing else is Ready — the frame barrier. See gsched_drain.
enum class State { Ready, Running, Blocked, DrainWait, Dead };

struct GuestThread {
    u32 os_thread = 0;
    u32 entry = 0, param = 0, stack = 0;
    int priority = 0;
    State state = State::Blocked;
    u32 wait_queue = 0;
    bool started = false;
    CPUState cpu{};
    std::condition_variable cv;
    std::thread host;
};

std::mutex g_lock;
std::vector<GuestThread*> g_threads;
GuestThread* g_running = nullptr;

// Which guest thread this host thread is. Set once per host thread.
thread_local GuestThread* t_self = nullptr;

GuestThread* find(u32 os_thread) {
    for (auto* t : g_threads)
        if (t->os_thread == os_thread) return t;
    return nullptr;
}

// Highest priority (lowest number) runnable thread.
GuestThread* pick_next() {
    GuestThread* best = nullptr;
    for (auto* t : g_threads)
        if (t->state == State::Ready && (!best || t->priority < best->priority)) best = t;
    return best;
}

// Hand the token to the best runnable thread. Caller holds g_lock and has already put
// itself into a non-Running state.
void release_token(std::unique_lock<std::mutex>& lk) {
    GuestThread* next = pick_next();
    if (!next) {
        // Nothing Ready: release the frame barrier. Everyone else has run to their own
        // block point, which is exactly the condition a retrace wait is waiting for.
        bool released = false;
        for (auto* t : g_threads)
            if (t->state == State::DrainWait) { t->state = State::Ready; released = true; }
        if (released) next = pick_next();
    }
    if (!next) {
        // Nothing runnable and someone is waiting: with no interrupt source there is no
        // event that can make a thread ready, so this can never resolve on its own.
        bool any_blocked = false;
        for (auto* t : g_threads) any_blocked |= (t->state == State::Blocked);
        if (any_blocked) {
            lucent::error("sched", "deadlock: every guest thread is blocked and nothing can "
                                   "wake them");
            for (auto* t : g_threads)
                lucent::error("sched", "  thread 0x{:08x} entry 0x{:08x} prio {} state {} "
                                       "queue 0x{:08x}",
                              t->os_thread, t->entry, t->priority, (int)t->state,
                              t->wait_queue);
            std::abort();
        }
        g_running = nullptr;
        return;
    }
    next->state = State::Running;
    g_running = next;
    next->cv.notify_one();
    (void)lk;
}

// The guest's "who is running" global. Guest code reads it through OSGetCurrentThread, so
// it has to track the token rather than whatever the guest scheduler last wrote.
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;

// OSThread::state, a halfword at +712 (verified against OSResumeThread and
// OSIsThreadTerminated, which tests it for MORIBUND or 0).
constexpr u32 T_STATE           = 712;
constexpr u16 OS_THREAD_MORIBUND = 8;

// Thread 0 is adopted before OSInit has created the default OSThread, so its guest identity
// is not known yet. Pick it up the first time it matters, rather than writing our
// placeholder zero over the global the OS later sets — doing that handed guest code a NULL
// OSThread* and it faulted reading a field at +0x2f8.
void adopt_self_id() {
    if (t_self && t_self->os_thread == 0) t_self->os_thread = sb_r32(OS_CURRENT_THREAD);
}

// Block until this thread holds the token.
void acquire_token(std::unique_lock<std::mutex>& lk, GuestThread* self) {
    self->cv.wait(lk, [self] { return g_running == self; });
    if (self->os_thread) sb_w32(OS_CURRENT_THREAD, self->os_thread);
}

void thread_main(GuestThread* self) {
    t_self = self;
    {
        std::unique_lock<std::mutex> lk(g_lock);
        acquire_token(lk, self);
    }

    // A minimal frame at the top of the thread's own stack: 8-byte aligned, null back chain.
    // Borrowing the creator's stack instead would let a deep body run off the end of
    // whatever frame happened to be current.
    self->cpu.gpr[1] = (self->stack - 8) & ~7u;
    self->cpu.gpr[3] = self->param;

    call_ppc(self->cpu, self->entry);

    // Returning from the body is a normal exit.
    gsched_exit();
}

} // namespace

CPUState& gsched_cpu() {
    if (t_self) return t_self->cpu;
    std::abort();   // called from a host thread that is not a guest thread
}

void gsched_init(CPUState& main_cpu, u32 os_thread) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto* t = new GuestThread();
    t->os_thread = os_thread;
    t->priority  = 16;          // the default thread's priority
    t->state     = State::Running;
    t->started   = true;
    t->cpu       = main_cpu;
    g_threads.push_back(t);
    g_running = t;
    t_self    = t;
}

void gsched_create(u32 os_thread, u32 entry, u32 param, u32 stack, int priority) {
    adopt_self_id();
    std::lock_guard<std::mutex> lk(g_lock);

    // The game REUSES OSThread structs: TApplication creates gSetupThread once for BOOT and
    // again for NLOGO with a different entry point. Treating the address as already-tracked
    // meant the second thread was never created — it never ran, never terminated, and the
    // NLOGO state waited forever on OSIsThreadTerminated(&gSetupThread).
    if (GuestThread* old = find(os_thread)) {
        // RUNNING means we are inside that thread's own body, which cannot be a legitimate
        // recreation — that is a real bug worth stopping for.
        if (old->state == State::Running) {
            lucent::error("sched", "OSThread 0x{:08x} recreated from inside its own body "
                                   "(entry 0x{:08x} -> 0x{:08x})",
                          os_thread, old->entry, entry);
            std::abort();
        }
        // Blocked is legitimate: the game recreates a struct whose previous occupant is
        // parked forever on a queue nothing will post to (a THP worker, for instance).
        // From the game's point of view that thread is finished. Retire it so the scheduler
        // never picks it again.
        //
        // The parked host thread stays parked — it is waiting on a condition variable that
        // will never be signalled. That leaks one host thread per recreation, which is
        // bounded and harmless in practice, but it is a leak: the honest fix is a generation
        // counter so a woken stale thread exits instead of resuming a retired body.
        if (old->state != State::Dead) {
            lucent::debug("sched", "OSThread 0x{:08x} recreated while parked; retiring the "
                                   "previous body (entry 0x{:08x})", os_thread, old->entry);
            old->state = State::Dead;
        }
        if (old->host.joinable()) old->host.detach();
        old->entry = entry; old->param = param; old->stack = stack;
        old->priority = priority;
        old->state = State::Blocked;    // created suspended; OSResumeThread starts it
        old->wait_queue = 0;
        old->started = false;
        old->cpu = CPUState{};
        if (t_self) {
            old->cpu.gpr[2]  = t_self->cpu.gpr[2];
            old->cpu.gpr[13] = t_self->cpu.gpr[13];
        }
        return;
    }

    auto* t = new GuestThread();
    t->os_thread = os_thread;
    t->entry     = entry;
    t->param     = param;
    t->stack     = stack;
    t->priority  = priority;
    t->state     = State::Blocked;   // created suspended; OSResumeThread starts it

    // Inherit the small-data base registers from the creating thread. r2 (SDA2) and r13
    // (SDA) are set once at boot and are part of the ABI for every thread — real
    // OSCreateThread seeds the new context with them. A zeroed CPUState instead makes every
    // r13-relative access a wild address (observed: SMSLoadArchive reading 0xffffa0d4).
    if (t_self) {
        t->cpu.gpr[2]  = t_self->cpu.gpr[2];
        t->cpu.gpr[13] = t_self->cpu.gpr[13];
    }
    g_threads.push_back(t);
}

void gsched_make_ready(u32 os_thread) {
    adopt_self_id();
    std::unique_lock<std::mutex> lk(g_lock);
    GuestThread* t = find(os_thread);
    if (!t || t->state == State::Dead || t->state == State::Running) return;

    t->wait_queue = 0;
    t->state = State::Ready;
    if (!t->started) {
        t->started = true;
        t->host = std::thread(thread_main, t);
    }

    // GC semantics: a thread that becomes runnable at a strictly higher priority than the
    // running one preempts it. Code relies on this — a creator that starts a higher-priority
    // worker and returns immediately expects the worker to have run.
    GuestThread* self = t_self;
    if (self && t->priority < self->priority) {
        self->state = State::Ready;
        release_token(lk);
        acquire_token(lk, self);
    }
}

void gsched_block(u32 wait_queue) {
    adopt_self_id();
    std::unique_lock<std::mutex> lk(g_lock);
    GuestThread* self = t_self;
    if (!self) return;
    self->state      = State::Blocked;
    self->wait_queue = wait_queue;
    release_token(lk);
    acquire_token(lk, self);
}

void gsched_wake_queue(u32 wait_queue) {
    std::unique_lock<std::mutex> lk(g_lock);
    for (auto* t : g_threads) {
        if (t->state == State::Blocked && t->wait_queue == wait_queue) {
            t->wait_queue = 0;
            t->state = State::Ready;
            if (!t->started) { t->started = true; t->host = std::thread(thread_main, t); }
        }
    }
}

// Frame barrier: park until no other thread is Ready. On hardware VIWaitForRetrace sleeps
// for a whole field and every runnable thread gets to run during it, REGARDLESS of priority.
// A plain yield cannot express that: the caller would still be the highest-priority runnable
// thread and would simply be picked again, starving the lower-priority setup and loader
// threads forever.
void gsched_drain() {
    adopt_self_id();
    std::unique_lock<std::mutex> lk(g_lock);
    GuestThread* self = t_self;
    if (!self) return;
    if (!pick_next()) return;          // nobody else to run; nothing to wait for
    self->state = State::DrainWait;
    release_token(lk);
    acquire_token(lk, self);
}

void gsched_yield() {
    adopt_self_id();
    std::unique_lock<std::mutex> lk(g_lock);
    GuestThread* self = t_self;
    if (!self) return;
    if (!pick_next()) return;          // nobody else to run
    self->state = State::Ready;
    release_token(lk);
    acquire_token(lk, self);
}

void gsched_exit() {
    adopt_self_id();
    std::unique_lock<std::mutex> lk(g_lock);
    GuestThread* self = t_self;
    if (!self) return;
    self->state = State::Dead;

    // Publish the death to the GUEST's own OSThread, not just our bookkeeping.
    // OSIsThreadTerminated reads this halfword, and the game's main loop polls it before
    // joining. Leaving it untouched meant the loop asked "terminated?" forever, got false,
    // joined (which returned instantly because our scheduler knew better), and looped —
    // rendering frames indefinitely without ever advancing the boot sequence.
    if (self->os_thread) sb_w16(self->os_thread + T_STATE, OS_THREAD_MORIBUND);
    // OSJoinThread parks on a queue keyed by the thread it is waiting for.
    for (auto* t : g_threads)
        if (t->state == State::Blocked && t->wait_queue == self->os_thread) {
            t->wait_queue = 0;
            t->state = State::Ready;
        }
    release_token(lk);
}

bool gsched_is_dead(u32 os_thread) {
    std::lock_guard<std::mutex> lk(g_lock);
    GuestThread* t = find(os_thread);
    return !t || t->state == State::Dead;
}

u32 gsched_current_os_thread() { return t_self ? t_self->os_thread : 0; }

bool gsched_is_tracked(u32 os_thread) {
    std::lock_guard<std::mutex> lk(g_lock);
    return find(os_thread) != nullptr;
}
