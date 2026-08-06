---
id: C029
kind: claim
status: falsified
created: 2026-08-06
tags: interp60
depends: sms-recomp/overrides/interp60_replace.cpp
falsified_on: 2026-08-06
---

## Claim

The 60fps record-and-replace path's residual is CONTENT IT DOES NOT COVER, not a matrix defect: at alpha=0 with the camera rotating 65 u/tick the sub-frame sits 36.7% px / mad 4.41 from the preceding main frame against a full tick of 11.42, i.e. ~39% of a tick's visible change is carried by things that stay at the current tick whatever alpha is.

## Evidence

debug_journal/2026-08-06_motion_census_and_uncovered_residual.md; DUMP_AFTER=2400 tools/interp/interp60_run.sh q00 0.0 SBR_INTERP60_REPLACE=1; scratch/screenshots/i60_a0_residual.png

## What would falsify it

if a matrix-side fix (a different pairing, a wider recording, a concat replacement) drops the alpha=0 residual below ~1.0 mad, the residual was a matrix defect after all

## FALSIFIED 2026-08-06

The (1-alpha)-shaped error is NOT a frozen population. Per-region lead reads 0.009/0.491/1.000 for the sky (tracks alpha exactly) and 0.854/0.874/1.000 for the J2D subtitle (genuinely uncovered), with ground 0.304, sea 0.346 and buildings 0.457 at alpha=0 sitting BETWEEN — a PARTIAL response distributed across covered geometry, not one missing subsystem. Multi-pass models are also ruled out: 0 of 22,001. See the per-region table in debug_journal/2026-08-06_motion_census_and_uncovered_residual.md

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
