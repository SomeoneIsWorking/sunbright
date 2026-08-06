---
id: C028
kind: claim
status: holds
created: 2026-08-06
tags: 
---

## Claim

The 60fps sub-frame's asymmetry is monotone in alpha and crosses zero at alpha=0.5 (measured -75.3% / +0.9% / +87.9% at alpha 0.0/0.5/1.0, presents 5..7, camera moving 19.664 u/tick, SBR_INTERP60_PREENTRY_VC=1) — but being centred is NOT being correct: at alpha=0.5 the sub-frame is further from each neighbour (13.68 / 13.59) than they are from each other (10.63), off-segment +157%. At alpha=1.0, sub->next is 5.03% of pixels where it must be ~0.

## Evidence

tools/interp/interp60_run.sh early{0.0,0.5,1.0} DUMP_AFTER=60; tools/interp/subframe_position.py; MAIN presents byte-identical across alpha (no leak); SBR_INTERP60_STREAMHASH shows same-size different-hash streams across alpha at the same moment

## What would falsify it

a change to sbr_interp60_subframe's pass order, camera_apply, or the PreEntry view-calc re-issue
