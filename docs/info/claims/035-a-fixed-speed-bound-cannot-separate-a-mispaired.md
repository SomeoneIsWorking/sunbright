---
id: C035
kind: claim
status: holds
created: 2026-08-11
tags: interp60
depends: extern/aurora/lib/gfx/interp.cpp#patch_draw
---

## Claim

A FIXED speed bound cannot separate a mispaired draw from a fast object: the per-object CONTINUITY test (this tick's motion vs 4x the same object's previous tick, floor 100 units) does. Pianta Village refusals fell 12791 -> 199 while the plaza kept its 154 genuine >=10k-unit teleports, and J3D shape interpolation there went 93.3% -> 99.2%.

## Evidence

Measured on SBR_STAGE=8 SBR_LERP60=1, 290 presents. The old constant refused 12791 of 231011 paired draws; the refused deltas were CONTIGUOUS with the accepted bulk (12657 in [100,1k) vs 41939 accepted in [10,100)), not a separated cluster. Printing coordinates one-per-tick showed a single object on a smooth decelerating arc: 323.8 -> 318.3 -> 313.0 -> 307.7 -> 302.5 units/tick, positions continuous across every tick — correctly paired, genuinely fast. Falsified the ordinal-mispair hypothesis first with frame_interp/shape_identity.cpp: over 290 ticks and 104737 shape draws, ZERO (shape, mDrawMatrices) tags repeated within a tick, so the ordinal fallback never ran.

## What would falsify it

if refusals climb with scene SPEED rather than with scene chaos, the 4x ratio is too tight; if a sustained mispair (a tag aliased to another object of similarly steady motion) is ever observed interpolating, the ratio cannot see it and the tag's own quality is what must be fixed
