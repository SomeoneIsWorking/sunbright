// Weak fallback for the recompiled function table.
//
// The real table lives in generated/jump_table.cpp and is linked into the
// sunbright binary when it exists (its strong symbols override these weak ones).
// Before /recompile has been run, these weak empties let the binary still link
// and run — everything just falls through to Dolphin's JIT.

#include "cpu_state.h"
#include <cstddef>

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

extern "C" __attribute__((weak)) const JumpEntry g_recomp_table[] = {{0, nullptr}};
extern "C" __attribute__((weak)) const size_t    g_recomp_table_size = 0;
