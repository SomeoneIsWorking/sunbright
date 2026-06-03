#pragma once
#include <cstdint>
#include <functional>

// Native OS-threading core — see docs/native_threading.md.
//
// This is a PC port, so the GameCube OS's software threads become REAL host threads
// instead of the PPC scheduler context-switching on one emulated CPU. Because the
// emulated machine is single-core (and Dolphin's core is not safe to drive from many
// host threads at once), exactly one guest thread runs at a time: it holds the "CPU
// token." A thread that blocks parks on a condition variable — no busy-spin, no step
// budget, no timing dependence — and the scheduler hands the token to the
// highest-priority ready thread (lower priority number = higher priority, as in the GC
// OS). When a blocked thread is woken, it becomes Ready and is picked at the next yield
// (cooperative, single-core semantics).
//
// This module is the substrate. The OS-primitive overrides (OSCreateThread,
// OSResumeThread, OSSleepThread, message queues, ...) are layered on top in later
// steps; each maps a guest call onto these primitives.

namespace nthr {

enum class State { Ready, Running, Blocked, Dead };

struct GuestThread;  // opaque (defined in native_threads.cpp)

// The guest thread currently executing on this host thread (null on a non-guest host).
GuestThread* current();
int          priority_of(GuestThread*);

// Create a guest thread that runs `body` on its own host thread once it is granted the
// CPU token. `start_ready` = becomes schedulable immediately; otherwise it starts
// Blocked (suspended) until make_ready().
GuestThread* spawn(int priority, std::function<void()> body, bool start_ready);

// The current thread yields the CPU token until rescheduled. `newState` is Ready to
// round-robin (stay schedulable) or Blocked to sleep until something makes it Ready.
void block(State newState);

// Mark a Blocked thread Ready (a wakeup/resume). Cooperative: the running thread keeps
// the token until it next yields, then the scheduler may pick the woken thread.
void make_ready(GuestThread*);

// ── Per-context switch hooks ─────────────────────────────────────────────────
// The PPC register file is a single GLOBAL (`ppc`) that run_jit_sync drives, so even though
// each guest thread is its own host thread, the running thread's registers live in that one
// global and must be swapped on every token hand-off. nthr gives each thread one opaque
// `user` slot and invokes the registered hooks around every switch: `save(self)` on the
// thread giving up the token, `restore(t)` for the thread about to get it. The runtime
// registers hooks that move the global register file into/out of that slot, so each thread's
// registers follow it. nthr itself stays agnostic to what is saved. See
// docs/native_threading.md step 1.
void   set_switch_hooks(void (*save)(GuestThread*), void (*restore)(GuestThread*));
void*& user_slot(GuestThread*);

// Bootstrap entry: the calling (non-guest) host thread grants the token to the first
// ready thread and blocks until every spawned thread is Dead, then joins them. Used by
// the self-test and the initial hand-off.
void run_and_wait();

// SUNBRIGHT_NTHR_SELFTEST: validate the token hand-off in isolation (no game state).
// Returns true on success.
bool self_test();

}  // namespace nthr
