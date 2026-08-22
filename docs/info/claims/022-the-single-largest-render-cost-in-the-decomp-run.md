---
id: C022
kind: claim
status: holds
created: 2026-08-05
tags: perf
depends: extern/aurora/lib/gx/gx.cpp#array_upload_lookup
reconfirmed: 2026-08-22
verified_at: 2026-08-22 17:00:55
---

## Claim

Before the data-identity cache, indexed-array storage processed 37.09 MB in a measured frame for
20.44 MB of distinct data (1.8x redundant work). `AttrArray::cachedRange` was a per-slot cache that
`GXSetArray` discarded on re-registration while the game repointed a slot A, then B, then A. Keying
the cache on data identity reduced that frame's processed bytes to the 20.44 MB distinct set. This
claim is about exact bytes and cache ownership, not elapsed cost or optimization priority.

## Evidence

The retired profiler's deterministic array counters (uploads, bytes, distinct bytes, redundancy)
in `debug_journal/2026-08-05_drawprim_phase_attribution.md`. The retained in-frame content-change
control reported zero changes under an unchanged `(ptr,size)` identity. The retired instrument's
elapsed fields are explicitly excluded from this evidence.

## What would falsify it

The in-frame content-change counter reports non-zero (an array rewritten in place mid-frame would
make the data-keyed cache serve stale geometry), or the exact byte counters no longer reproduce
the stated distinct/redundant populations on the pinned scene

## Re-confirmed 2026-08-22

Rewritten to preserve only the exact byte populations and the data-identity cache rule. The old
“single largest cost,” milliseconds, percentage, and speedup statements were elapsed claims from
I011 and are not part of this confirmation.
