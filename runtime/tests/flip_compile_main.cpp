// STEP-0 compile-gate runner: the recomp-world TU that exercises the generated GAME TU. Lives in
// the SAME world as generated/functions_*.cpp (cpu_state.h + intrinsics.h, NO decomp headers), and
// provides minimal real implementations of the boundary runtime entry points so the flipped
// function can construct + field-access a host object without faulting. Proves the accessor split
// links AND runs (the construction/field thunks resolve to the port-compiled accessor TU).
#include "cpu_state.h"
#include <cstdio>
#include <cstdint>
#include <vector>

// Minimal engine-handle table (real eng_handle.cpp is the production one; this keeps the gate
// self-contained). C++ linkage to match runtime/intrinsics.h + runtime/eng_accessor_rt.h decls.
static std::vector<void*> g_tbl;
u32   sb_eng_handle(void* host) { g_tbl.push_back(host); return (u32)g_tbl.size(); }   // 1-based
void* sb_eng_host(u32 h)        { return (h && h <= g_tbl.size()) ? g_tbl[h - 1] : nullptr; }
void  sb_eng_release(void*)     {}
void* sb_guest_to_host(u32 ea)  { return (void*)(uintptr_t)ea; }
u32   sb_host_to_guest(void* p) { return (u32)(uintptr_t)p; }

// Bridged engine method (the synthetic body has a `bl storeTIMG`). In the real binary this
// dispatches to the override/recomp; here it's a no-op so the gate links.
void call_ppc(CPUState&, u32) {}

extern "C" void func_80100000(CPUState&);

int main() {
    CPUState cpu{};
    func_80100000(cpu);   // r3 = sbnew_JUTTexture(); inlined ctor + field accesses run on host obj
    std::printf("flip_compile gate: ran flipped function, r3(handle)=%u\n", cpu.gpr[3]);
    return 0;
}
