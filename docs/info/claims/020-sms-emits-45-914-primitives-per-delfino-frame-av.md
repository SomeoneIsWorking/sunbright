---
id: C020
kind: claim
status: holds
created: 2026-08-05
tags: perf,60fps
depends: extern/aurora/lib/gfx/common.cpp, extern/aurora/lib/gx/command_processor.cpp
---

## Claim

SMS emits ~45,914 primitives per Delfino frame averaging 5.1 vertices (53% are 4-vertex quads, 235,376 verts total); aurora already merges them ~35:1 into 1314 emitted draws, so render cost is per-primitive overhead (~340 ns each) and NOT a batching failure. Note draws=1314 is the POST-merge count and SB_DRAW_STATS verts=18226 counts only immediate-mode verts.

## Evidence

debug_journal/2026-08-05_runtime_cost_comparison_for_60fps.md; SB_PROFILE_DRAWPRIM=1 distribution

## What would falsify it

measured on Delfino Plaza only; a scene with different geometry (title, an interior) could have a different primitive size profile and a different verdict on batching
