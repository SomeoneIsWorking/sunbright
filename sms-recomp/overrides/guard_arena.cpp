// guard_arena.cpp — the arena must never contain the stack the caller is standing on.
//
// This exists because the opposite went unnoticed for months. Our boot environment published
// BootInfo->arenaLo as the end of the DOL image, which sounds right and is not: the game's main
// stack lives immediately above the last section, in 64 KB that belongs to no section, so the
// arena began INSIDE the running stack. JKRExpHeap built the 128 KB system heap there, JKRThread
// stacks were allocated out of it on top of the main stack, and the first model deep enough to
// recurse 64 levels (Noki Bay's) wrote through a CMemBlock header, cut the heap's used list and
// lost 79 KB. What that looked like, three layers away, was "/scene/mapObj is missing from the
// archive" — a missing-asset bug. Nothing between the cause and that symptom could have been
// diagnosed as a memory-layout error.
//
// The check is one comparison, and it is the check that would have failed at boot instead: the
// caller's stack pointer is a live address inside a stack, so an arena that starts at or below it
// is an arena containing a stack. There is no threshold to tune and no per-DOL constant — every
// term is measured at the moment of the call.
//
// It cannot pass vacuously. A run in which OSSetArenaLo is never called reports that at shutdown
// rather than staying silent, because "no violation" and "never looked" are the same output
// otherwise, and this file's whole reason for existing is that the second one reads like the
// first. `SBR_ARENA_SELFTEST=1` feeds it a deliberately bad value that MUST be caught.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

extern "C" void func_803433ac(CPUState&);   // OSSetArenaLo

namespace {

unsigned long g_calls = 0;
u32 g_lowest = 0xFFFFFFFFu, g_last = 0;

// The guest stack pointer at the moment of the call is inside a stack by definition — it is the
// one this very call is using. So the rule is not "arena lo must be above some address we know",
// which would need a constant; it is "arena lo must be above a stack address we can see".
void check(u32 arena_lo, u32 sp, const char* what) {
    if (arena_lo == 0) return;   // "let the game choose" — OSInit resolves it to &__ArenaLo
    if (arena_lo > sp) return;

    lucent::error("arena",
                  "{}: arena lo {:#010x} is at or below the CALLER'S OWN STACK POINTER {:#010x}. "
                  "The arena is the game's heap, so this hands the allocator the stack that is "
                  "executing right now: the next few allocations land on live frames, and the "
                  "damage surfaces later and somewhere else entirely (issue #1 surfaced it as a "
                  "missing archive directory). The DOL's own __ArenaLo linker symbol is placed "
                  "above the stacks — publish BootInfo->arenaLo as 0 and let OSInit use it.",
                  what, arena_lo, sp);
    std::abort();
}

// OSSetArenaLo(void* lo)
void os_set_arena_lo(CPUState& cpu) {
    const u32 lo = cpu.gpr[3];
    ++g_calls;
    g_last = lo;
    if (lo != 0 && lo < g_lowest) g_lowest = lo;
    check(lo, (u32)cpu.gpr[1], "OSSetArenaLo");
    func_803433ac(cpu);
}

} // namespace

// Reported at shutdown by the runtime's final reports, so a silent run is distinguishable from a
// clean one — a guard that never ran is not a guard that passed.
void sbr_arena_guard_report() {
    if (g_calls == 0) {
        lucent::warn("arena", "the arena guard NEVER RAN: OSSetArenaLo was not called even once "
                              "this run, so nothing was checked. This is not a pass. Either the "
                              "run ended before OSInit or the override is not installed.");
        return;
    }
    lucent::info("arena",
                 "arena guard: {} OSSetArenaLo call(s) checked against the caller's live stack "
                 "pointer, lowest non-zero arena lo {:#010x}, last {:#010x} — none of them "
                 "enclosed a stack.",
                 g_calls, g_lowest == 0xFFFFFFFFu ? 0u : g_lowest, g_last);
}

// The self-test: a value that MUST be rejected. Without it, "no violation" is a claim about an
// instrument nobody has ever seen fire. It aborts on success, which is the point — it is run
// deliberately (SBR_ARENA_SELFTEST=1) and its expected outcome is the abort message above.
void sbr_arena_guard_selftest() {
    if (const char* e = std::getenv("SBR_ARENA_SELFTEST"); !e || e[0] != '1') return;
    lucent::info("arena", "SELF-TEST: feeding the guard an arena lo BELOW a stack pointer. The "
                          "correct outcome is the abort that follows; a run that continues past "
                          "this line means the guard cannot fire and every 'clean' report from it "
                          "is worthless.");
    check(/*arena_lo=*/0x80100000u, /*sp=*/0x80200000u, "SELF-TEST");
    lucent::error("arena", "SELF-TEST DID NOT FIRE — the guard accepted an arena that starts "
                           "1 MB below the stack pointer it was given.");
    std::abort();
}

SB_OVERRIDE(0x803433acu, os_set_arena_lo, "OSSetArenaLo",
            "guard: reject an arena that starts at or below the caller's own stack pointer "
            "(issue #1 — the arena was published over the main stack and the heap grew onto it)");
