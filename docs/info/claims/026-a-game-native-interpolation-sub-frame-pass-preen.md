---
id: C026
kind: claim
status: holds
created: 2026-08-05
tags: 60fps
depends: decomp/sms/src/System/MarDirectorDirect.cpp
---

## Claim

A game-native interpolation sub-frame pass (PreEntry + the draw perform lists, re-run from post-draw state) reproduces the frame PIXEL-IDENTICALLY: 0 of 1,228,800 pixels differ from a normal single-pass frame. The residual GX command-stream differences (+164 bytes, GX_LOAD_BP_REG writes elided as redundant on the second pass) are render-neutral. Dropping PreEntry is visibly wrong — 1,956 pixels change — so the sub-frame boundary is PreEntry + draw.

## Evidence

SB_DOUBLE_DRAW=3 rewinds pass 0 and renders from pass 1, so its presented frame IS a sub-frame's output; dumped via SB_DUMP_FRAME at AFTER=400 and compared byte-exact against baseline. Positive control: mode 1 (draw-only re-run, missing ~122 KB geometry) differs in 1956 pixels, so the comparison can detect a difference. debug_journal/2026-08-05_game_native_interpolation_design.md

## What would falsify it

a scene where a sub-frame re-run differs from the baseline frame in any pixel (Delfino steady state is one scene; a scene with per-tick-rebuilt effect geometry may behave differently)
