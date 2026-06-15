#!/usr/bin/env bash
# End-to-end mechanism proof for offset-0 virtual-dispatch-on-handle routing
# (docs/ARCHITECTURE_TARGET.md function-call boundary; handoff step 3 verify).
#
# Proves the TWO-WORLD split the routing relies on:
#   * the GENERATED GAME TU (game world: runtime/cpu_state.h, NO decomp headers) marshals the
#     handle from r3 and calls an `extern "C"` thunk — exactly what the emitter produces at a
#     routed bctrl (`sbvirt_<T>_<slot>(cpu.gpr[3])`);
#   * the PORT-WORLD THUNK TU (decomp/host headers — here a fake J3DModel carrying the decomp's
#     `typedef unsigned long long u64`, NO cpu_state.h) resolves the handle via sb_eng_host and
#     calls the HOST virtual method `((J3DModel*)host)->calc()`.
# Separate TUs => the cpu_state.h-vs-decomp u64 type collision (STEP 0) is structurally absent,
# and the call dispatches to the real host vtable through the handle table.
#
# Run from the repo root. Artifacts in scratch/virt/ (never /tmp).
set -euo pipefail
cd "$(dirname "$0")/../.."
W=scratch/virt
mkdir -p "$W"

# ── port world: fake decomp J3DModel + handle resolver + the generated thunk ────────────────
cat > "$W/fake_j3d.h" <<'EOF'
#pragma once
#include <cstdint>
typedef unsigned long long u64;   // the decomp types.h definition (clashes with cpu_state.h)
struct J3DModel {
    int calc_count = 0;
    virtual void update() {}
    virtual void entry()  {}
    virtual void calc()   { ++calc_count; }   // slot 2 — the routed method
    virtual void viewCalc(){}
    virtual ~J3DModel() {}
};
EOF

cat > "$W/port_thunks.cpp" <<'EOF'
// PORT WORLD: host types, NO cpu_state.h. This is generated/virt_thunks.cpp's shape.
#include <cstdint>
#include "fake_j3d.h"
extern void* sb_eng_host(std::uint32_t);     // runtime/eng_handle.cpp (C++ linkage)
extern "C" void sbvirt_J3DModel_2(std::uint32_t h) {  // the generated thunk
    ((J3DModel*)sb_eng_host(h))->calc();
}
// test scaffolding (also port world, owns the host object + a tiny handle table)
static J3DModel* g_obj = nullptr;
void* sb_eng_host(std::uint32_t h) { return (h == 0x90000001u) ? (void*)g_obj : nullptr; }
extern "C" std::uint32_t test_make_obj()              { g_obj = new J3DModel(); return 0x90000001u; }
extern "C" int          test_calc_count(std::uint32_t){ return g_obj ? g_obj->calc_count : -1; }
EOF

# ── game world: the emitted call site (cpu_state.h, NO decomp headers) ───────────────────────
cat > "$W/game_call.cpp" <<'EOF'
// GAME WORLD: cpu_state.h, NO decomp headers — what a generated functions_*.cpp looks like.
#include "cpu_state.h"
extern "C" void sbvirt_J3DModel_2(std::uint32_t);   // from generated/virt_thunks.h
extern "C" void game_virtual_call(CPUState& cpu) {  // emitter output for a routed bctrl
    cpu.lr = 0x80100004u;
    sbvirt_J3DModel_2(cpu.gpr[3]);
}
EOF

cat > "$W/driver.cpp" <<'EOF'
// GAME WORLD driver (cpu_state.h, no decomp headers).
#include "cpu_state.h"
#include <cstdio>
extern "C" std::uint32_t test_make_obj();
extern "C" int          test_calc_count(std::uint32_t);
extern "C" void         game_virtual_call(CPUState&);
int main() {
    std::uint32_t h = test_make_obj();
    CPUState cpu{};
    cpu.gpr[3] = h;                       // the engine handle in r3 (the `this` arg)
    game_virtual_call(cpu);               // routed bctrl -> thunk -> host J3DModel::calc()
    int n = test_calc_count(h);
    if (n != 1) { std::printf("run_virt_dispatch_test: FAIL (calc_count=%d, want 1)\n", n); return 1; }
    std::printf("run_virt_dispatch_test: PASS (host J3DModel::calc dispatched via handle)\n");
    return 0;
}
EOF

CXX="${CXX:-g++}"
$CXX -std=c++17 -Wall -Wextra -I runtime -c "$W/port_thunks.cpp" -o "$W/port_thunks.o"
$CXX -std=c++17 -Wall -Wextra -I runtime -c "$W/game_call.cpp"   -o "$W/game_call.o"
$CXX -std=c++17 -Wall -Wextra -I runtime -c "$W/driver.cpp"      -o "$W/driver.o"
$CXX "$W/port_thunks.o" "$W/game_call.o" "$W/driver.o" -o "$W/virt_dispatch_test"
"$W/virt_dispatch_test"
