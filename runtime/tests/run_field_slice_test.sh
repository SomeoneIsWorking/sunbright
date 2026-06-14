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
    -o "$out/field_slice_gen"
"$out/field_slice_gen" "$gen_out"

echo
echo "== stage 2: structural checks on the emitter output =="
fail=0
chk() { if eval "$2"; then echo "  ok: $1"; else echo "FAIL: $1"; fail=1; fi; }
# TAILORED must read/write the HOST member through sb_eng_host, NOT guest MEM.
chk "tailored bakes host field read  (->mFov via sb_eng_host)" \
    "grep -q 'sb_eng_host(cpu.gpr\[3\]))->mFov' $gen_out/slice_tailored.inc"
chk "tailored bakes host field write (->mFov via sb_eng_host)" \
    "grep -q 'sb_eng_host(cpu.gpr\[31\]))->mFov' $gen_out/slice_tailored.inc"
chk "tailored does NOT MEM_RF32 the engine field" \
    "! grep -q 'MEM_RF32' $gen_out/slice_tailored.inc"
chk "tailored still calls the engine fn through the boundary (call_ppc)" \
    "grep -q 'call_ppc(cpu, 0x80009000u)' $gen_out/slice_tailored.inc"
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
