---
id: C016
kind: claim
status: falsified
created: 2026-08-05
tags: 60fps,perf
falsified_on: 2026-08-22
---

## Claim

True 60 Hz is gated on RENDER cost, not on which runtime executes the game logic: decomp logic is ~3.2 ms/tick vs recomp 11.6 ms (3.6x cheaper) but per-tick totals are comparable (22.2 vs 20.0 ms) because rendering dominates both, and both sit ~20-25% short of the 60 fps-equivalent needed. The recomp's 'guest logic' figure also includes ~4.3 ms of aurora per-draw build, because it drains the GX stream incrementally while the decomp drains at end_frame.

## Evidence

debug_journal/2026-08-05_runtime_cost_comparison_for_60fps.md; SB_PROFILE / SB_PROFILE_GFX on Delfino stage 1

## What would falsify it

re-measure on an IDLE machine: these runs carried load average 6-31 and a back-to-back attempt produced unusable figures. If render cost drops below ~13 ms/tick on either runtime, true 60 Hz becomes feasible there and this conclusion changes.

## FALSIFIED 2026-08-22

Host elapsed time is not admissible evidence for selecting an optimization target. The same code
produced a false apparent renderer regression while unrelated compiler work contended for the CPU,
which is exactly the failure this claim's own caveat anticipated. Runtime choice and 60 Hz
feasibility must instead be investigated through deterministic game/subsystem work counts and
bounded, no-loss CPU sampling; an end-to-end clock can remain only a safety observation.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
