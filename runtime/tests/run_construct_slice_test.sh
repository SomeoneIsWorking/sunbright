#!/usr/bin/env bash
# Object-identity slice (engine-CONSTRUCTION half of the boundary; docs/re_notes/object_identity.md).
# Stage 1: run the generator (real decoder + recovery + emitter) -> two C bodies (ORACLE/TAILORED).
# Stage 2: structural checks on the emitted output (operator new rewritten; inlined writes host-typed).
# Stage 3: compile + run the end-to-end verification (TAILORED host object == ORACLE guest buffer).
# No Dolphin / no full build. Plain g++ C++17.
set -euo pipefail
cd "$(dirname "$0")/../.."          # repo root
out=scratch/bin
gen_out=scratch/port
mkdir -p "$out" "$gen_out"

echo "== stage 1: generate (real decoder + emitter) =="
g++ -std=c++17 -O0 -g -Wall -Wextra \
    -I tools/recompiler \
    runtime/tests/construct_slice_gen.cpp \
    tools/recompiler/c_emitter.cpp \
    tools/recompiler/ppc_decoder.cpp \
    tools/recompiler/ppc_mnemonic.cpp \
    tools/recompiler/func_collect.cpp \
    tools/recompiler/type_recovery.cpp \
    -o "$out/construct_slice_gen"
"$out/construct_slice_gen" "$gen_out"

echo
echo "== stage 2: structural checks on the emitter output =="
fail=0
chk() { if eval "$2"; then echo "  ok: $1"; else echo "FAIL: $1"; fail=1; fi; }
chk "tailored rewrites operator new -> sb_eng_alloc<EngineTex>()" \
    "grep -q 'cpu.gpr\[3\] = sb_eng_alloc<EngineTex>()' $gen_out/construct_tailored.inc"
chk "tailored does NOT call the guest operator new" \
    "! grep -q 'call_ppc(cpu, 0x80009100u)' $gen_out/construct_tailored.inc"
chk "tailored writes mWidth to the HOST member" \
    "grep -q 'sb_eng_host(cpu.gpr\[3\]))->mWidth = ' $gen_out/construct_tailored.inc"
chk "tailored writes mEmbPalette to the HOST member" \
    "grep -q 'sb_eng_host(cpu.gpr\[3\]))->mEmbPalette = ' $gen_out/construct_tailored.inc"
chk "tailored still calls the type-revealing engine method (bridged)" \
    "grep -q 'call_ppc(cpu, 0x80009200u)' $gen_out/construct_tailored.inc"
chk "Pattern A: tailored out-of-line ctor (recompiled) writes the HOST mWidth member" \
    "grep -q 'sb_eng_host(cpu.gpr\[3\]))->mWidth = ' $gen_out/construct_tailored.inc"
chk "Pattern A: tailored caller still calls the out-of-line ctor (no special bridge)" \
    "grep -q 'call_ppc(cpu, 0x80022000u)' $gen_out/construct_tailored.inc"
chk "Pattern C: tailored declares a RAII host SbStackObj for the frame slot" \
    "grep -q 'SbStackObj<EngineTex> _eng_stack_' $gen_out/construct_tailored.inc"
chk "Pattern C: tailored addi yields the shared stack-object handle" \
    "grep -q '_eng_stack_[0-9]*.handle()' $gen_out/construct_tailored.inc"
chk "Pattern C: oracle keeps the interior addi as raw stack arithmetic" \
    "grep -q 'cpu.gpr\[3\] = cpu.gpr\[1\] + 40' $gen_out/construct_oracle.inc"
chk "oracle calls the real operator new" \
    "grep -q 'call_ppc(cpu, 0x80009100u)' $gen_out/construct_oracle.inc"
chk "oracle uses raw guest MEM for the inlined writes" \
    "grep -q 'MEM_W32' $gen_out/construct_oracle.inc"
chk "oracle has NO host construction" \
    "! grep -q 'sb_eng_alloc' $gen_out/construct_oracle.inc"
[ "$fail" = 0 ] || { echo "structural checks FAILED"; exit 1; }

echo
echo "== stage 3: compile + run the end-to-end verification =="
g++ -std=c++17 -O0 -g -Wall -Wextra \
    runtime/tests/construct_slice_test.cpp \
    -o "$out/construct_slice_test"
"$out/construct_slice_test"
