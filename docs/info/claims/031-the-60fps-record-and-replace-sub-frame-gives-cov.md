---
id: C031
kind: claim
status: superseded
created: 2026-08-06
tags: interp60
depends: sms-recomp/overrides/interp60_replace.cpp
---

## Claim

The 60fps record-and-replace sub-frame gives covered geometry a PARTIAL response to alpha: at alpha=0 the sky lands at lead 0.009 (exact) while ground/sea/buildings land at 0.304/0.346/0.457 and the uncovered J2D subtitle at 0.854. The offset grows with how much the region changes per tick, so it is not a single missing subsystem.

## Evidence

debug_journal/2026-08-06_motion_census_and_uncovered_residual.md per-region table; dumps scratch/render/i60_q{00,05,10}.rgba.*

## What would falsify it

if a named subsystem is wired in and every region's alpha=0 lead drops to the sky's ~0.01, it WAS a single missing population after all

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
