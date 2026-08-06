---
id: C030
kind: claim
status: holds
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
