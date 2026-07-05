// os_impl.cpp — native PC implementation of the GameCube OS kernel seam (E1).
//
// Implements the 61 unresolved OS* symbols the game logic references (grounded in
// scratch/native_unresolved.txt). The game embeds the SDK structs (OSThread,
// OSMutex, OSMessageQueue, OSCond, OSStopwatch) directly in its objects, so we
// KEEP their SDK layouts and carry native backing state in a side-table keyed by
// the SDK-struct pointer.
//
// THREADING MODEL (option B — COOPERATIVE single-runner, condvar substrate):
//   The game does not use threads for gameplay; its OSThreads are async-resource
//   workers (THP decode, JKRAram/JAS DVD, the movie setup/loadResource thread).
//   Running them as real preemptive host threads caused nondeterministic heap
//   corruption / races. So we make ALL threading SYNCHRONOUS: exactly one thread
//   executes game code at a time. Each OSThread is still carried by a host
//   std::thread, but the threads NEVER run concurrently — a single "baton"
//   (g_current) is handed off only at explicit cooperative points (a GC BLOCKING
//   primitive, OSYieldThread, or the VI retrace drain). No preemption => no data
//   races, deterministic, "synchronous". (ucontext/swapcontext are banned; the
//   hand-off is done with one mutex + a per-thread condition_variable.)
//
//   Scheduler invariants (all protected by the single g_sched mutex):
//     - g_current holds the baton; only it runs game code.
//     - A thread BLOCKS by marking itself not-runnable, parking on an object
//       (wait_obj) and handing the baton to the highest-priority runnable thread
//       (round-robin among equals). It wakes only when a WAKER marks it runnable
//       (sched_wake on the matching object) and the baton later returns to it.
//     - VIWaitForRetrace drains all lower-priority workers to idle, then proceeds
//       (sb_sched_drain_until_idle) — the native stand-in for "main sleeps on the
//       VI interrupt while workers run".
//   GC fixed-priority order is approximated (pick lowest priority number first);
//   true mid-flight preemption is unneeded because there is no concurrency.
//
// Host bookkeeping (NativeThread, the thread table) is MALLOC-backed (class
// operator new + a malloc allocator) so it survives the game's JKRHeap freeAll/
// destroy on scene transitions — it must never live on a wiped game heap.
//
// VERIFIED: primitive CONTRACTS in tests/os_test.cpp (cooperative create/resume/
// join/exit, FIFO message passing with blocking back-pressure, mutex recursion,
// cond wait/signal, yield-driven progress, time/heap/stopwatch).

#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSStopwatch.h>
#include <dolphin/os/OSAlloc.h>
#include <dolphin/os/OSCache.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

// Stuck-process watchdog (watchdog_impl.cpp): heartbeat kicked at every scheduler
// hand-off; the running fiber is recorded so a stall can be backtraced.
extern "C" void sb_watchdog_kick(void);
extern "C" void sb_watchdog_set_running(void);

// Host-allocation gate: route plain `new` to host malloc while raised. Used around
// std::thread construction so the carrier's internal state is malloc-backed (it must
// survive the game's JKRHeap teardown on scene transitions). The real (strong)
// definitions live in JKRHeap.cpp (sms-native); these WEAK no-ops let the platform
// unit tests — which do not link sms-native / the JKR heap — resolve the symbols.
extern "C" __attribute__((weak)) void sb_host_alloc_push(void) {}
extern "C" __attribute__((weak)) void sb_host_alloc_pop(void) {}

// Mark the calling host thread as a game thread (its plain `new` may use the JKR heap).
// Called at every OSCreateThread carrier's entry; the bootstrap/main fiber is marked in
// boot_heap_bringup. Default-FALSE so foreign GL-driver worker threads fall back to host
// malloc (see JKRHeap.cpp). Weak no-op for the platform unit tests (no JKR heap linked).
extern "C" __attribute__((weak)) void sb_mark_game_thread(void) {}

namespace {

// GC time base = bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
constexpr long long kTBClock = 40500000LL;

using clock_t_ = std::chrono::steady_clock;
clock_t_::time_point g_boot = clock_t_::now();

long long now_ticks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clock_t_::now() - g_boot).count();
    // ticks = ns * tbclock / 1e9, done in 128-bit-safe order.
    return (long long)((__int128)ns * kTBClock / 1000000000LL);
}

// ---- malloc-backed allocator (host bookkeeping must NOT live on a game heap) --
template <class T>
struct MallocAlloc {
    using value_type = T;
    MallocAlloc() noexcept {}
    template <class U> MallocAlloc(const MallocAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        if (n > SIZE_MAX / sizeof(T)) throw std::bad_alloc();
        void* p = std::malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
};
template <class A, class B>
bool operator==(const MallocAlloc<A>&, const MallocAlloc<B>&) noexcept { return true; }
template <class A, class B>
bool operator!=(const MallocAlloc<A>&, const MallocAlloc<B>&) noexcept { return false; }

// ---- thread backing -------------------------------------------------------
struct NativeThread {
    std::thread th;                  // host carrier; empty for the bootstrap/main fiber
    void* (*func)(void*) = nullptr;
    void* param = nullptr;
    OSThread* os = nullptr;
    std::condition_variable cv;      // signalled when this thread becomes g_current
    bool runnable = false;           // eligible to be scheduled
    bool finished = false;
    bool started  = false;
    bool detached = false;
    bool cancel   = false;
    bool is_main  = false;
    bool abandoned = false;          // retired from the scheduler (e.g. the process-main fiber once
                                     // the game thread takes over) — never picked again

    s32  priority = 16;
    const void* wait_obj = nullptr;  // object this thread is parked on (targeted wake)
    void* retval = nullptr;

    // Host-malloc backed so a NativeThread survives the game's JKRHeap teardown.
    static void* operator new(std::size_t n) {
        void* p = std::malloc(n);
        if (!p) throw std::bad_alloc();
        return p;
    }
    static void operator delete(void* p) noexcept { std::free(p); }
};

// Internal sentinel thrown by OSExitThread / OSCancelThread to unwind a thread
// body (GC OSExitThread is noreturn; we unwind to the trampoline wrapper).
struct ThreadExit { void* val; };

// ---- the cooperative scheduler (single g_sched protects all of this) -------
std::mutex g_sched;
NativeThread* g_current = nullptr;       // holds the baton
NativeThread* g_idle_waiter = nullptr;   // thread in the VI retrace drain
std::unordered_map<OSThread*, NativeThread*, std::hash<OSThread*>,
                   std::equal_to<OSThread*>,
                   MallocAlloc<std::pair<OSThread* const, NativeThread*>>> g_threads;
std::vector<NativeThread*, MallocAlloc<NativeThread*>> g_all;  // creation order (round-robin)

// Pending wakeups for OSSleepThread/OSWakeupThread (ad-hoc queues) so a wake that
// races ahead of the sleep is not lost (the SDK's generation-counter role).
std::vector<const void*, MallocAlloc<const void*>> g_pending_wakes;

thread_local NativeThread* t_self = nullptr;
thread_local OSThread* t_current = nullptr;
thread_local int t_intr_depth = 0;
OSThread g_main_os;   // backing OSThread storage for the bootstrap/main fiber

NativeThread* backing(OSThread* t) {
    auto it = g_threads.find(t);
    return it == g_threads.end() ? nullptr : it->second;
}

// Register the calling host thread (one that did NOT come through OSCreateThread —
// the bootstrap/main) as a cooperative fiber. Idempotent. Caller holds g_sched.
NativeThread* cur_self_locked() {
    if (t_self) return t_self;
    if (std::getenv("SB_SCHED_DBG"))
        std::fprintf(stderr, "[sched] cur_self_locked: NEW main-ish fiber on host tid=%ld (g_current=%p, g_all=%zu)\n",
                     (long)syscall(SYS_gettid), (void*)g_current, g_all.size());
    NativeThread* n = new NativeThread();
    n->is_main = true; n->runnable = true; n->started = true; n->priority = 16;
    n->os = &g_main_os;
    std::memset(&g_main_os, 0, sizeof(g_main_os));
    g_main_os.state = OS_THREAD_STATE_RUNNING;
    g_main_os.priority = 16; g_main_os.base = 16;
    g_threads[n->os] = n;
    g_all.push_back(n);
    t_self = n; t_current = n->os;
    if (!g_current) g_current = n;
    return n;
}

int index_of(NativeThread* n) {
    for (size_t i = 0; i < g_all.size(); ++i)
        if (g_all[i] == n) return (int)i;
    return -1;
}

bool schedulable(NativeThread* n) {
    return n->runnable && !n->finished && !n->abandoned && n->os->suspend <= 0 && n != g_idle_waiter;
}

// Highest priority (lowest number) schedulable thread, round-robin among equals
// starting after g_current. Caller holds g_sched.
NativeThread* pick_next() {
    int best = INT_MAX;
    for (NativeThread* n : g_all)
        if (schedulable(n) && n->priority < best) best = n->priority;
    if (best == INT_MAX) return nullptr;
    int start = index_of(g_current);
    if (start < 0) start = 0;
    int cnt = (int)g_all.size();
    for (int k = 1; k <= cnt; ++k) {
        NativeThread* n = g_all[(start + k) % cnt];
        if (schedulable(n) && n->priority == best) return n;
    }
    return nullptr;
}

[[noreturn]] void sched_deadlock() {
    std::fprintf(stderr, "OSPanic: os_impl scheduler DEADLOCK — no runnable thread "
                 "(current=%p)\n", (void*)g_current);
    std::fflush(stderr);
    std::abort();
}

// Give the baton to the next runnable thread. `self` must have its runnable flag
// already set as desired (false to block, true to yield). Caller holds g_sched.
void hand_baton(NativeThread* self) {
    sb_watchdog_kick();                // forward-progress heartbeat
    NativeThread* next = pick_next();
    if (next == self) return;          // nobody better — keep the baton
    if (!next) next = g_idle_waiter;   // system idle — resume the VI retrace drain
    if (!next) sched_deadlock();
    g_current = next;
    next->cv.notify_all();
}

void wait_my_turn(NativeThread* self, std::unique_lock<std::mutex>& lk) {
    self->cv.wait(lk, [self] { return g_current == self; });
    sb_watchdog_set_running();         // this fiber now holds the baton
}

// Mark every thread parked on `obj` runnable. Caller holds g_sched.
void sched_wake(const void* obj) {
    for (NativeThread* n : g_all)
        if (!n->finished && n->wait_obj == obj) {
            n->wait_obj = nullptr;
            n->runnable = true;
        }
}

// Block the current thread on `obj`, hand the baton on, and wait until woken and
// rescheduled. Throws ThreadExit if the thread was cancelled while parked.
void park_on(const void* obj, std::unique_lock<std::mutex>& lk) {
    NativeThread* self = cur_self_locked();
    self->wait_obj = obj;
    self->runnable = false;
    hand_baton(self);
    wait_my_turn(self, lk);
    self->wait_obj = nullptr;
    if (self->cancel) throw ThreadExit{ self->os ? self->os->val : nullptr };
}

// ---- heap / arena ---------------------------------------------------------
std::mutex g_heap_mu;
std::unordered_map<void*, unsigned long> g_referent;  // ptr -> size (OSReferentSize)
void* g_arena_lo = nullptr;
void* g_arena_hi = nullptr;
std::atomic<int> g_next_heap{0};

// ---- misc HW-vestigial state ---------------------------------------------
std::atomic<unsigned int> g_sound_mode{OS_SOUND_MODE_STEREO};
std::atomic<unsigned int> g_progressive_mode{0};
std::atomic<bool> g_reset_requested{false};

} // namespace

extern "C" {

// ===========================================================================
// Report / Panic
// ===========================================================================
void OSReport(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    std::fprintf(stderr, "OSPanic: %s:%d: ", file ? file : "?", line);
    va_list ap; va_start(ap, msg);
    std::vfprintf(stderr, msg, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

// ===========================================================================
// Time
// ===========================================================================
OSTime OSGetTime(void)  { return (OSTime)now_ticks(); }
OSTick OSGetTick(void)  { return (OSTick)(now_ticks() & 0xFFFFFFFF); }

// Calendar conversion. GC epoch is 2000-01-01 00:00:00; ticks count from boot, but
// for SMS the value is only used for relative save timestamps / display, so we
// treat the tick count as seconds-since-an-epoch and break it down with a faithful
// civil-from-days algorithm (Howard Hinnant's), anchored at year 2000.
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    long long t = (long long)ticks;
    long long secsTotal = t / kTBClock;
    long long rem = t % kTBClock;
    if (rem < 0) { rem += kTBClock; secsTotal -= 1; }
    td->usec = (int)((rem * 1000000LL) / kTBClock % 1000);
    td->msec = (int)((rem * 1000LL) / kTBClock);

    long long days = secsTotal / 86400;
    long long sod  = secsTotal % 86400;
    if (sod < 0) { sod += 86400; days -= 1; }
    td->hour = (int)(sod / 3600);
    td->min  = (int)((sod % 3600) / 60);
    td->sec  = (int)(sod % 60);

    // days since 2000-01-01 -> civil date. days_from_civil epoch shift: 2000-01-01
    // is day 10957 in the 1970 epoch; we anchor at 2000 directly.
    long long z = days + 730425;  // shift to internal era epoch (0000-03-01 based)
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    unsigned d = doy - (153*mp+2)/5 + 1;
    unsigned mo = mp < 10 ? mp+3 : mp-9;
    y += (mo <= 2);
    td->year = (int)y;
    td->mon  = (int)mo - 1;  // GC OSCalendarTime mon is 0-based
    td->mday = (int)d;
    // weekday: 1970-01-01 was Thursday(4); compute from total days since 1970.
    long long days1970 = days + 10957;  // 2000-01-01 is 10957 days after 1970-01-01
    td->wday = (int)(((days1970 % 7) + 4 + 7) % 7);
    td->yday = (int)doy;  // approximate (era day-of-year), good enough for display
}

// ===========================================================================
// Interrupts — vestigial under the cooperative single-runner.
//   Only ONE thread runs at a time and reschedules happen only at explicit
//   block/yield/drain points, so a "disabled interrupts" critical section is
//   already atomic (no other game thread can run in the middle). We keep a
//   per-thread depth so OSYieldThread defers any reschedule inside a critical
//   section, and so the enable/restore return values match the SDK contract.
// ===========================================================================
BOOL OSDisableInterrupts(void) {
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    ++t_intr_depth;
    return wasEnabled;
}
BOOL OSRestoreInterrupts(BOOL /*level*/) {
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    if (t_intr_depth > 0) --t_intr_depth;
    return wasEnabled;
}
BOOL OSEnableInterrupts(void) {
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    if (t_intr_depth > 0) --t_intr_depth;
    return wasEnabled;
}

// ===========================================================================
// Threads
// ===========================================================================
void OSInitThreadQueue(OSThreadQueue* q) { q->head = nullptr; q->tail = nullptr; }

OSThread* OSGetCurrentThread(void) { return t_current; }

BOOL OSIsThreadTerminated(OSThread* t) {
    std::lock_guard<std::mutex> lk(g_sched);
    NativeThread* n = backing(t);
    if (n && n->finished) return TRUE;
    // No backing NativeThread but the OSThread is in MORIBUND state: the game
    // ran what would have been the thread body synchronously (see
    // TMarDirector::setup on SMS_NATIVE_PLATFORM) and stamped MORIBUND to
    // signal completion. Treat that as terminated so the caller's polling
    // `while (!OSIsThreadTerminated) return 0;` loops make progress instead
    // of spinning forever.
    if (!n && t && t->state == OS_THREAD_STATE_MORIBUND) return TRUE;
    return FALSE;
}

static void thread_trampoline(NativeThread* n) {
    sb_mark_game_thread();   // this carrier may run game code that allocs on the JKR heap
    {
        std::unique_lock<std::mutex> lk(g_sched);
        t_self = n;
        t_current = n->os;
        n->started = true;
        // Wait until resumed (runnable) or cancelled, AND holding the baton.
        n->cv.wait(lk, [n] { return (n->runnable || n->cancel) && g_current == n; });
        sb_watchdog_set_running();
    }
    void* rv = nullptr;
    if (!n->cancel) {
        n->os->state = OS_THREAD_STATE_RUNNING;
        try {
            rv = n->func(n->param);
        } catch (ThreadExit& e) {
            rv = e.val;
        }
    }
    std::unique_lock<std::mutex> lk(g_sched);
    n->retval = rv;
    n->finished = true;
    n->runnable = false;
    n->os->state = OS_THREAD_STATE_MORIBUND;
    n->os->val = rv;
    sched_wake(n->os);            // wake joiners parked on this OSThread*
    bool detached = n->detached;
    hand_baton(n);                // pass control on; this host thread exits next
    if (detached) {
        // No joiner will reap us — self-clean.
        g_threads.erase(n->os);
        int idx = index_of(n);
        if (idx >= 0) g_all.erase(g_all.begin() + idx);
        n->th.detach();
        lk.unlock();
        delete n;
        return;
    }
    // Non-detached: a future OSJoinThread will th.join()+delete us.
}

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param,
                   void* stack, u32 stackSize, s32 priority, u16 attr) {
    (void)stack; (void)stackSize;
    // NativeThread uses a class operator new (malloc), so this allocation does NOT
    // go through the JKR heap / OSLockMutex.
    NativeThread* n = new NativeThread();
    n->func = func; n->param = param; n->os = thread; n->priority = priority;
    n->detached = (attr & OS_THREAD_ATTR_DETACH) != 0;
    n->runnable = false;  // created suspended (GC contract)

    std::memset(thread, 0, sizeof(*thread));
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->priority = priority;
    thread->base = priority;
    thread->suspend = 1;  // created suspended
    thread->val = (void*)-1;
    OSInitThreadQueue(&thread->queueJoin);

    {
        std::unique_lock<std::mutex> lk(g_sched);
        cur_self_locked();   // ensure the bootstrap/main fiber is registered
        g_threads[thread] = n;
        g_all.push_back(n);
    }
    // Spawn the carrier OUTSIDE g_sched: std::thread's internal state is allocated
    // via the GLOBAL operator new, which routes through the JKR heap -> OSLockMutex
    // -> g_sched. Holding g_sched here would self-deadlock (it is non-recursive).
    // The trampoline locks g_sched on entry and parks on its start gate until
    // OSResumeThread + the scheduler hand it the baton; n is created suspended so
    // pick_next never selects it before then.
    //
    // Raise the host-allocation gate so std::thread's internal _State_impl goes to
    // host malloc, not the JKR heap — otherwise a scene transition's freeAll/destroy
    // wipes the carrier and the thread crashes on next use.
    sb_host_alloc_push();
    n->th = std::thread(thread_trampoline, n);
    sb_host_alloc_pop();
    return 1;
}

long OSResumeThread(OSThread* thread) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* n = backing(thread);
    if (!n) return 0;
    long prev = thread->suspend;
    if (thread->suspend > 0) thread->suspend--;
    if (thread->suspend == 0) {
        // Releases the creation-suspend (start gate) AND a runtime self-suspend.
        n->runnable = true;
        if (thread->state == OS_THREAD_STATE_READY)
            thread->state = OS_THREAD_STATE_RUNNING;
        // Cooperative: do NOT switch now; it runs at the next scheduling point.
    }
    return prev;
}

// OSSuspendThread — raise the suspend count. A SELF-suspend parks here until
// OSResumeThread (or OSCancelThread) releases it; a cross-thread suspend just bumps
// the count, which pick_next honours (the target stops being scheduled). SMS only
// ever self-suspends its workers (THP Reader/Video/Audio halt themselves at movie
// end), so this is exact for the paths the game exercises.
s32 OSSuspendThread(OSThread* thread) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* n = backing(thread);
    if (!n) return -1;
    NativeThread* self = cur_self_locked();
    s32 prev = (s32)thread->suspend;
    thread->suspend++;
    if (n == self) {
        thread->state = OS_THREAD_STATE_READY;
        while (thread->suspend > 0 && !n->cancel) {
            n->runnable = false;
            hand_baton(n);
            wait_my_turn(n, lk);
        }
        if (n->cancel) throw ThreadExit{ thread->val };
        n->runnable = true;
        thread->state = OS_THREAD_STATE_RUNNING;
    }
    return prev;
}

void OSExitThread(OSThread* thread) {
    // GC OSExitThread is noreturn; the exit value is the thread's ->val. Unwind to
    // the trampoline via throw (caught there).
    void* rv = thread ? thread->val : (t_current ? t_current->val : nullptr);
    throw ThreadExit{ rv };
}

int OSJoinThread(OSThread* thread, void* val) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* n = backing(thread);
    if (!n) return 0;
    cur_self_locked();
    while (!n->finished)
        park_on(thread, lk);   // parked on the target OSThread* (woken on finish)
    void* rv = n->retval;
    if (val) *(void**)val = rv;
    // Reap: remove from the tables, then join + delete the carrier outside the lock.
    g_threads.erase(thread);
    int idx = index_of(n);
    if (idx >= 0) g_all.erase(g_all.begin() + idx);
    lk.unlock();
    if (n->th.joinable()) n->th.join();
    delete n;
    return 1;
}

void OSDetachThread(OSThread* thread) {
    std::lock_guard<std::mutex> lk(g_sched);
    NativeThread* n = backing(thread);
    if (!n) return;
    thread->attr |= OS_THREAD_ATTR_DETACH;
    n->detached = true;
}

void OSCancelThread(OSThread* thread) {
    // Cooperative cancel: flag the thread and make it schedulable so it unwinds
    // (throws ThreadExit out of park_on / the start gate) the next time it runs.
    std::lock_guard<std::mutex> lk(g_sched);
    NativeThread* n = backing(thread);
    if (!n) return;
    n->cancel = true;
    thread->state = OS_THREAD_STATE_MORIBUND;
    thread->suspend = 0;
    n->runnable = true;
    n->wait_obj = nullptr;
}

long OSGetThreadPriority(OSThread* thread) { return thread->priority; }

void OSYieldThread(void) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* self = cur_self_locked();
    if (t_intr_depth > 0) return;     // defer reschedule inside a critical section
    NativeThread* next = pick_next(); // self is runnable, so it is a candidate
    if (!next || next == self) return;
    g_current = next;
    next->cv.notify_all();
    wait_my_turn(self, lk);
}

s32 OSEnableScheduler(void) { return 0; }  // no explicit scheduler gate to toggle

// ThreadQueue sleep/wake — ad-hoc wait queues (caller-owned SDK structs). Backed by
// the cooperative park/wake on the queue pointer, with a pending-wake list so a
// wakeup that races ahead of the sleep is not lost (the SDK generation-counter role).
void OSWakeupThread(OSThreadQueue* q) {
    std::lock_guard<std::mutex> lk(g_sched);
    bool any = false;
    for (NativeThread* n : g_all)
        if (!n->finished && n->wait_obj == q) any = true;
    sched_wake(q);
    if (!any) g_pending_wakes.push_back(q);  // remember for a sleeper not yet parked
}

void OSSleepThread(OSThreadQueue* q) {
    std::unique_lock<std::mutex> lk(g_sched);
    cur_self_locked();
    for (size_t i = 0; i < g_pending_wakes.size(); ++i)
        if (g_pending_wakes[i] == q) {        // a wake already arrived — consume it
            g_pending_wakes.erase(g_pending_wakes.begin() + i);
            return;
        }
    park_on(q, lk);
}

// ===========================================================================
// Mutex (recursive) / Cond
// ===========================================================================
void OSInitMutex(OSMutex* m) {
    m->queue.head = m->queue.tail = nullptr;
    m->thread = nullptr;
    m->count = 0;
    m->link.next = m->link.prev = nullptr;
}

void OSLockMutex(OSMutex* m) {
    std::unique_lock<std::mutex> lk(g_sched);
    OSThread* self = cur_self_locked()->os;
    if (m->thread == self) { m->count++; return; }   // recursive relock
    while (m->thread != nullptr) park_on(m, lk);
    m->thread = self;
    m->count = 1;
}

BOOL OSTryLockMutex(OSMutex* m) {
    std::unique_lock<std::mutex> lk(g_sched);
    OSThread* self = cur_self_locked()->os;
    if (m->thread == self) { m->count++; return TRUE; }
    if (m->thread == nullptr) { m->thread = self; m->count = 1; return TRUE; }
    return FALSE;
}

void OSUnlockMutex(OSMutex* m) {
    std::lock_guard<std::mutex> lk(g_sched);
    if (m->count > 0 && --m->count == 0) {
        m->thread = nullptr;
        sched_wake(m);
    }
}

void OSInitCond(OSCond* c) { c->queue.head = c->queue.tail = nullptr; }

void OSWaitCond(OSCond* c, OSMutex* m) {
    std::unique_lock<std::mutex> lk(g_sched);
    OSThread* self = cur_self_locked()->os;
    int saved = m->count;
    m->count = 0; m->thread = nullptr;   // fully release the (recursive) mutex
    sched_wake(m);
    park_on(c, lk);                       // wait for OSSignalCond
    while (m->thread != nullptr) park_on(m, lk);  // reacquire
    m->thread = self; m->count = saved;
}

void OSSignalCond(OSCond* c) {
    std::lock_guard<std::mutex> lk(g_sched);
    sched_wake(c);
}

// ===========================================================================
// Message queue (bounded ring, FIFO, blocking + nonblocking + jam)
// ===========================================================================
void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, long msgCount) {
    mq->queueSend.head = mq->queueSend.tail = nullptr;
    mq->queueReceive.head = mq->queueReceive.tail = nullptr;
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}

int OSSendMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_sched);
    cur_self_locked();
    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        while (mq->usedCount >= mq->msgCount) park_on(&mq->queueSend, lk);
    }
    long idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[idx] = msg;
    mq->usedCount++;
    sched_wake(&mq->queueReceive);
    return TRUE;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_sched);
    cur_self_locked();
    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        while (mq->usedCount >= mq->msgCount) park_on(&mq->queueSend, lk);
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[mq->firstIndex] = msg;
    mq->usedCount++;
    sched_wake(&mq->queueReceive);
    return TRUE;
}

int OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_sched);
    cur_self_locked();
    if (mq->usedCount == 0) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        while (mq->usedCount == 0) park_on(&mq->queueReceive, lk);
    }
    if (msg) *(OSMessage*)msg = ((OSMessage*)mq->msgArray)[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    sched_wake(&mq->queueSend);
    return TRUE;
}

// ===========================================================================
// Scheduler drive — VIWaitForRetrace runs the workers until the system is idle,
// then proceeds with the retrace. This is the native stand-in for "main blocks on
// the VI interrupt while lower-priority workers run". Called from vi_impl.cpp.
// ===========================================================================
// Retire the CALLING host thread's fiber from the cooperative scheduler permanently.
// Used by the SB_WINDOW boot path: a static-init heap bringup (JKRExpHeap::alloc ->
// OSLockMutex) registers the PROCESS-MAIN thread as the "main fiber" and makes it
// g_current before __wrap_main spawns the real game thread (a second main fiber) and
// leaves process-main to run the SDL present loop. process-main then never re-enters
// the scheduler, so any pick_next that selects its fiber loses the baton (cv.notify to
// a thread asleep in SDL) -> deadlock. Retiring it makes the game thread the sole main
// fiber (identical to the headless single-main-thread configuration, which never hangs).
// The fiber struct is kept alive (t_self stays valid) but flagged unschedulable.
void sb_sched_retire_current_fiber(void) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* self = cur_self_locked();
    self->abandoned = true;
    self->runnable  = false;
    if (g_current == self) {
        // Hand the baton on. If the game thread has already registered, pick it;
        // otherwise clear g_current so its first cur_self_locked() claims the baton.
        NativeThread* next = pick_next();
        g_current = next;
        if (next) next->cv.notify_all();
    }
}

void sb_sched_drain_until_idle(void) {
    std::unique_lock<std::mutex> lk(g_sched);
    NativeThread* self = cur_self_locked();
    NativeThread* prev = g_idle_waiter;
    g_idle_waiter = self;
    for (;;) {
        bool other = false;
        for (NativeThread* n : g_all)
            if (n != self && n->runnable && !n->finished && n->os->suspend <= 0) {
                other = true; break;
            }
        if (!other) break;
        self->runnable = false;     // step out so a worker (and only a worker) runs
        hand_baton(self);
        wait_my_turn(self, lk);     // a worker handed the baton back when it blocked
        self->runnable = true;
    }
    g_idle_waiter = prev;
}

// ===========================================================================
// Heap / arena
//   OSAllocFromHeap is backed by aligned malloc (32-byte, GC default). JKRHeap
//   does the real sub-allocation atop a few big OSAlloc blocks, so this layer is
//   called rarely with large sizes. LANDMINE: allocations are NOT guaranteed to
//   fall within [OSGetArenaLo, OSGetArenaHi]; if the game ever assumes that, switch
//   to an arena bump/free-list allocator over the g_arena block.
// ===========================================================================
} // extern "C"

namespace {
void* aligned_alloc32(unsigned long size) {
    void* p = nullptr;
    if (size == 0) size = 1;
    if (posix_memalign(&p, 32, size) != 0) return nullptr;
    return p;
}
}

extern "C" {

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int /*maxHeaps*/) {
    g_arena_lo = arenaStart;
    g_arena_hi = arenaEnd;
    return arenaStart;
}

int OSCreateHeap(void* /*start*/, void* /*end*/) { return g_next_heap++; }
void OSDestroyHeap(int /*heap*/) {}

void* OSAllocFromHeap(int /*heap*/, unsigned long size) {
    void* p = aligned_alloc32(size);
    if (p) { std::lock_guard<std::mutex> lk(g_heap_mu); g_referent[p] = size; }
    return p;
}
void OSFreeToHeap(int /*heap*/, void* ptr) {
    if (!ptr) return;
    { std::lock_guard<std::mutex> lk(g_heap_mu); g_referent.erase(ptr); }
    std::free(ptr);
}
unsigned long OSReferentSize(void* ptr) {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    auto it = g_referent.find(ptr);
    return it == g_referent.end() ? 0 : it->second;
}
long OSCheckHeap(int /*heap*/) { return 0; }   // >=0 means consistent
void OSDumpHeap(int /*heap*/) {}

void* OSGetArenaLo(void) { return g_arena_lo; }
void* OSGetArenaHi(void) { return g_arena_hi; }
void  OSSetArenaLo(void* p) { g_arena_lo = p; }
void  OSSetArenaHi(void* p) { g_arena_hi = p; }

// ===========================================================================
// Stopwatch
// ===========================================================================
void OSInitStopwatch(OSStopwatch* sw, char* name) {
    sw->name = name; sw->total = 0; sw->hits = 0;
    sw->min = 0x7FFFFFFFFFFFFFFFLL; sw->max = 0; sw->last = 0; sw->running = 0;
}
void OSResetStopwatch(OSStopwatch* sw) {
    sw->total = 0; sw->hits = 0; sw->min = 0x7FFFFFFFFFFFFFFFLL;
    sw->max = 0; sw->last = 0;
}
void OSStartStopwatch(OSStopwatch* sw) { sw->running = 1; sw->last = now_ticks(); }
void OSStopStopwatch(OSStopwatch* sw) {
    if (!sw->running) return;
    long long elapsed = now_ticks() - sw->last;
    sw->running = 0; sw->last = elapsed; sw->total += elapsed; sw->hits++;
    if (elapsed < sw->min) sw->min = elapsed;
    if (elapsed > sw->max) sw->max = elapsed;
}
long long OSCheckStopwatch(OSStopwatch* sw) {
    return sw->running ? (sw->total + (now_ticks() - sw->last)) : sw->total;
}

// ===========================================================================
// Misc HW-vestigial
// ===========================================================================
void OSProtectRange(u32, void*, u32, u32) {}          // no MMU on host
void OSFillFPUContext(OSContext*) {}                  // no register-file swap
OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler old) { return old; }
void OSResetSystem(int, u32, BOOL) { g_reset_requested = true; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
u32  OSGetSoundMode(void) { return g_sound_mode.load(); }
void OSSetSoundMode(u32 m) { g_sound_mode = m; }
u32  OSGetProgressiveMode(void) { return g_progressive_mode.load(); }
void OSSetProgressiveMode(u32 m) { g_progressive_mode = m; }

// Font — needs the SDK font ROM (sysmenu font). Not yet ported; stub returning "no
// glyph" so callers degrade gracefully. LANDMINE: OSReport-to-screen / debug font
// rendering will show nothing until OSInitFont reads real font data.
BOOL OSInitFont(OSFontHeader*) { return FALSE; }
u16  OSGetFontEncode(void) { return 0; /* ANSI */ }
char* OSGetFontTexture(char* str, void** image, s32* x, s32* y, s32* width) {
    if (image) *image = nullptr;
    if (x) *x = 0;
    if (y) *y = 0;
    if (width) *width = 0;
    return str && *str ? str + 1 : str;
}
char* OSGetFontWidth(char* str, s32* width) {
    if (width) *width = 0;
    return str && *str ? str + 1 : str;
}

// ---- cache ops (OSCache.h) ------------------------------------------------
// On GameCube these flush/invalidate the L1 data/instruction cache so the GPU
// (which reads main RAM directly, bypassing the CPU cache) sees CPU writes — and
// vice versa. On the native host there is ONE coherent address space and no
// separate GX FIFO DMA engine reading stale RAM, so flush/store/invalidate are
// no-ops. DCZeroRange is a real memset (callers use it to zero a buffer); it
// operates on whole 32-byte cache blocks, matching dcbz granularity.
void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCTouchRange(void*, u32) {}
void ICInvalidateRange(void*, u32) {}
void DCZeroRange(void* addr, u32 nBytes) {
    if (!addr || !nBytes) return;
    // Round to the 32-byte cache-block span the HW would clear.
    uintptr_t lo = (uintptr_t)addr & ~uintptr_t(31);
    uintptr_t hi = ((uintptr_t)addr + nBytes + 31u) & ~uintptr_t(31);
    std::memset((void*)lo, 0, hi - lo);
}

} // extern "C"
