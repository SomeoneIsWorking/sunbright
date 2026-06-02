#include "native_threads.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
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

bool self_test() {
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
    fprintf(stderr, "[nthr] self_test: %s  (concurrent=%s  total=%d/%d)\n",
            pass ? "PASS" : "FAIL", concurrent ? "YES" : "no", total.load(), 2 * N);
    return pass;
}

}  // namespace nthr
