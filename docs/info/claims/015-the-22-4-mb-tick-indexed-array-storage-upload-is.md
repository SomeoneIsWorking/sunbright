---
id: C015
kind: claim
status: holds
created: 2026-08-04
tags: perf,aurora
---

## Claim

The 22.4 MB/tick indexed-array storage upload is architectural, not a cache miss: array.cachedRange is invalidated every end_frame BECAUSE the single global storage buffer re-copies its staging from offset 0 each frame, so a cross-frame cached range would point at overwritten bytes. Removing the re-upload requires a persistent buffer plus guest-write detection (deformable geometry rewrites its arrays, so an unchanged pointer does not prove unchanged contents).

## Evidence

debug_journal/2026-08-04_afterimage_effect_and_frame_budget.md; AURORA_REPLAY_LOG_EVERY frame sizes; common.cpp per-frame cachedRange reset

## What would falsify it

if aurora ever gives storage its own persistent (non-restarting) allocator, the per-frame invalidation is no longer required and this reasoning does not apply
