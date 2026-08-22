---
id: C024
kind: claim
status: falsified
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gx/command_processor.cpp#draw_prim
falsified_on: 2026-08-22
---

## Claim

Collapsing draw_prim's ~15 gated diagnostic blocks behind one master gate is worth only ~1% of draw_prim (~0.3% of drain) and was REVERTED. The theory that 15 scattered cached-static loads dominate the 13.4% spent in prologue+diag-pre+diag-post is FALSE — collapsing them recovered ~10-16% of those phases, not most of them. Where that ~19.5ns/call actually goes is still unattributed.

## Evidence

Interleaved A/B (both binaries run alternately so each saw the same machine load), medians over ~2000 frames: ratio-vs-push-verts 1.956 -> 1.758, ratio-vs-prologue 2.485 -> 2.083. Gate itself verified working both directions (SB_POS_PROBE 40 lines, SB_NDC_PROBE 2185607 lines, 0 when off). debug_journal/2026-08-05_drawprim_phase_attribution.md

## What would falsify it

a phase probe inside the diag-pre/diag-post regions attributes their cost to something a master gate WOULD remove

## FALSIFIED 2026-08-22

The “worth only ~1%” conclusion is an elapsed-time A/B derived from I011 and is not admissible
optimization evidence. The gate controls prove only that the tested switches turn their diagnostic
work on and off; they do not make the timing ratio invariant to host contention. Whether dormant
diagnostic checks are a useful target must be answered by deterministic executed-work counts or a
bounded no-loss sample, not this clock comparison.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
