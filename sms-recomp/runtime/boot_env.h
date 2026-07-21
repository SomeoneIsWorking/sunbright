// boot_env.h — publish the low-memory state a real GameCube boot leaves behind.
// See boot_env.cpp. Must run after the disc is mounted and after guest RAM exists.
#pragma once

#include "cpu_state.h"

// `arena_lo` must be the end of everything the DOL occupies (sections and BSS). The arena
// is the game's heap; starting it any lower means the heap allocates on top of the game's
// own code and data.
bool boot_env_setup(u32 arena_lo);
u32  boot_env_fst_addr();
