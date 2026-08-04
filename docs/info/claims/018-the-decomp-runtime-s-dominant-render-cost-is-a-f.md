---
id: C018
kind: claim
status: holds
created: 2026-08-05
tags: perf,60fps
---

## Claim

The decomp runtime's dominant render cost is a FIFO round-trip it does not need: aurora's GX API serialises every call (GXPosition3f32 -> GX_WRITE_F32) into a 2.26 MB/frame buffer that gx::fifo::drain() decodes back at end of frame, costing ~11.5 ms of the ~15.8 ms drain while the encode costs under ~0.5 ms. createdPipelines plateaus at 1824 and is NOT a per-frame cost.

## Evidence

debug_journal/2026-08-05_runtime_cost_comparison_for_60fps.md; SB_DRAW_STATS bytes=2260384 draws=1314; SB_PROFILE_GFX drain ~15.8ms steady

## What would falsify it

if a faster command decode closes most of the 11.5 ms, the round-trip is a decode-efficiency problem rather than an architectural one and no bypass path is needed
