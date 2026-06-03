#include "cpu_state.h"
#include <cstdio>
#include <cstring>

// GameCube OS High-Level Emulation.
// Most OS calls are handled by Dolphin's existing HLE layer when running under Dolphin.
// This file covers calls made through the SC (syscall) instruction directly,
// and common SDK functions that are cheaper to HLE than to emulate.

// OSReport — maps to fprintf(stderr, ...)
// r3 = format string pointer, r4..r10 = args (game uses GC printf ABI)
static void hle_os_report(CPUState& cpu) {
    // In standalone mode, we can't easily dereference GC pointers.
    // Log the PC and return.
    fprintf(stderr, "[OSReport] called from 0x%08x\n", cpu.pc);
    // TODO: when memory bridge is live, dereference cpu.gpr[3] as char*
}

// OSPanic — game hit an assert
static void hle_os_panic(CPUState& cpu) {
    fprintf(stderr, "[OSPanic] Game panicked at 0x%08x\n", cpu.pc);
    // Don't terminate — let caller handle it
}

// OSGetTime — return a fake time (nanoseconds since boot)
static uint64_t g_fake_time = 0;
static void hle_os_get_time(CPUState& cpu) {
    g_fake_time += 16666667;  // ~60fps
    cpu.gpr[3] = (u32)(g_fake_time >> 32);
    cpu.gpr[4] = (u32)(g_fake_time);
}

// Dispatch table: address (GC vaddr) → HLE handler
// Populated with symbols from a symbol map once we have one.
struct HLEEntry {
    u32 addr;
    void (*fn)(CPUState&);
    const char* name;
};

static HLEEntry g_hle_table[] = {
    // Addresses are SMS-specific — update from symbol map
    // { 0x803A0000, hle_os_report, "OSReport" },
    // { 0x803A0100, hle_os_panic,  "OSPanic"  },
};

#ifdef HAVE_DOLPHIN_CORE
extern void sunbright_run_syscall(CPUState& cpu, u32 sc_pc);   // dolphin_hook.cpp
#endif

void os_hle_call(CPUState& cpu, u32 address) {
    for (const auto& e : g_hle_table) {
        if (e.addr == address) {
            e.fn(cpu);
            return;
        }
    }
#ifdef HAVE_DOLPHIN_CORE
    // `sc` at `address`: run it under Dolphin so the OS syscall exception handler actually executes
    // (the recompiler stubs every sc to this call). Was a silent no-op, which stalled the game.
    sunbright_run_syscall(cpu, address);
#else
    fprintf(stderr, "[os_hle] unhandled call at 0x%08x\n", address);
#endif
}
