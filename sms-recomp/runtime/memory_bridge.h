#pragma once
#include "cpu_state.h"

// Initialize standalone memory (call before running recompiled code)
// Not needed when HAVE_DOLPHIN_MEMMAP is set — Dolphin owns the memory.
void memory_bridge_init(const u8* initial_ram = nullptr, u32 size = 0);

// Set whenever a recompiled access touches a non-RAM address (MMIO, gather pipe).
// The differential validator uses this to ignore functions whose result depends on
// hardware-register reads — those legitimately differ between two separate runs.
extern bool g_recomp_touched_mmio;

// Points at the single CPUState threaded through the current top-level recomp call
// tree (set by sunbright_run_recomp_tree). Lets a fault handler — e.g. the wild-read
// trap — dump the live guest registers so we can see which base register held the bad
// pointer and walk the offending pointer chain. nullptr outside a recomp tree.
extern thread_local CPUState* g_cur_recomp_cpu;

// Set when a recompiled call tree yielded via the GC OS context-switch handoff
// (OSLoadContext → siglongjmp) instead of returning. Such a function did not "return"
// — it rescheduled — so the differential validator cannot compare it (the mutex-lock
// path in JKR heap alloc, etc.). The validator skips these, like MMIO functions.
extern bool g_recomp_context_switched;

// Abort with a guest-state + native backtrace dump for an unmapped guest access made by code
// running under Dolphin (the recomp wild-read/write traps only cover recomp-emitted accesses).
// main_sdl.cpp's MsgAlertHandler calls this when Dolphin's MMU PanicAlerts "Invalid read/write".
[[noreturn]] void sunbright_trap_invalid_access(u32 ea, u32 pc, bool write);
