---
id: C027
kind: claim
status: superseded
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

## Superseded (2026-08-12)

The subsystem this claim measured no longer exists. It describes the SUBSTITUTE-AND-RE-ISSUE
interp60 stack (`sms-recomp/overrides/interp60_replace.cpp`, `interp60_snapshot.cpp`), deleted in
21aa561 when 60fps was rebuilt as ONE module — `sms-recomp/frame_interp/` — on dusklight's
RECORD-AND-REPLACE model: the sim tick runs untouched, every final matrix is recorded keyed by its
own address, and the presentation frame lerps prev-to-cur into a replacement table consulted at
draw time. Guest state is never mutated, so the failure mode this claim measured (a substitution
that reaches zero pixels) has no counterpart in it.

Marked `superseded` rather than `falsified`: nothing here was disproven, and re-verifying it is not
possible because the code it rests on is gone. What replaced its subject matter is C037-C039.
It is left on file because the measurement METHOD — ablating the camera and actor contributions
independently and checking the frame is byte-identical — is still the right way to ask where an
interpolation response comes from.
