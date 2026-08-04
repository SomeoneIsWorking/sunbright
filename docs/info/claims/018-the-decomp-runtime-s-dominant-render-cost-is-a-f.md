---
id: C018
kind: claim
status: falsified
created: 2026-08-05
tags: perf,60fps
falsified_on: 2026-08-05
---

## Claim

The decomp runtime's dominant render cost is a FIFO round-trip it does not need: aurora's GX API serialises every call (GXPosition3f32 -> GX_WRITE_F32) into a 2.26 MB/frame buffer that gx::fifo::drain() decodes back at end of frame, costing ~11.5 ms of the ~15.8 ms drain while the encode costs under ~0.5 ms. createdPipelines plateaus at 1824 and is NOT a per-frame cost.

## Evidence

debug_journal/2026-08-05_runtime_cost_comparison_for_60fps.md; SB_DRAW_STATS bytes=2260384 draws=1314; SB_PROFILE_GFX drain ~15.8ms steady

## What would falsify it

if a faster command decode closes most of the 11.5 ms, the round-trip is a decode-efficiency problem rather than an architectural one and no bypass path is needed

## FALSIFIED 2026-08-05

Sampling the running decomp (60 stack samples) attributes 45% of time to aurora::gx::fifo::draw_prim and the rest to per-draw buffer preparation (push_storage, prepare_idx_buffer, build_uniform) — NOT to command framing or decode dispatch. The 11.5 ms is real per-vertex/per-primitive work that a FIFO bypass would not remove, since the vertex data must be converted into GPU buffers however the commands arrive. All getenv calls inside draw_prim are correctly cached behind statics, so there is no cheap win there either.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
