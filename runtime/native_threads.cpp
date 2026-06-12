#include "native_threads.h"
#include "watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace nthr {

// Guest threads are REAL host threads (one std::thread each), serialised by a single CPU
// token so that — as on the single-core GameCube — exactly one runs guest/Dolphin code at a
// time (see docs/native_threading.md: host-thread substrate, fibers dropped). A thread that
// blocks parks on its own condition variable; the token is handed to the highest-priority
// Ready thread. No busy-spin, no step budget, no timing dependence.
//
// The token is `g_running`: a thread executes its body only while `g_running == self`; every
// other live thread is parked in `cv.wait` (lock released). Token hand-off happens under
// `g_mtx`, whose acquire/release also publishes the body's writes (e.g. the global PPC
// register file) to the next thread — so the plain ints the tests touch "only by the token
// holder" are safe without extra synchronisation.
struct GuestThread {
    std::thread             host;
    std::condition_variable cv;          // parked here until granted the token
    State                   state = State::Blocked;
    int                     prio  = 16;
    uint64_t                ready_seq = 0;   // FIFO order among equal-priority Ready
    std::function<void()>   body;
    void*                   user  = nullptr; // per-thread slot for saved global ctx (see hooks)
    // DrainWait bound: the host-clock instant at which the frame barrier resumes even if other
    // threads are still Ready (the hardware retrace interrupt cannot be starved by a yield-spin).
    std::chrono::steady_clock::time_point drain_deadline{};
    bool                    drain_bounded = false;
};

namespace {

std::mutex                 g_mtx;            // guards the token and the registry
std::condition_variable    g_idle_cv;       // run_and_wait() parks here while a thread runs
std::vector<GuestThread*>  g_threads;
GuestThread*               g_running = nullptr;   // the token holder (null ⇒ scheduler idle)
thread_local GuestThread*  t_self    = nullptr;   // THIS host thread's GuestThread (identity)
uint64_t                   g_seq = 0;

void (*g_save_hook)(GuestThread*)    = nullptr;   // called on the thread giving up the token
void (*g_restore_hook)(GuestThread*) = nullptr;   // called for the thread about to get it
void (*g_idle_handler)()             = nullptr;   // called when nothing is Ready but threads live

// SUNBRIGHT_DBG_DRAIN=1 — token-hold accounting (permanent diag). Attributes wall time to
// whichever GuestThread held the token (or to idle, when nobody did) and reports once per
// second together with the bounded-drain overshoot, so a "drain eats the frame" symptom names
// the thread that actually consumed it instead of leaving it to stack-sample roulette.
bool        g_dbg_drain = getenv("SUNBRIGHT_DBG_DRAIN") != nullptr;
std::chrono::steady_clock::time_point g_hold_t0{};   // when the current holder got the token
GuestThread* g_hold_who = nullptr;
struct HoldStat { long long us = 0; long grants = 0; };
std::map<int, HoldStat> g_hold_by_prio;              // keyed by guest prio (idle = -1)
long long   g_drain_overshoot_us = 0;                // resumed past the bounded deadline
long        g_drain_resumes = 0;
std::chrono::steady_clock::time_point g_hold_report{};

void hold_account_locked(GuestThread* next) {
    if (!g_dbg_drain) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_hold_t0.time_since_epoch().count() != 0) {
        auto& s = g_hold_by_prio[g_hold_who ? g_hold_who->prio : -1];
        s.us += std::chrono::duration_cast<std::chrono::microseconds>(now - g_hold_t0).count();
        s.grants++;
    }
    g_hold_t0  = now;
    g_hold_who = next;
    if (g_hold_report.time_since_epoch().count() == 0) g_hold_report = now;
    if (now - g_hold_report >= std::chrono::seconds(1)) {
        g_hold_report = now;
        fprintf(stderr, "[nthr-hold]");
        for (auto& [prio, s] : g_hold_by_prio) {
            fprintf(stderr, " prio%d=%lldms/%ld", prio, s.us / 1000, s.grants);
            s = {};
        }
        fprintf(stderr, "  drain_overshoot=%lldms/%ld\n",
                g_drain_overshoot_us / 1000, g_drain_resumes);
        g_drain_overshoot_us = 0; g_drain_resumes = 0;
    }
}

// Highest-priority Ready thread (lowest prio number; FIFO among ties), or null.
GuestThread* pick_next() {
    GuestThread* best = nullptr;
    for (auto* t : g_threads) {
        if (t->state != State::Ready) continue;
        if (!best || t->prio < best->prio ||
            (t->prio == best->prio && t->ready_seq < best->ready_seq))
            best = t;
    }
    return best;
}

// Give up the token: save `outgoing`'s context, pick the next Ready thread, restore its
// context and wake it. Caller holds g_mtx via `lk`. `outgoing` is null when the bootstrap
// thread grants the token without owning one. If nothing is Ready but threads are still alive
// (all Blocked on something external), the idle handler is run — unlocked, so it can make a
// thread Ready (advance hardware) — and we re-pick; with no handler, control returns to
// run_and_wait().
void grant_token_locked(std::unique_lock<std::mutex>& lk, GuestThread* outgoing) {
    if (outgoing && g_save_hook) g_save_hook(outgoing);   // stash the global ctx into its slot
    for (;;) {
        // Bounded frame barrier: a DrainWait whose host-clock deadline has passed resumes NOW,
        // even though other threads are still Ready — the hardware retrace is an interrupt and
        // cannot be starved by a thread that yield-spins instead of blocking. Checked on every
        // token hand-off (a yield-spinner hands the token back each loop iteration).
        {
            const auto now = std::chrono::steady_clock::now();
            for (auto* t : g_threads)
                if (t->state == State::DrainWait && t->drain_bounded && now >= t->drain_deadline) {
                    t->state = State::Ready;
                    t->ready_seq = g_seq++;
                }
        }
        GuestThread* next = pick_next();
        if (next) {
            hold_account_locked(next);
            g_running = next;
            next->state = State::Running;
            if (g_restore_hook) g_restore_hook(next);     // load the global ctx from its slot
            next->cv.notify_one();
            return;
        }
        // Frame barrier: nothing Ready — release DrainWait threads (they resume exactly when
        // every other thread has run to its own block/yield point) before the idle handler.
        bool drained = false;
        for (auto* t : g_threads)
            if (t->state == State::DrainWait) { t->state = State::Ready; t->ready_seq = g_seq++; drained = true; }
        if (drained) continue;
        bool any_alive = false;
        for (auto* t : g_threads) if (t->state != State::Dead) { any_alive = true; break; }
        if (!any_alive || !g_idle_handler) {
            g_running = nullptr;
            g_idle_cv.notify_all();   // run_and_wait (self-test) waiter; game has none
            return;
        }
        // Every live thread is Blocked waiting on something external. Let the idle handler try
        // to wake one (it runs unlocked — it may call make_ready / run guest code), then re-pick.
        hold_account_locked(nullptr);   // idle period: attribute to prio -1
        lk.unlock();
        g_idle_handler();
        lk.lock();
    }
}

// Body trampoline for every host thread: wait for the token, run the body, then die and
// hand the token on. Never reacquires the token after the body returns.
void thread_main(GuestThread* self) {
    t_self = self;                                 // host-thread identity, never changes
    std::unique_lock<std::mutex> lk(g_mtx);
    while (g_running != self) self->cv.wait(lk);   // restore_hook already ran for us
    lk.unlock();

    self->body();

    lk.lock();
    self->state = State::Dead;
    grant_token_locked(lk, self); // pass the token on; we never get it back
}

}  // namespace

GuestThread* current()                   { return g_running; }
// Deadlock diagnostics: dump every registered thread's scheduler state.
void dump_threads(void* file) {
    FILE* f = (FILE*)file;
    std::unique_lock<std::mutex> lk(g_mtx);
    int i = 0;
    for (auto* t : g_threads)
        fprintf(f, "  [thr %2d] state=%d prio=%d user=%p%s\n", i++, (int)t->state, t->prio,
                t->user, t == g_running ? "  <RUNNING>" : "");
}
// True iff the CALLING host thread may legitimately run guest code right now: it holds the token,
// or nthr isn't driving yet (early boot, before adoption), or the scheduler is idle (the idle-hook
// context runs guest ISRs with g_running == nullptr). Diagnostic for the interpreter entry guard.
bool self_may_run_guest() {
    return g_threads.empty() || g_running == nullptr || g_running == t_self;
}
int          priority_of(GuestThread* t) { return t->prio; }
void*&       user_slot(GuestThread* t)   { return t->user; }

void set_switch_hooks(void (*save)(GuestThread*), void (*restore)(GuestThread*)) {
    g_save_hook    = save;
    g_restore_hook = restore;
}

void set_idle_handler(void (*handler)()) { g_idle_handler = handler; }

int ready_count() {
    std::unique_lock<std::mutex> lk(g_mtx);
    int n = 0;
    for (auto* t : g_threads) if (t->state == State::Ready) n++;
    return n;
}

bool bounded_drain_pending() {
    std::unique_lock<std::mutex> lk(g_mtx);
    for (auto* t : g_threads)
        if (t->state == State::DrainWait && t->drain_bounded) return true;
    return false;
}

bool bounded_drain_expired() {
    std::unique_lock<std::mutex> lk(g_mtx);
    const auto now = std::chrono::steady_clock::now();
    for (auto* t : g_threads)
        if (t->state == State::DrainWait && t->drain_bounded && now >= t->drain_deadline)
            return true;
    return false;
}

GuestThread* spawn(int priority, std::function<void()> body, bool start_ready) {
    std::unique_lock<std::mutex> lk(g_mtx);
    auto* gt = new GuestThread;
    gt->prio      = priority;
    gt->body      = std::move(body);
    gt->state     = start_ready ? State::Ready : State::Blocked;
    gt->ready_seq = g_seq++;
    g_threads.push_back(gt);
    gt->host = std::thread(thread_main, gt);   // parks on cv until granted the token
    return gt;
}

GuestThread* adopt_current(int priority) {
    std::unique_lock<std::mutex> lk(g_mtx);
    auto* gt = new GuestThread;       // no host std::thread — this IS the caller
    gt->prio      = priority;
    gt->state     = State::Running;
    gt->ready_seq = g_seq++;
    g_threads.push_back(gt);
    g_running = gt;                   // the calling thread holds the token immediately
    t_self    = gt;                   // host-thread identity for block()/yield paths
    return gt;
}

static void block_impl(State newState, long deadline_us) {
    std::unique_lock<std::mutex> lk(g_mtx);
    // Identity MUST be the calling host thread, never "whoever holds the token": with the old
    // `self = g_running`, a non-holder calling block() (e.g. guest code run from the idle hook
    // after the token was re-picked underneath it) blocked the WRONG GuestThread and waited on
    // that thread's condvar — when the token later reached that thread, BOTH host threads woke
    // and ran concurrently, racing the shared interpreter/ppc state (the spurious-MEM-dispatch /
    // OSError-15 corruption class, 2026-06-10).
    GuestThread* self = t_self;
    if (!self || self != g_running) {
        fprintf(stderr,
            "\n[nthr] FATAL: block(%d) by a host thread that does not hold the token "
            "(self=%p running=%p). Guest code that can sleep must never run from a non-holder "
            "context (idle hook / interrupt dispatch).\n",
            (int)newState, (void*)self, (void*)g_running);
        fflush(stderr);
        lk.unlock();
        sunbright_park("nthr block() without token");
    }
    self->state     = newState;
    self->ready_seq = g_seq++;            // back of the FIFO (round-robin for Ready)
    self->drain_bounded = (newState == State::DrainWait && deadline_us > 0);
    if (self->drain_bounded)
        self->drain_deadline = std::chrono::steady_clock::now()
                             + std::chrono::microseconds(deadline_us);
    grant_token_locked(lk, self);        // save our ctx, hand the token to the next thread
    while (g_running != self) self->cv.wait(lk);   // park until the token returns to us
    // Resumed: grant_token_locked already restored our ctx via g_restore_hook.
    if (g_dbg_drain && self->drain_bounded) {
        const auto now = std::chrono::steady_clock::now();
        if (now > self->drain_deadline)
            g_drain_overshoot_us += std::chrono::duration_cast<std::chrono::microseconds>(
                now - self->drain_deadline).count();
        g_drain_resumes++;
    }
}

void block(State newState) { block_impl(newState, /*deadline_us=*/0); }

void block_drain(long deadline_us) { block_impl(State::DrainWait, deadline_us); }

void make_ready(GuestThread* t) {
    std::unique_lock<std::mutex> lk(g_mtx);
    if (t->state == State::Blocked) {
        t->state     = State::Ready;
        t->ready_seq = g_seq++;
    }
    // Cooperative: the running thread keeps the token until it next blocks; no preemption.
}

void run_and_wait() {
    std::unique_lock<std::mutex> lk(g_mtx);
    grant_token_locked(lk, nullptr);      // hand the token to the first Ready thread
    for (;;) {
        bool any_alive = false;
        for (auto* t : g_threads)
            if (t->state != State::Dead) { any_alive = true; break; }
        if (!any_alive) break;
        if (!g_running) {   // blocked with no waker — no idle/driver in the core yet
            fprintf(stderr, "[nthr] deadlock: no Ready thread but threads still alive\n");
            break;
        }
        g_idle_cv.wait(lk);   // a thread holds the token; wait until it goes idle/dies
    }
    lk.unlock();

    for (auto* t : g_threads) { if (t->host.joinable()) t->host.join(); delete t; }
    g_threads.clear();
    g_seq = 0;
}

namespace {

// Test 1: two equal-priority threads round-robin via block(Ready); serialized, all run.
bool test_round_robin() {
    constexpr int N = 1000;
    std::atomic<int>  in_crit{0};   // >1 would mean two threads ran concurrently
    std::atomic<bool> concurrent{false};
    std::atomic<int>  total{0};

    auto work = [&] {
        for (int i = 0; i < N; i++) {
            if (in_crit.fetch_add(1, std::memory_order_acq_rel) != 0) concurrent = true;
            total.fetch_add(1, std::memory_order_relaxed);
            in_crit.fetch_sub(1, std::memory_order_acq_rel);
            block(State::Ready);            // yield the token to the other thread
        }
    };
    spawn(10, work, /*start_ready=*/true);
    spawn(10, work, /*start_ready=*/true);
    run_and_wait();

    const bool pass = !concurrent && total.load() == 2 * N;
    fprintf(stderr, "[nthr] round_robin:       %s  (concurrent=%s  total=%d/%d)\n",
            pass ? "PASS" : "FAIL", concurrent ? "YES" : "no", total.load(), 2 * N);
    return pass;
}

// Test 2: the SMS pattern — a HIGHER-priority producer and a LOWER-priority consumer share
// a bounded buffer, blocking via block(Blocked) and waking each other via make_ready. This
// is exactly the message-queue case that starves under run_jit_sync today: if the scheduler
// fails to run the woken (lower-prio) consumer, this hangs or comes up short. PASS proves the
// substrate schedules a woken thread correctly regardless of priority.
bool test_producer_consumer() {
    constexpr int N = 2000;
    constexpr int CAP = 1;
    int count = 0, produced = 0, consumed = 0;   // touched only by the token holder
    GuestThread* prod = nullptr;
    GuestThread* cons = nullptr;
    std::atomic<int>  in_crit{0};
    std::atomic<bool> concurrent{false};
    auto enter = [&] { if (in_crit.fetch_add(1) != 0) concurrent = true; };
    auto leave = [&] { in_crit.fetch_sub(1); };

    prod = spawn(8, [&] {                          // higher priority (lower number)
        for (int i = 0; i < N; i++) {
            while (count >= CAP) block(State::Blocked);   // buffer full → sleep
            enter(); count++; produced++; leave();
            make_ready(cons);                              // wake the consumer
        }
    }, /*start_ready=*/true);

    cons = spawn(12, [&] {                          // lower priority
        for (int i = 0; i < N; i++) {
            while (count == 0) block(State::Blocked);     // buffer empty → sleep
            enter(); count--; consumed++; leave();
            make_ready(prod);                              // wake the producer
        }
    }, /*start_ready=*/true);

    run_and_wait();

    const bool pass = !concurrent && produced == N && consumed == N && count == 0;
    fprintf(stderr, "[nthr] producer_consumer: %s  (produced=%d consumed=%d count=%d concurrent=%s)\n",
            pass ? "PASS" : "FAIL", produced, consumed, count, concurrent ? "YES" : "no");
    return pass;
}

// Test 3: PER-THREAD context save/restore (the switch-hook mechanism — docs step 1). The
// real PPC register file is a single GLOBAL (`ppc`); run_jit_sync works on it, so on every
// token hand-off the outgoing thread's registers must be saved out of that global and the
// incoming thread's restored into it. Here a non-thread_local global slot stands in for that
// register file: each thread writes its own marker into the shared global, yields (letting
// the other thread overwrite it), and on resume MUST still observe its own marker — which
// only holds because the switch hooks move the global into/out of each thread's `user` slot.
// FAIL ⇒ a resumed guest thread would run on another thread's registers.
void* g_shared_ctx = nullptr;                 // stand-in for the global PPC register file
void ctx_save(GuestThread* t)    { user_slot(t) = g_shared_ctx; }
void ctx_restore(GuestThread* t) { g_shared_ctx = user_slot(t); }

bool test_per_thread_ctx() {
    constexpr int N = 500;
    bool clobbered = false;
    int  ran[2] = {0, 0};

    auto work = [&](void* marker, int idx) {
        g_shared_ctx = marker;                    // this thread's "register file"
        for (int i = 0; i < N; i++) {
            block(State::Ready);                  // yield; the other thread overwrites the global
            if (g_shared_ctx != marker) clobbered = true;   // must be restored to OUR marker
            ran[idx]++;
        }
    };

    set_switch_hooks(ctx_save, ctx_restore);
    spawn(10, [&] { work((void*)0x1111, 0); }, /*start_ready=*/true);
    spawn(10, [&] { work((void*)0x2222, 1); }, /*start_ready=*/true);
    run_and_wait();
    set_switch_hooks(nullptr, nullptr);           // don't leak hooks past the test

    const bool pass = !clobbered && ran[0] == N && ran[1] == N;
    fprintf(stderr, "[nthr] per_thread_ctx:    %s  (clobbered=%s ran=%d,%d)\n",
            pass ? "PASS" : "FAIL", clobbered ? "YES" : "no", ran[0], ran[1]);
    return pass;
}
}  // namespace

bool self_test() {
    bool ok = true;
    ok &= test_round_robin();
    ok &= test_producer_consumer();
    ok &= test_per_thread_ctx();
    fprintf(stderr, "[nthr] self_test: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok;
}

}  // namespace nthr
