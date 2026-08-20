---
id: C050
kind: claim
status: holds
created: 2026-08-20
tags:
depends: sms-recomp/overrides/native_frame.cpp
---

## Claim

Native 60 FPS changes the game-owned TVideo retrace request to one field at the native frame seam

## Evidence

Windowless run-safe Native 60 run logged repeated +1 retrace deltas after NLOGO and exited cleanly after 24 presents on 2026-08-20

## What would falsify it

A Native 60 run logs steady-state +2 retrace deltas, or the native waitForRetrace override moves away from the TVideo seam
