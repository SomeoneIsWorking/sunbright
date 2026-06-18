#pragma once
#include "cpu_state.h"

// ── Native overrides & JIT-forced addresses ──────────────────────────────────
//
// Two escape hatches from pure static recompilation, resolved at runtime so no
// regeneration is needed:
//
//  1. Native override — a hand-written C++ replacement for a PPC function. Takes
//     precedence over the generated func_XXXX. Same calling convention: it gets a
//     CPUState& (already synced from Dolphin) and must exit via call_ppc(cpu, ...)
//     exactly like generated code. Use for functions our emitter gets wrong, or
//     to swap a routine for a faster native implementation (memcpy, matrix math…).
//
//  2. JIT-forced address — an address (or range) we deliberately route back to
//     Dolphin's JIT instead of running our recomp. Use for low-level OS/HW code
//     that depends on MMIO/SPR side effects the recompiler can't reproduce.
//
// Both are consulted by recomp_lookup() and SunbrightBridge, so they apply to
// JIT-entry, direct calls (bl), and indirect branches (bctr/blr) alike.

using RecompFunc = void (*)(CPUState&);

RecompFunc override_lookup(u32 addr);     // nullptr if none

// Register a native override. `group` is the source file the override lives in (filled in
// automatically by the macro below). Under the NO_RECOMP bisection envs the group lets whole
// override files be skipped (routed to Dolphin JIT) without touching code:
//   SUNBRIGHT_NORECOMP_SKIP=substr1,substr2  → do NOT register overrides whose file matches
//   SUNBRIGHT_NORECOMP_ONLY=substr1,substr2  → register ONLY overrides whose file matches
// Both are honored only when SUNBRIGHT_NO_RECOMP is set; matching is by filename substring.
void       register_override_impl(u32 addr, RecompFunc fn, const char* group);
// Function-like macro so EVERY call site (the SUNBRIGHT_OVERRIDE macro AND the direct
// register_override(...) calls scattered through runtime/overrides/) captures its __FILE__.
#define register_override(addr, fn) register_override_impl((addr), (fn), __FILE__)

// Super-call: the original generated body for `addr`, bypassing the override registered on it.
// Lets an override run the real function after observing/adjusting state (run the original draw
// after fixing 2D layout, capture transforms, etc.). nullptr if `addr` isn't recompiled.
RecompFunc recomp_raw(u32 addr);

// ── Pre-hooks (no-recomp / pure-JIT engine seam) ─────────────────────────────
// A pre-hook OBSERVES a guest function (reads its args/guest RAM to feed the native engine, e.g. ngx
// J3D capture) WITHOUT replacing it. Under the pure-JIT no-recomp mode the trampoline runs the
// pre-hook, then lets Dolphin JIT the ORIGINAL block (returns false). This is the conversion target
// for the WRAPPING overrides that today super-call recomp_raw — once recomp is gone there is no body
// to super-call, so they become observe-then-let-Dolphin-run-the-original. Observation only: the
// pre-hook must NOT change control flow or guest state it isn't deliberately patching.
using PreHookFn = void (*)(CPUState&);
PreHookFn  prehook_lookup(u32 addr);      // nullptr if none
void       register_prehook(u32 addr, PreHookFn fn);
bool       prehooks_registered();

// ── True no-recomp execution mode (SUNBRIGHT_PUREJIT) ─────────────────────────
// The target architecture (debug_journal/2026-06-18_no_recomp_jit_native_pivot.md): the static
// recompiler is OFF entirely — recomp_lookup/recomp_raw serve null, native_os (the nthr scheduler)
// is off, and the hybrid-era EXEC-MODEL overrides (VI pacing / GX-FIFO drawsync / host-clock
// governor) are off. Gameplay runs under Dolphin's own JIT (which owns OS/threading/pacing — proven
// by the DISABLE_RECOMP boot), and the PC-native ENGINE intercepts via PRE-HOOKS consulted in the
// JIT trampoline (observe guest state, then let Dolphin JIT the original block). This is the real
// form of what was the throwaway PUREJIT validation switch. Distinct from SUNBRIGHT_NO_RECOMP, the
// Phase-A half-measure that only flips the top-level JIT entry while recomp still dominates.
//
// Engine overrides cannot dispatch via Run() under this mode: nearly all of them carry a default-OFF
// super-call fallback (recomp_raw / call_ppc(cpu.lr)) that, with recomp gone, would route the caller
// continuation through the interpreter (which bypasses overrides). So the engine seam is pre-hooks
// only; full-replacement overrides are converted/re-grounded as purejit-aware seams per-override.
bool sunbright_purejit_mode();

bool is_jit_forced(u32 addr);
void force_jit(u32 addr);                 // single address
void force_jit_range(u32 lo, u32 hi);     // [lo, hi)

bool overrides_registered();              // any override registered?
bool jit_forced_registered();             // any forced-JIT range registered?

// Self-registering native override. Place in any .cpp linked into the launcher:
//
//   SUNBRIGHT_OVERRIDE(ov_my_memcpy, 0x80003abc) {
//       u32 dst = cpu.gpr[3], src = cpu.gpr[4], n = cpu.gpr[5];
//       ... ; call_ppc(cpu, cpu.lr); return;
//   }
//
#define SUNBRIGHT_OVERRIDE(name, addr)                                          \
    static void name(CPUState& cpu);                                            \
    static const bool name##_registered =                                       \
        (register_override((addr), &name), true);                               \
    static void name(CPUState& cpu)

// Conditional variant — register the override ONLY when `cond` is true at static-init time.
// For FEATURE/observer overrides (interp60, debug tracers): when the feature is off they would
// otherwise just super-call the recompiled body, which under the no-recomp pivot needlessly forces
// the whole subtree (frame loop / director / scene graph) through recomp instead of letting Dolphin
// JIT the original. Not registering them when off routes the original to the JIT. (`cond` is
// evaluated once; getenv-based gates are fine at static init.)
#define SUNBRIGHT_OVERRIDE_IF(name, addr, cond)                                 \
    static void name(CPUState& cpu);                                            \
    static const bool name##_registered =                                       \
        ((cond) ? (register_override((addr), &name), true) : false);           \
    static void name(CPUState& cpu)
