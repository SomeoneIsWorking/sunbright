---
id: C030
kind: claim
status: superseded
created: 2026-08-06
tags: interp60
depends: sms-recomp/overrides/interp60_replace.cpp
---

## Claim

Every DRAWN draw-matrix moves between ticks and the lerp reaches all of them; the 37% of zero-delta translation elements are invisible slots, not a hidden world-matrix population.

## Evidence

SBR_INTERP60_REPLACE_KICK_ONLY=frozen moves 0.00% of pixels, =moving moves 96.33%, =all moves 96.33% (exhaustive), same moment, alpha=1.0

## What would falsify it

if a kick restricted to frozen matrices ever moves a pixel, some frozen matrix is drawn and the population is real

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
