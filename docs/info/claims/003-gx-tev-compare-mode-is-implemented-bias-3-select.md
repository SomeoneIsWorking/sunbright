---
id: C003
kind: claim
status: holds
created: 2026-07-28
tags: native-render,re
depends: decomp/sms/src/dolphin/gx/GXTev.c#GXSetTevColorOp, decomp/sms/src/dolphin/gx/GXTev.c#GXSetTevAlphaOp
---

## Claim

GX TEV compare mode uses bias 3 to select comparison, the scale field to select comparison width,
and the subtract bit to select equality rather than greater-than.

## Evidence

Recovered `GXSetTevColorOp` and `GXSetTevAlphaOp` packing in `decomp/sms` establishes the field
meanings. A settled Delfino observation found compare mode in 537 of 2,816 enabled stages.

## What would falsify it

A retail disassembly or independently decoded command stream that assigns different meanings to
these fields.
