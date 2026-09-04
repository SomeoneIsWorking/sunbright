---
id: C005
kind: claim
status: holds
created: 2026-07-28
tags: gx,re
depends: decomp/sms/src/dolphin/gx/GXGeometry.c, decomp/sms/src/dolphin/gx/GXTev.c
---

## Claim

GMSE01 relies on BP write-mask register `0xFE`; a decoder that models BP state must merge the next
write as `(cached & ~mask) | (value & mask)`.

## Evidence

A title trace counted roughly 3.5 million mask writes. Recovered GX helpers arm `0x07FC3F` before
GENMODE and `0xC000` before the cull-mode update, independently demonstrating partial-register
writes.

## What would falsify it

An independently decoded representative trace contains no `0xFE` writes, or retail GX code is shown
to apply the mask with different merge semantics.
