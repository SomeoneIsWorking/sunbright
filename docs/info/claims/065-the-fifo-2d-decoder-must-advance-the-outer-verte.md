---
id: C065
kind: claim
status: holds
created: 2026-08-22
tags: gx,fifo2d
depends: sms-recomp/runtime/devices/gx_fifo_2d.cpp#decodeDraw
---

## Claim

The FIFO 2D decoder must advance the outer vertex cursor after direct position components; advancing only a shadow cursor makes CLR0 and TEX0 reread position bytes.

## Evidence

clang-tidy identified all three shadow increments as dead stores; the corrected decoder advances only direct payloads and a bounded SBR_FIFO_2D control decoded 17,204 of 17,227 orthographic draws with zero collapsed indexed or non-indexed draws.

## What would falsify it

If the GX vertex layout permits CLR0 or TEX0 to overlap direct position payload bytes, or a byte-for-byte retail decoder shows no cursor advance.
