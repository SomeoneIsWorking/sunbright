#pragma once
// Bridge between Dolphin's JIT and sunbright's recompiled native code.
// Called from Jit64::Jit() when the requested PC is in our recomp table.

#include <cstdint>

namespace SunbrightBridge {

// Initialize: load libsms_recomp.so and build the address lookup table.
// Call once from Jit64::Jit or Core startup.
bool Init(const char* recomp_lib_path);

// Returns true if 'pc' has a recompiled native function.
bool IsRecompiled(uint32_t pc);

// Run the recompiled function for 'pc', using Dolphin's live PowerPCState.
// Syncs state in/out. Call this instead of jit.Jit(pc) when IsRecompiled.
// Returns true on success.
bool Run(uint32_t pc);

void Shutdown();

}  // namespace SunbrightBridge
