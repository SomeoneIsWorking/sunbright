---
id: C027
kind: claim
status: holds
created: 2026-08-06
tags: 60fps
depends: sms-recomp/overrides/interp60_snapshot.cpp
---

## Claim

In the recomp 60fps sub-frame, the ENTIRE alpha response comes from the camera; the actor transform substitution reaches ZERO pixels. Ablating camera and actor alphas independently (SBR_INTERP60_ALPHA_CAM / _ACT) gives byte-identical frames for any actor alpha, and a 3000-unit kick applied to all 400 substituted entries changes 0 of 1,228,800 pixels. Re-issuing the director's calc-anim list (+0x2C) does NOT restore reach and makes the frame far worse (mean |d| 21.9 vs 0.075) via double-advanced animation.

## Evidence

scratch/logs/ab_*.log + ca_*.log + kick.log, present 2796 series, roles stamped by the runtime; controls: split cam=act=0 reproduces single-alpha 0.0 exactly, and sub@alpha=1 vs following main = 3116 px / mean 0.075 so the pipeline itself is sound. Per-sub-frame tally (SBR_INTERP60_ACTTALLY): 400 entries substituted, 8 with prev != cur, largest ~20 units. debug_journal/2026-08-05_game_native_interpolation_design.md

## What would falsify it

a kick run showing non-zero pixel change from the actor substitution alone, or an actor alpha that moves any pixel with the camera alpha pinned
