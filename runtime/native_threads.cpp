#include "native_threads.h"

#include <atomic>
#include <csetjmp>
#include <cstdio>
#include <ucontext.h>
#include <vector>

namespace nthr {

// Guest threads are FIBERS multiplexed on ONE host thread (see docs/native_threading.md:
// keeps Dolphin's single CPU-thread identity, avoids IsCPUThread/s_core_mutex). Cooperative
// + single-threaded ⇒ no locks: only the running fiber (or the scheduler) executes at a time.
struct GuestThread {
    ucontext_t            ctx;
    std::vector<char>     stack;
    State                 state = State::Blocked;
    int                   prio  = 16;
    uint64_t              ready_seq = 0;   // FIFO order among equal-priority Ready
    std::function<void()> body;
    void*                 user  = nullptr; // per-fiber slot for saved host TLS (see hooks)
};

namespace {

ucontext_t                 g_sched_ctx;     // the scheduler loop's context (run_and_wait)
std::vector<GuestThread*>  g_threads;
GuestThread*               g_running = nullptr;   // fiber currently executing (null in sched)
uint64_t                   g_seq = 0;
constexpr size_t           STACK_SIZE = 1u << 20; // 1 MiB per fiber (recomp uses the C stack)

void (*g_save_hook)(GuestThread*)    = nullptr;   // called when a fiber yields the CPU
void (*g_restore_hook)(GuestThread*) = nullptr;   // called before a fiber is granted the CPU

// Highest-priority Ready fiber (lowest prio number; FIFO among ties), or null.
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

// Entry trampoline for every fiber: run its body, mark dead, return to the scheduler.
void fiber_trampoline() {
    g_running->body();
    g_running->state = State::Dead;
    swapcontext(&g_running->ctx, &g_sched_ctx);   // back to scheduler; never resumes
}

}  // namespace

GuestThread* current()                   { return g_running; }
int          priority_of(GuestThread* t) { return t->prio; }
void*&       user_slot(GuestThread* t)   { return t->user; }

void set_switch_hooks(void (*save)(GuestThread*), void (*restore)(GuestThread*)) {
    g_save_hook    = save;
    g_restore_hook = restore;
}

GuestThread* spawn(int priority, std::function<void()> body, bool start_ready) {
    auto* gt = new GuestThread;
    gt->prio      = priority;
    gt->body      = std::move(body);
    gt->state     = start_ready ? State::Ready : State::Blocked;
    gt->ready_seq = g_seq++;
    gt->stack.resize(STACK_SIZE);
    getcontext(&gt->ctx);
    gt->ctx.uc_stack.ss_sp   = gt->stack.data();
    gt->ctx.uc_stack.ss_size = gt->stack.size();
    gt->ctx.uc_link          = &g_sched_ctx;
    makecontext(&gt->ctx, fiber_trampoline, 0);
    g_threads.push_back(gt);
    return gt;
}

void block(State newState) {
    GuestThread* self = g_running;
    self->state     = newState;
    self->ready_seq = g_seq++;            // back of the FIFO (round-robin for Ready)
    if (g_save_hook) g_save_hook(self);   // stash this fiber's host TLS before leaving it
    swapcontext(&self->ctx, &g_sched_ctx);   // to scheduler; resumes here when rescheduled
    // Resumed: run_and_wait already restored our TLS via g_restore_hook before swapping in.
}

void make_ready(GuestThread* t) {
    if (t->state == State::Blocked) {
        t->state     = State::Ready;
        t->ready_seq = g_seq++;
    }
}

void run_and_wait() {
    for (;;) {
        GuestThread* gt = pick_next();
        if (!gt) {
            bool any_alive = false;
            for (auto* t : g_threads)
                if (t->state != State::Dead) { any_alive = true; break; }
            if (any_alive)   // blocked with no waker — no idle/driver in the core yet
                fprintf(stderr, "[nthr] deadlock: no Ready fiber but threads still alive\n");
            break;
        }
        g_running = gt;
        gt->state = State::Running;
        if (g_restore_hook) g_restore_hook(gt);   // restore this fiber's host TLS before it runs
        swapcontext(&g_sched_ctx, &gt->ctx);   // run the fiber until it blocks or dies
        g_running = nullptr;
    }
    for (auto* t : g_threads) delete t;
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

// Test 3: FIBER feasibility (the recommended execution model — see docs/native_threading.md).
// Two cooperative fibers (ucontext) multiplexed on ONE thread via swapcontext, proving:
//  (a) fiber context switching works and interleaves as scheduled;
//  (b) a sigsetjmp/siglongjmp pair with a jmpbuf living on the FIBER's own stack (not
//      thread_local) lands correctly within that fiber — this is exactly the recomp
//      tail-handoff (g_tail_jmp) mechanism, which must become per-fiber once guest threads
//      are fibers sharing one thread's TLS. If this works, fibers are viable for guest
//      execution without tripping Dolphin's IsCPUThread (everything stays on the EmuThread).
ucontext_t fb_sched, fb_ctx[2];
bool       fb_done[2] = {false, false};
int        fb_seq[16], fb_seq_n = 0;
bool       fb_jmp_ok = false;

void fiber_a_fn() {
    for (int i = 0; i < 3; i++) { fb_seq[fb_seq_n++] = 0; swapcontext(&fb_ctx[0], &fb_sched); }
    fb_done[0] = true;                       // falls through to uc_link (= fb_sched)
}
void fiber_b_fn() {
    sigjmp_buf jb;                           // on THIS fiber's stack, not thread_local
    if (sigsetjmp(jb, 0) == 0) {
        for (int i = 0; i < 3; i++) { fb_seq[fb_seq_n++] = 1; swapcontext(&fb_ctx[1], &fb_sched); }
        siglongjmp(jb, 1);                   // tail-handoff style jump back into this fiber
    } else {
        fb_jmp_ok = true; fb_seq[fb_seq_n++] = 9;
    }
    fb_done[1] = true;
}

bool test_fibers() {
    static std::vector<char> sa(128 * 1024), sb(128 * 1024);
    fb_seq_n = 0; fb_jmp_ok = false; fb_done[0] = fb_done[1] = false;

    getcontext(&fb_ctx[0]);
    fb_ctx[0].uc_stack.ss_sp = sa.data(); fb_ctx[0].uc_stack.ss_size = sa.size();
    fb_ctx[0].uc_link = &fb_sched; makecontext(&fb_ctx[0], fiber_a_fn, 0);
    getcontext(&fb_ctx[1]);
    fb_ctx[1].uc_stack.ss_sp = sb.data(); fb_ctx[1].uc_stack.ss_size = sb.size();
    fb_ctx[1].uc_link = &fb_sched; makecontext(&fb_ctx[1], fiber_b_fn, 0);

    int turn = 0, guard = 0;
    while (!(fb_done[0] && fb_done[1]) && guard++ < 100) {
        if (!fb_done[turn]) swapcontext(&fb_sched, &fb_ctx[turn]);
        turn ^= 1;
    }
    // expected interleave 0,1,0,1,0,1 then B's longjmp lands → 9
    const bool pass = fb_jmp_ok && fb_seq_n == 7 && fb_seq[6] == 9 &&
                      fb_done[0] && fb_done[1];
    fprintf(stderr, "[nthr] fibers:            %s  (siglongjmp-in-fiber=%s seq_n=%d)\n",
            pass ? "PASS" : "FAIL", fb_jmp_ok ? "ok" : "BAD", fb_seq_n);
    return pass;
}

// Test 4: PER-FIBER host TLS (the per-fiber tail-jmp mechanism — docs step 1). All fibers
// share the host thread's real thread_local storage, so without save/restore one fiber would
// clobber another's `g_tail_jmp`. Each fiber writes its own marker into a shared thread_local,
// yields (block(Ready)) — letting the other fiber overwrite the shared slot — and on resume
// MUST still observe its own marker. The switch hooks move the thread_local into/out of the
// fiber's `user` slot, exactly as the runtime will move g_tail_jmp. FAIL ⇒ a resumed fiber
// would siglongjmp through another fiber's stale jmpbuf.
thread_local void* t_shared_tls = nullptr;   // stand-in for the real g_tail_jmp thread_local
void tls_save(GuestThread* t)    { user_slot(t) = t_shared_tls; }
void tls_restore(GuestThread* t) { t_shared_tls = user_slot(t); }

bool test_per_fiber_tls() {
    constexpr int N = 500;
    bool clobbered = false;
    int  ran[2] = {0, 0};

    auto work = [&](void* marker, int idx) {
        t_shared_tls = marker;                    // this fiber's "g_tail_jmp"
        for (int i = 0; i < N; i++) {
            block(State::Ready);                  // yield; the other fiber overwrites t_shared_tls
            if (t_shared_tls != marker) clobbered = true;   // must be restored to OUR marker
            ran[idx]++;
        }
    };

    set_switch_hooks(tls_save, tls_restore);
    spawn(10, [&] { work((void*)0x1111, 0); }, /*start_ready=*/true);
    spawn(10, [&] { work((void*)0x2222, 1); }, /*start_ready=*/true);
    run_and_wait();
    set_switch_hooks(nullptr, nullptr);           // don't leak hooks past the test

    const bool pass = !clobbered && ran[0] == N && ran[1] == N;
    fprintf(stderr, "[nthr] per_fiber_tls:     %s  (clobbered=%s ran=%d,%d)\n",
            pass ? "PASS" : "FAIL", clobbered ? "YES" : "no", ran[0], ran[1]);
    return pass;
}
}  // namespace

bool self_test() {
    bool ok = true;
    ok &= test_round_robin();
    ok &= test_producer_consumer();
    ok &= test_fibers();
    ok &= test_per_fiber_tls();
    fprintf(stderr, "[nthr] self_test: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok;
}

}  // namespace nthr
