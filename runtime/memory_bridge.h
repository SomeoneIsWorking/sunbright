#pragma once
#include "cpu_state.h"

// Initialize standalone memory (call before running recompiled code)
// Not needed when HAVE_DOLPHIN_MEMMAP is set — Dolphin owns the memory.
void memory_bridge_init(const u8* initial_ram = nullptr, u32 size = 0);

// Set whenever a recompiled access touches a non-RAM address (MMIO, gather pipe).
// The differential validator uses this to ignore functions whose result depends on
// hardware-register reads — those legitimately differ between two separate runs.
extern bool g_recomp_touched_mmio;
