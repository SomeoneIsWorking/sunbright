---
id: C054
kind: claim
status: holds
created: 2026-08-21
tags: bse,framerate,native60
depends: sms-recomp/overrides/native_frame.cpp#video_wait_for_retrace, sms-recomp/app/frame_rate.cpp#animation_rate_constant, sms-recomp/bse/frame_rate_fixes.cpp#bse_hx_motion_update
---

## Claim

Native 60 no longer changes retrace count alone: the frame seam applies BetterSunshineEngine's SMS animation-rate and ModelGate timing values before the retail TVideo body, while sms-recomp/bse owns the targeted game-rate fixes without deleting the retail recompiled functions.

## Evidence

debug_journal/2026-08-21_bse_native_frame_rate.md

## What would falsify it

if either BSE timing write no longer occurs before the retail TVideo body, or an equal-wall-clock 30/60 control shows game motion advancing twice as far at Native 60
