#!/usr/bin/env bash
# Standalone test for the TAILORED-RECOMP de-risk slice (the field-access half of the
# game<->engine boundary). The GATING experiment of docs/ARCHITECTURE_TARGET.md.
#
# Stage 1: build + run the generator, which runs the slice's game function through the
#          REAL recompiler (decoder + collection + emitter) and writes two C bodies:
#          ORACLE (guest-layout MEM) and TAILORED (host-native field access).
# Stage 2: structurally assert the two bodies differ in the expected way.
# Stage 3: compile the two bodies into the runner and verify TAILORED == ORACLE.
#
# No Dolphin / no full build. Plain g++ C++17.
set -euo pipefail
cd "$(dirname "$0")/../.."          # repo root
out=scratch/bin
gen_out=scratch/port
mkdir -p "$out" "$gen_out"

echo "== stage 1: generate (real decoder + emitter) =="
g++ -std=c++17 -O0 -g -Wall -Wextra \
    -I tools/recompiler \
    runtime/tests/field_slice_gen.cpp \
    tools/recompiler/c_emitter.cpp \
    tools/recompiler/ppc_decoder.cpp \
    tools/recompiler/ppc_mnemonic.cpp \
    tools/recompiler/func_collect.cpp \
    tools/recompiler/type_recovery.cpp \
    tools/recompiler/abi_layout.cpp \
    tools/recompiler/type_db.cpp \
    -o "$out/field_slice_gen"
"$out/field_slice_gen" "$gen_out"

echo
echo "== stage 2: structural checks on the emitter output =="
fail=0
chk() { if eval "$2"; then echo "  ok: $1"; else echo "FAIL: $1"; fail=1; fi; }
# TAILORED game TU (Option A): field accesses are CALLS to accessor thunks; the host type is
# named ONLY in the separate accessor defs (slice_accessors.inc), never in the game code.
chk "tailored loads the nested engine ptr via the lwzp accessor thunk (-> HANDLE)" \
    "grep -q 'sbf_EngineCam_mNext_lwzp_' $gen_out/slice_tailored.inc"
chk "tailored chains: reads ->mFov through the nested handle (base r4) via the lfs accessor" \
    "grep -Eq 'sbf_EngineCam_mFov_lfs_[0-9a-f]+\(cpu.gpr\[4\]\)' $gen_out/slice_tailored.inc"
chk "tailored writes ->mFov through the RELOADED this (base r31) via the stfs accessor" \
    "grep -Eq 'sbf_EngineCam_mFov_stfs_[0-9a-f]+\(cpu.gpr\[31\]' $gen_out/slice_tailored.inc"
chk "tailored game code NAMES no host struct type (compiles without decomp headers)" \
    "! grep -q '((EngineCam\*)' $gen_out/slice_tailored.inc && ! grep -q 'sb_eng_host' $gen_out/slice_tailored.inc"
chk "tailored does NOT MEM_RF32 the engine field" \
    "! grep -q 'MEM_RF32' $gen_out/slice_tailored.inc"
chk "tailored still spills/reloads the handle via guest MEM (the stack slot is NOT typed)" \
    "grep -q 'MEM_W32(cpu.gpr\[1\] + 8' $gen_out/slice_tailored.inc"
chk "tailored still calls the engine fn through the boundary (call_ppc)" \
    "grep -q 'call_ppc(cpu, 0x80009000u)' $gen_out/slice_tailored.inc"
# The accessor DEFS are where the host member is baked by name (the port-compiled TU).
chk "accessor defs bake the host member access by name (->mNext, ->mFov)" \
    "grep -q '((EngineCam\*)sb_eng_host(h))->mNext' $gen_out/slice_accessors.inc && grep -q '((EngineCam\*)sb_eng_host(h))->mFov' $gen_out/slice_accessors.inc"
# ORACLE must use raw guest-layout MEM (the baseline the recompiler emits today).
chk "oracle uses guest-layout MEM_RF32" \
    "grep -q 'MEM_RF32' $gen_out/slice_oracle.inc"
chk "oracle has NO host field access" \
    "! grep -q 'sb_eng_host' $gen_out/slice_oracle.inc"
[ "$fail" = 0 ] || { echo "structural checks FAILED"; exit 1; }

echo
echo "== stage 3: compile + run the end-to-end verification =="
g++ -std=c++17 -O0 -g -Wall -Wextra \
    runtime/tests/field_slice_test.cpp \
    -o "$out/field_slice_test"
"$out/field_slice_test"
