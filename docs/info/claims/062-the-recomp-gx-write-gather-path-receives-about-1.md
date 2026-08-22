---
id: C062
kind: claim
status: holds
created: 2026-08-22
tags: performance,gxfifo
depends: sms-recomp/runtime/devices/dev_gxfifo.cpp#fifo_write, sms-recomp/runtime/devices/gx_fifo_input.cpp#appendBigEndian
---

## Claim

The recomp GX write-gather path receives about 123.5k 1/2/4-byte stores totaling about 357 KB per settled plaza frame; generic vector range insertion was per-store overhead, not useful game work

## Evidence

2026-08-22 SB_DRAW_STATS+SB_LOG=gxwork controlled run; debug_journal/2026-08-22_internal_work_profiling_and_decomp_rebase.md

## What would falsify it

A settled stage-1 internal-work run reports a materially different append population, or the MMIO write path stops using GxFifoInput::appendBigEndian
