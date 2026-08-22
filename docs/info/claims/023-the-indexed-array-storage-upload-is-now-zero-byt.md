---
id: C023
kind: claim
status: holds
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gfx/common.cpp#push_storage_persistent
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:00:55
---

## Claim

The indexed-array persistent arena performs zero upload bytes in a measured steady-state frame: a
region inside `g_storageBuffer`, beyond the staging-mirrored range, retains approximately 20.5 MB
in about 574 entries and rewrites an entry only when its content hash changes. Skipping a staging
write without persistent storage is unsound because the staging buffer is unmapped and remapped
each frame, so its contents are not retained by the WebGPU mapping contract. This claim does not
assert an elapsed-time reduction.

## Evidence

The retired profiler's deterministic `arena:` counters reported `reused=492` (20.44 MB),
`uploaded=0`, `full-fallbacks=0`, `stable=492`, and `moved=0`, with unchanged content hashes.
`debug_journal/2026-08-05_drawprim_phase_attribution.md`. I011's elapsed ratios are explicitly
excluded from this evidence.

## What would falsify it

the arena's content-hash detector reports uploads>0 in a steady-state frame (an array rewritten in place), or full-fallbacks>0 (working set exceeds PersistentStorageSize)

## Re-confirmed 2026-08-22

Rewritten to retain only the persistent-arena ownership rule and exact work counters. The old
elapsed ratios and “3x” reduction were produced by I011 and are excluded.
