---
id: C066
kind: claim
status: holds
created: 2026-08-22
tags: interpolation,tdl
depends: sms-recomp/frame_interp/tag_indexed_quad.cpp, extern/aurora/lib/gfx/indexed_interp.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:40:22
---

## Claim

Continuously visible TDLTexQuad and TDLColorTexQuad members interpolate by stable per-quad identity across dynamic batch membership changes.

## Evidence

2026-08-22 live stage-1 FLUDD run: 50 keyed arrays/706 groups, zero unkeyed arrays and layout mismatches; final audit 472 paired arrays, four births, one correct reappearance, zero camera-only. Synthetic A,B->B,C control follows B by key.

## What would falsify it

Any live TDL run reports an unkeyed array, a layout mismatch, or a continuously visible camera-only TDL draw; or the membership-change control stops following B by key.

## Re-confirmed 2026-08-22

2026-08-22 final stage-1 FLUDD control: 472 paired TDL arrays, 4 births, 1 correct reappearance, 0 camera-only; 50 keyed arrays/706 groups at tick 400, 0 unkeyed and 0 layout mismatches; A,B->B,C selftest passed.
