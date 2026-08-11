---
id: C019
kind: claim
status: holds
created: 2026-08-05
tags: perf,60fps
depends: extern/aurora/lib/gfx/common.cpp
---

## Claim

The decomp's ~15.8 ms/frame drain is dominated by aurora::gx::fifo::draw_prim (45% of 60 stack samples) plus per-draw buffer preparation, i.e. genuine per-vertex work — not by FIFO framing or decode dispatch. Optimising the render path therefore means making draw_prim's attribute processing cheaper, which would benefit BOTH runtimes; a decomp-only FIFO bypass would not remove this cost.

## Evidence

debug_journal/2026-08-05_runtime_cost_comparison_for_60fps.md; eu-stack sampling, 60 samples: draw_prim 27, render_worker 7, process 4, push_storage 4, prepare_idx_buffer 3

## What would falsify it

60 samples is a small sample taken from one scene; a proper profiler (perf is not installed here) or a different scene could shift the attribution
