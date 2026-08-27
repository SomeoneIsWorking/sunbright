---
id: C076
kind: claim
status: holds
created: 2026-08-28
tags: 60fps,interpolation,water
depends: tools/interp/compare_modes.py, sms-recomp/frame_interp/frame_interp.cpp, sms-recomp/overrides/native_pad.cpp
---

## Claim

In the controlled water-facing camera rotation, the visible sea region is not a whole-population missing-lerp target: Lerp60 slightly reduces its mean spatial alternation versus Native60 from 0.234 to 0.227.

## Evidence

Schema-5 three-run capture on 2026-08-28: Native60, byte-exact Native60 repeat, and Lerp60 each supplied 33 complete 1280x960 frames at shared guest retraces 1822..1854 with identical settled camera matrices and texture descriptor sets. For grid ROI [9,3..14,5), which covers the visible sea while excluding the dialogue band, the forced-snap control raised alternation 0.234 to 1.000 and real Lerp60 measured 0.227. One worse cell above the sea, [10,2], contains a moving palm trunk/sky edge, not water.

## What would falsify it

A different water view, motion pattern, renderer/interpolation change, or a draw-identity join showing a water draw snapping despite this aggregate screen-region result requires remeasurement.
