#pragma once
#include "cpu_state.h"

// Hook into Dolphin's JIT interface to redirect compiled blocks to our
// statically-recompiled native functions.
//
// Mechanism:
//   1. Load the recompiled shared library (generated/libsms_recomp.so)
//   2. Patch JitInterface::Compile to check g_recomp_table first
//   3. If found: call native function (after translating Dolphin PowerPC::State to CPUState)
//   4. If not found: fall through to Dolphin's normal JIT

// Call once after Dolphin Core is initialized and before starting emulation.
bool dolphin_hook_install(const char* recomp_lib_path);

// Uninstall hook (called on shutdown).
void dolphin_hook_uninstall();

// Build the direct-mapped dispatch table from the linked recomp table. Must be
// called once at startup so call_ppc resolves intra-recomp bl/returns directly
// (otherwise every call bounces to Dolphin's JIT with a full register-file copy).
void recomp_build_dispatch();

// Translate Dolphin's internal CPU state to our CPUState struct and back.
// Used at every recompiled function call boundary.
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/PowerPC/PowerPC.h"
void dolphin_state_to_cpu(const PowerPC::PowerPCState& src, CPUState& dst);
void cpu_to_dolphin_state(const CPUState& src, PowerPC::PowerPCState& dst);
#endif
