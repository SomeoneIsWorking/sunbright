// guest_sched.h — cooperative guest threading for the standalone recomp.
//
// The GameCube is single-core and the game is written for it: exactly one guest thread runs
// at a time. We keep that property, but give each guest thread a real host thread and its
// own CPUState, so a thread can block in the middle of a recompiled function and resume
// there later — which a function-granular recompiler cannot express any other way.
//
// This is why the guest's own scheduler (SelectThread / OSLoadContext, which resume at an
// arbitrary saved PC) never runs: the blocking primitives are intercepted above it.

#pragma once

#include "cpu_state.h"

// The CPUState of whichever guest thread currently holds the token.
CPUState& gsched_cpu();

// Adopt the calling host thread as guest thread 0 (the one the DOL entry runs on).
void gsched_init(CPUState& main_cpu, u32 os_thread);

// A thread the game created. It starts parked; gsched_make_ready starts it running.
void gsched_create(u32 os_thread, u32 entry, u32 param, u32 stack, int priority);

// Blocking primitives, called from the OS overrides.
void gsched_block(u32 wait_queue);       // park the running thread until woken
void gsched_make_ready(u32 os_thread);   // a specific thread becomes runnable
void gsched_wake_queue(u32 wait_queue);  // everything parked on this queue becomes runnable
void gsched_drain();                    // park until nothing else is Ready (frame barrier)
void gsched_yield();                     // stay runnable, let an equal/higher priority run
void gsched_exit();                      // the running thread is finished

// Guest OSThread* of the running thread, or 0 if it is not a tracked guest thread.
u32 gsched_current_os_thread();

bool gsched_is_tracked(u32 os_thread);
bool gsched_is_dead(u32 os_thread);

// The value the thread body returned (its r3), for OSJoinThread.
u32  gsched_exit_value(u32 os_thread);
