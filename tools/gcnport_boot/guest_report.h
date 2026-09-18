// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/CommonTypes.h"

namespace Memory {
class MemoryManager;
}

namespace sunbright::gcnport_boot {

// Reading guest state and printing it, separated from the run that decides when to ask.
//
// These three are pure readers over guest memory: they take an address, walk a structure the
// GameCube ABI or the SDK defines, and print what is there. None of them touches the runtime, the
// boot request or the dispatch loop, and each refuses an address that is not word-aligned guest RAM
// rather than printing a window of whatever happens to be mapped.

// Walks the PowerPC ABI's saved-stack-pointer chain from `stack_pointer`, printing each frame's
// return address. Stops at a frame outside guest RAM or after a fixed depth.
void ReportGuestBacktrace(const Memory::MemoryManager& memory, u32 stack_pointer);

// Prints `words` big-endian words at `address`, following plausible guest pointers to a bounded
// depth so an object header and its first members are legible in one report.
void ReportGuestMemory(const Memory::MemoryManager& memory, u32 address, u32 words, u32 depth);

// Walks the SDK's active-thread queue from its low-memory head, printing each thread's state,
// priority and suspend count.
void ReportActiveThreads(const Memory::MemoryManager& memory);

} // namespace sunbright::gcnport_boot
