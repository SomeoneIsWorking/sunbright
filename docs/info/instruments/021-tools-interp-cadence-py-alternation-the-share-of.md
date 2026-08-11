---
id: I021
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

tools/interp/cadence.py ALTERNATION — the share of on-screen change landing on one of the two presents. BLIND SPOT, measured: in a scene with little GEOMETRIC motion it is dominated by per-tick content updates (animated textures, EFB copies, 2D layers) that no geometry interpolation can smooth, and reads catastrophically high while nothing is wrong — 15.30 at mean step 0.88 on an idle camera against 1.19 at mean step 13.34 with the camera rotating, same build. Drive the camera (SBR_PAD_SCRIPT=400:CSTICK=100/0) before concluding anything from a high value; the tool prints this warning with those numbers whenever alternation is high.

## Validated by

Run against both classes on 2026-08-12: near-static scenes (6.24 and 15.30) and a rotating camera (1.19) on the SAME build over matched guest ticks. Its own --selftest also refuses a one-step series and a series with no dumps.

## Known failure modes

(none recorded yet)
