---
id: C020
kind: claim
status: holds
created: 2026-08-05
tags: perf,60fps
depends: extern/aurora/lib/gfx/common.cpp, extern/aurora/lib/gx/command_processor.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:18:02
---

## Claim

A settled stage-1 recomp frame sends approximately 30,400 auto-sized primitives and 169,000
vertices through Aurora's exact indexed-array scan, comprising about 506,000 indexed-field visits
and 1.01 MB of index bytes, before producing approximately 1,420 finalized Aurora draws and 18,800
immediate vertices. These are distinct work populations and must not be joined or interpreted as an
elapsed per-primitive cost.

## Evidence

Instrument I029 (`SB_DRAW_STATS` plus the recomp `gxwork` hook) and
`debug_journal/2026-08-22_internal_work_profiling_and_decomp_rebase.md`. The reporter counts work at
the owning root and Aurora boundaries and was validated with non-empty and zero-work controls.

## What would falsify it

A settled stage-1 frame reports materially different work populations without a corresponding
scene/code change, or the non-empty/zero-work controls fail. Other scenes are outside this claim.

## Re-confirmed 2026-08-22

Rewritten from retired per-primitive timing to the settled-frame populations reported by I029:
approximately 30.4k auto-sized primitives, 169k scanned vertices, 506k indexed-field visits,
1.01 MB of index bytes, 1.42k finalized draws, and 18.8k immediate vertices.

## Re-confirmed 2026-08-22

2026-08-22 bounded run-safe frame 22: auto_scan_draws=30431, vertices=169169, field_visits=506574, index_bytes=1013148; classified=31387, unclassified=0; GPU clean.
