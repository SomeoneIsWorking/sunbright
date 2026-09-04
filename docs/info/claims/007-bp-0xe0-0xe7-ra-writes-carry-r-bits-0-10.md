---
id: C007
kind: claim
status: holds
created: 2026-07-28
tags: gx,re
depends: decomp/sms/src/dolphin/gx/GXTev.c#GXSetTevColor
---

## Claim

BP registers `0xE0` through `0xE7` carry red in bits 0–10 and alpha in bits 12–22.

## Evidence

Recovered `GXSetTevColor` packing writes red with `SET_REG_FIELD(regRA, 11, 0, color.r)` and alpha
with `SET_REG_FIELD(regRA, 11, 12, color.a)`. The source interpretation was checked independently;
an earlier unequal-sample image-score comparison is intentionally not evidence for this layout.

## What would falsify it

A retail disassembly or independently decoded register write places either channel in a different
bit range.
