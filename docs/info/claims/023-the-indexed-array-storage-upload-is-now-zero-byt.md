---
id: C023
kind: claim
status: holds
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gfx/common.cpp#push_storage_persistent
---

## Claim

The indexed-array storage upload is now ZERO bytes per steady-state frame: a persistent arena inside g_storageBuffer (past the staging-mirrored region) holds 20.5 MB in ~574 entries, written once via queue.WriteBuffer and rewritten only on content-hash change. arrayUpload fell ~3x (63% -> 34% of the per-draw build). Skipping the staging write and relying on retained bytes is UNSOUND -- the staging buffer is Unmap()/re-MapAsync'd each frame, so its contents are undefined per the WebGPU mapping contract.

## Evidence

SB_PROFILE_DRAWPRIM 'arena:' line reads reused=492 (20.44 MB, NOT uploaded) uploaded=0 full-fallbacks=0; offsets measured stable=492 moved=0; cross-frame content 100% unchanged. Ratios vs untouched sections: arrayUpload/shaderinfo 5.94x->1.66x, /pipeline_ref 6.84x->2.54x, /build_uniform 9.45x->3.09x. debug_journal/2026-08-05_drawprim_phase_attribution.md

## What would falsify it

the arena's content-hash detector reports uploads>0 in a steady-state frame (an array rewritten in place), or full-fallbacks>0 (working set exceeds PersistentStorageSize)
