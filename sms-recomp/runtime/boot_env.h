// boot_env.h — publish the low-memory state a real GameCube boot leaves behind.
// See boot_env.cpp. Must run after the disc is mounted and after guest RAM exists.
#pragma once

#include "cpu_state.h"

// `arena_lo` is the end of everything the DOL occupies (sections and BSS). It is used for the
// LOG LINE ONLY: BootInfo->arenaLo is published as zero, because that is what a disc boot leaves
// and because the end of the DOL image is the WRONG place for the arena to begin — the game's
// stacks live above it and are not in any section. See boot_env.cpp.
bool boot_env_setup(u32 dol_end);
u32  boot_env_fst_addr();
