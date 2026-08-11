---
id: C039
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Interpolation measurably smooths the presented image: with the camera rotating, both presents of a tick advance the picture nearly equally (ALTERNATION 1.19) and each present moves it about two thirds as far as a 30Hz present

## Evidence

tools/interp/cadence.py on Delfino, matched guest ticks 1606-1684, SBR_PAD_SCRIPT=400:CSTICK=100/0: SBR_LERP60=1 gives mean step 13.34 and ALTERNATION 1.19 (phase means 14.46/12.19); the same ticks with interpolation off give mean step 19.45. Measured at the PIXEL level, independent of the per-draw pairing percentages.

## What would falsify it

a change to the interpolation alpha (forced 0.5 in stream_interp.cpp), to which draws are tagged, or to the replay-emission mechanism; re-measure with the same CSTICK script and matched ticks
