---
id: C021
kind: claim
status: falsified
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gx/command_processor.cpp#draw_prim
falsified_on: 2026-08-22
---

## Claim

draw_prim's cost splits ~55% per-primitive / ~43% per-DRAW: handle_draw_unmerged is 43% of draw_prim across only 1291 calls (p50=2.6us, p99=61us, max=1.1ms, recurring in steady state, so NOT warm-up), while the ~46k-call per-primitive phases are idx-scan 26%, merge-idx 12%, attr-enum 7%. REFINES the earlier 'render lever is per-primitive, not batching' conclusion: per-primitive is the larger half, but per-draw is comparable in size and had been written off.

## Evidence

SB_PROFILE_DRAWPRIM rdtsc phase probes on Delfino; probe cost 0.3ns = 0.5% of body; controls all pass: unattributed 1.9%, merged+unmerged+early-return == calls exactly, per-phase n= denominator. debug_journal/2026-08-05_drawprim_phase_attribution.md

## What would falsify it

handle_draw_unmerged is restructured, or the DP_PHASE probes in draw_prim move/change

## FALSIFIED 2026-08-22

The percentage split and nanoseconds-per-call conclusion came from I011's TSC elapsed profiler.
Its accounting controls validate the partition, but they do not make host-time attribution stable
under unrelated system contention. I011 is now distrusted for optimization selection. Preserve the
path call counts as diagnostic population data, but do not use the 55/43 split, percent-of-drain,
or latency percentiles as evidence of what should be optimized.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
