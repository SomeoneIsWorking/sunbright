---
id: C019
kind: claim
status: holds
created: 2026-08-05
tags: perf,60fps
depends: sms-recomp/runtime/devices/dev_gxfifo.cpp, extern/aurora/lib/gx/command_processor.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:18:02
---

## Claim

A bounded, no-loss sample of settled stage-1 recomp execution places recurring CPU work in the GX
command path: root FIFO parsing/copying, Aurora parsing and draw creation, exact indexed-array
scanning, and per-draw hashing. Sampling ranks those implementation regions as candidates; it does
not assign them elapsed-time budgets or establish a maximum FPS.

## Evidence

Instrument I030; `debug_journal/2026-08-22_internal_work_profiling_and_decomp_rebase.md`. A bounded
499 Hz capture recorded 10,398 samples with zero losses. Independent `SB_DRAW_STATS`/`gxwork`
counters established that the sampled path was processing non-empty command, vertex, index, and
draw populations.

## What would falsify it

A matched bounded capture with zero losses no longer samples the GX command path as a recurring
hot region, or either the sampler's loss check or the independent non-empty-work control fails

## Re-confirmed 2026-08-22

Rewritten after the elapsed attribution was retired. A 499 Hz bounded stage-1 capture recorded
10,398 samples with zero losses and repeatedly sampled the root/Aurora GX command path; independent
work counters reported non-empty command, indexed-field, vertex, and finalized-draw populations.

## Re-confirmed 2026-08-22

Bounded 499 Hz capture retained 10,398 samples with zero losses; ec65909 removed elapsed probes while preserving the sampled GX parse, array-scan, draw-build, and hashing paths.
