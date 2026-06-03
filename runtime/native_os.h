#pragma once
#include "cpu_state.h"

// Native GameCube OS primitive set (GMSE01) — see docs/native_threading.md.
//
// These are genuinely-native reimplementations of OS thread/sync primitives, kept in a
// DEDICATED set separate from the general SUNBRIGHT_OVERRIDE table. The general table is
// only consulted on the recomp path (recomp_lookup); but the audio-init region runs under
// Dolphin's interpreter inside run_jit_sync (entered at an interior addr), so an OS primitive
// reached there would never hit a general override. call_ppc therefore consults THIS set on
// both the recomp call path AND inside the run_jit_sync interpreter loop, so a primitive fires
// regardless of which backend reaches it. (The sms_os_intr.cpp interrupt overrides stay
// recomp-path-only — they must NOT fire under the interpreter.)
//
// A primitive operates on a CPUState (the interpreter path converts ppc<->CPUState around the
// call) and returns to the guest caller's LR, exactly like the recompiled function's blr.

using NativeOSFn = void (*)(CPUState& cpu);

void       native_os_register(u32 addr, NativeOSFn fn);
NativeOSFn native_os_lookup(u32 addr);   // null if no native primitive at addr
void       native_os_init();             // register the primitive set (idempotent)
