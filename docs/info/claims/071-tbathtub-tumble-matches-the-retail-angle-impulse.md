---
id: C071
kind: claim
status: holds
created: 2026-08-26
tags: decomp,re,movebg
depends: decomp/sms/src/MoveBG/MapObjCorona.cpp#TBathtub::tumble, sms-boot/runtime/tests/bathtub_tumble_test.cpp
---

## Claim

The native decomp `TBathtub::tumble` applies the retail gated, strength-scaled sine/cosine impulse.

## Evidence

Ghidra and range disassembly of GMSE01 `0x801fb568..0x801fb5ec` agree on the `unk29A` early return,
the exact `0.0001f` strength scale, the `65536/360` short-angle conversion, positive sine on X,
zero on Y, and negative cosine on Z. The focused Clang test calls the shipping member and passes
0°, 90°, scale, and no-mutation controls.

## What would falsify it

The focused test stops driving the shipping function, any pinned control fails, or a new binary
reading shows different constants, gate, signs, or destination fields.

