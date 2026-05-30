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
void       register_override(u32 addr, RecompFunc fn);

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
