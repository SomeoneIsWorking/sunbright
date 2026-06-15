#!/usr/bin/env bash
# STEP-0 COMPILE GATE (docs/re_notes/j3d_subsystem_ownership_plan.md STEP 0, Option A).
#
# Proves the previously-BLOCKING finding is FIXED: a tailored engine-types flip now compiles +
# links into the binary. The split (Option A):
#   * the generated GAME TU calls extern accessor thunks and is compiled WITHOUT the decomp headers
#     -> structurally cannot hit the cpu_state.h <-> decomp `u64` typedef collision that gated it;
#   * the generated ACCESSOR TU bakes the host field offsets / ctor sizes BY NAME and is compiled
#     WITH the decomp headers + shims (the port/ environment) but WITHOUT cpu_state.h.
# Then both link against the boundary runtime entry points and the flipped function runs.
#
# No Dolphin, no DOL needed (the synthetic function is seeded with the confirmed JUTTexture layout).
set -euo pipefail
cd "$(dirname "$0")/../.."          # repo root
out=scratch/bin
gen_out=scratch/port
mkdir -p "$out" "$gen_out"

DECOMP_INC=reference/sms/include
COMPAT_INC=port/compat/include
SHIMS=(-include port/compat/intrinsics.h -include port/compat/msl_shim.h)

echo "== stage 1: build + run the generator (real emitter + type DB) =="
g++ -std=c++20 -O0 -g -Wall -Wextra \
    -I tools/recompiler \
    runtime/tests/flip_compile_gen.cpp \
    tools/recompiler/c_emitter.cpp \
    tools/recompiler/ppc_decoder.cpp \
    tools/recompiler/ppc_mnemonic.cpp \
    tools/recompiler/func_collect.cpp \
    tools/recompiler/type_recovery.cpp \
    tools/recompiler/type_db_build.cpp \
    tools/recompiler/decomp_parse.cpp \
    tools/recompiler/func_sig.cpp \
    tools/recompiler/abi_layout.cpp \
    -o "$out/flip_compile_gen"
"$out/flip_compile_gen" "$gen_out"

echo
echo "== stage 2: compile the GAME TU WITHOUT the decomp headers (the collision can't occur) =="
# Include path: runtime/ (cpu_state.h, intrinsics.h, eng_accessor_rt.h) + the generated dir
# (eng_accessors.h). Deliberately NO -I reference/sms/include -> if a host struct leaked into the
# generated code it would fail to compile here.
g++ -std=c++20 -O0 -g -Wall -Wextra \
    -I runtime -I "$gen_out" \
    -c "$gen_out/flip_funcs.cpp" -o "$out/flip_funcs.o"
echo "  ok: GAME TU compiled with no decomp headers in scope"

echo
echo "== stage 3: compile the ACCESSOR TU WITH the decomp headers + shims (the port world) =="
# Mirror port/CMakeLists.txt: compat shadow headers win, then the pristine decomp, force-include
# the CodeWarrior-builtin + MSL shims. -I runtime is LAST and only supplies eng_accessor_rt.h.
g++ -std=c++17 -O0 -g \
    -I "$COMPAT_INC" -I "$DECOMP_INC" -I runtime \
    "${SHIMS[@]}" \
    -fno-strict-aliasing -Wno-multichar -Wno-register -w \
    -c "$gen_out/eng_accessors.cpp" -o "$out/eng_accessors.o"
echo "  ok: ACCESSOR TU compiled with the real decomp headers (no cpu_state.h)"

echo
echo "== stage 4: link both TUs + the boundary runtime + run =="
g++ -std=c++20 -O0 -g -I runtime \
    runtime/tests/flip_compile_main.cpp \
    "$out/flip_funcs.o" "$out/eng_accessors.o" \
    -o "$out/flip_compile_test"
"$out/flip_compile_test"

echo
echo "FLIP COMPILE GATE: PASS — a tailored flip compiles, links, and runs."
