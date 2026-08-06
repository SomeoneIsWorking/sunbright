---
id: C031
kind: claim
status: holds
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
