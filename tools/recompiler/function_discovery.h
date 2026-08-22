#pragma once

#include "ppc_decoder.h"

#include <vector>

// Build the list of functions from an entry point + linear scan.
// Returns function start addresses that lie INSIDE [base_addr, base_addr+size).
std::vector<u32> find_functions(const u8* text, u32 base_addr, u32 size);

// Every direct-call (bl/bcl) target in a text blob, WITHOUT clipping to that blob's
// own address range. A bl that crosses sections (.init calling into .text — which is
// exactly how __start reaches the game) is a genuine function entry, but a per-section
// scan discards it because the target is out of the scanned range. Callers union these
// across all text sections, then keep the ones landing in the section being emitted.
std::vector<u32> find_call_targets(const u8* text, u32 base_addr, u32 size);
