#pragma once
#include "cpu_state.h"

// Initialize standalone memory (call before running recompiled code)
// Not needed when HAVE_DOLPHIN_MEMMAP is set — Dolphin owns the memory.
void memory_bridge_init(const u8* initial_ram = nullptr, u32 size = 0);
