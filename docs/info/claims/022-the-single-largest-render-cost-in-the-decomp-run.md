---
id: C022
kind: claim
status: holds
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gx/gx.cpp#array_upload_lookup
---

## Claim

The single largest render cost in the decomp runtime is indexed-array storage upload: SB_PROFILE_GFX arrayUpload was ~2.85ms of a ~10.2ms drain (63% of the per-draw build). It was uploading 37.09 MB/frame for 20.44 MB of distinct data (1.8x redundancy) because AttrArray::cachedRange is a per-SLOT cache that GXSetArray drops on re-registration, while the game re-points a slot at A then B then A. Now keyed on data identity: 20.44 MB/frame, redundancy 1.0x, ~30% less upload time.

## Evidence

SB_PROFILE_DRAWPRIM arrays: line (uploads/bytes/distinct/redundancy); SB_PROFILE_GFX per-draw-build breakdown, stable across 3 consecutive 60-frame reports. Precondition measured not assumed: in-frame content changes under unchanged (ptr,size) == 0 on every frame, counter retained in-build. debug_journal/2026-08-05_drawprim_phase_attribution.md

## What would falsify it

the in-frame content-change counter reports non-zero (an array rewritten in place mid-frame would make the data-keyed cache serve stale geometry)
