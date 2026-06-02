#include "native_threads.h"

#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <mutex>
#include <thread>
#include <ucontext.h>
#include <vector>

namespace nthr {

struct GuestThread {
    std::thread             host;
    std::condition_variable cv;        // parked here until granted the token
    bool                    granted = false;
    State                   state   = State::Blocked;
    int                     prio    = 16;
    uint64_t                ready_seq = 0;   // FIFO order among equal-priority Ready
    std::function<void()>   body;
};

namespace {

std::mutex                 g_mx;        // guards all scheduler state below
std::vector<GuestThread*>  g_threads;
GuestThread*               g_running = nullptr;
uint64_t                   g_seq = 0;
std::condition_variable    g_all_done_cv;   // signalled when no thread is alive

thread_local GuestThread*  tls_current = nullptr;

// Pick the highest-priority Ready thread (lowest prio number; FIFO among ties) and
// grant it the token. If none is Ready: signal all-done when nothing is alive, else
// report a deadlock (a thread blocked with no one able to wake it — a bug in this
// minimal core, which has no idle/driver yet). Must be called holding g_mx.
void pick_next_locked() {
    GuestThread* best = nullptr;
    for (auto* t : g_threads) {
        if (t->state != State::Ready) continue;
        if (!best || t->prio < best->prio ||
            (t->prio == best->prio && t->ready_seq < best->ready_seq))
            best = t;
    }
    if (best) {
        best->state   = State::Running;
        best->granted = true;
        g_running     = best;
        best->cv.notify_one();
        return;
    }
    g_running = nullptr;
    bool any_alive = false;
    for (auto* t : g_threads)
        if (t->state != State::Dead) { any_alive = true; break; }
    if (!any_alive)
        g_all_done_cv.notify_all();
    else
        fprintf(stderr, "[nthr] deadlock: no Ready thread but %zu still alive\n",
                g_threads.size());
}

}  // namespace

GuestThread* current()                 { return tls_current; }
int          priority_of(GuestThread* t) { return t->prio; }

GuestThread* spawn(int priority, std::function<void()> body, bool start_ready) {
    auto* gt = new GuestThread;
    gt->prio  = priority;
    gt->body  = std::move(body);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        gt->state     = start_ready ? State::Ready : State::Blocked;
        gt->ready_seq = g_seq++;
        g_threads.push_back(gt);
    }
    gt->host = std::thread([gt] {
        {   // park until the scheduler grants us the token
            std::unique_lock<std::mutex> lk(g_mx);
            gt->cv.wait(lk, [gt] { return gt->granted; });
        }
        tls_current = gt;
        gt->body();                       // runs holding the token (g_mx not held)
        std::lock_guard<std::mutex> lk(g_mx);
        gt->state   = State::Dead;
        gt->granted = false;
        pick_next_locked();               // hand the token to the next ready thread
    });
    return gt;
}

void block(State newState) {
    GuestThread* self = tls_current;
    std::unique_lock<std::mutex> lk(g_mx);
    self->state     = newState;
    self->granted   = false;
    self->ready_seq = g_seq++;            // go to the back of the FIFO (round-robin)
    pick_next_locked();                   // grant the token to someone else
    self->cv.wait(lk, [self] { return self->granted; });  // park until rescheduled
}

void make_ready(GuestThread* t) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (t->state == State::Blocked) {
        t->state     = State::Ready;
        t->ready_seq = g_seq++;
    }
}

void run_and_wait() {
    std::unique_lock<std::mutex> lk(g_mx);
    if (!g_running) pick_next_locked();   // grant the token to the first ready thread
    g_all_done_cv.wait(lk, [] {
        for (auto* t : g_threads)
            if (t->state != State::Dead) return false;
        return true;
    });
    std::vector<GuestThread*> snapshot = g_threads;
    lk.unlock();
    for (auto* t : snapshot) if (t->host.joinable()) t->host.join();
    lk.lock();
    for (auto* t : snapshot) delete t;
    g_threads.clear();
    g_running = nullptr;
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
}  // namespace

bool self_test() {
    bool ok = true;
    ok &= test_round_robin();
    ok &= test_producer_consumer();
    ok &= test_fibers();
    fprintf(stderr, "[nthr] self_test: %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok;
}

}  // namespace nthr
