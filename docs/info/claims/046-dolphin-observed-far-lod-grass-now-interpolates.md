---
id: C046
kind: claim
status: holds
created: 2026-08-13
tags:
depends: extern/aurora/lib/gfx/interp.cpp#patch_vertices, extern/aurora/lib/gfx/common.cpp, sms-recomp/frame_interp/tag_deforming.cpp
---

## Claim

Dolphin-observed far-LOD grass now interpolates through the recomp vertex path as direct signed-16 XYZ with its VAT fractional shift preserved.

## Evidence

Instrumented Dolphin FIFO title capture completed 2026-08-13; guarded stage-8 audit scratch/logs/s16_grass_stage8_after.log reported grass (deforming) 391/392 vertex draws lerped (99.7%), 11,368 drawFar calls, and zero GPU kernel faults.

## What would falsify it

A stage-8 audit reports drawFar calls but the grass population snaps, or a signed-16/VAT layout change bypasses the recorded format.
