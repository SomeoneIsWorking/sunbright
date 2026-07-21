// aram.h — direct access to the ARAM buffer for seams above the register level.
// See dev_aram.cpp.
#pragma once

#include "cpu_state.h"

// Move `len` bytes between main memory and ARAM. `to_mram` selects the direction.
// Addresses are guest addresses; ARAM addresses wrap at the 16 MB boundary, as on hardware.
void aram_dma(u32 mram_addr, u32 aram_addr, u32 len, bool to_mram);
