---
id: 10
title: Recomp aborts on zero-byte J3D material display list
status: resolved
symptom: After entering Bianco Hills, gxfifo aborts: display list 0x8157daa0 +0x0 is outside MEM1 from J3DDisplayListObj::callDL
tags: recomp,gxfifo,j3d,display-list,crash
created: 2026-08-25
updated: 2026-08-25
---

## Root cause

The FIFO hardening reused `checked_mem1_offset`, whose generic nonempty-span contract rejects
`byteCount == 0`, and therefore mislabeled a legal `GXCallDisplayList(addr, 0)` as outside MEM1.
Retail J3D submits `mSize` unconditionally; Dolphin and Aurora consume an empty call without
decoding target bytes.

## Evidence and controls

- The exact `0x8157daa0 + 0` regression case classifies `Empty`.
- A 16-byte span ending exactly at MEM1's boundary classifies `Valid`.
- The same span extended by one byte classifies `Invalid`.
- A prior clean run observed each of two stable zero-byte lists 1,150 times before exiting normally.
- A 700-present recomp + Aurora run exits zero with the amdgpu counter unchanged at 42.

## Resolution

A display-list-specific `Empty` / `Valid` / `Invalid` classifier returns before pointer arithmetic
for `Empty` while retaining fail-fast checks for every nonempty invalid span.
