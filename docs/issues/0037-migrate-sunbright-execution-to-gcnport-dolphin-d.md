---
id: 37
title: Migrate Sunbright execution to gcnport Dolphin dynarec
status: open
symptom: The intended native/dynarec product is not runnable: exact GMSE01 still lacks a complete gcnport/Dolphin-JIT boot path and a robust runtime J3DShape::draw hook at 0x802e0390.
state_items: S001,S002,S003
tags: migration,gcnport,dolphin,jit,override
created: 2026-09-04
updated: 2026-09-04
---

## Root cause

Sunbright's title-owned runtime was built around an offline-generated PowerPC corpus and a
title-local dispatch table. That ownership cannot provide the required product contract: one
maintained platform executor translating the user's live image on demand, robust hooks independent
of JIT block shape, and mechanical absence of interpreter/static fallback. The replacement owner is
the shared `gcnport` framework around Dolphin's runtime JIT.

## What was tried / dead ends

Do not revive the prior mixed executor or use removed executor artifacts as the comparison
leg. Existing evidence from that path remains useful only for exact addresses, layouts, behavior,
and reached scenarios. A title-local wrapper around Dolphin's current block split is also rejected:
direct chaining or invalidation could bypass it and another GameCube title would need to duplicate
the same integration.

## Acceptance

- `gcnport` owns image generations, runtime hooks, original calls, bounded exits, invalidation, and
  execution counters without title addresses.
- exact `GMSE01` boots with nonzero Dolphin JIT blocks;
- the runtime dispatcher reaches `J3DShape::draw` at `0x802e0390`, submits the existing semantic
  J3D value, and runs the original body through one-call override suppression;
- controls exercise hook hit/miss, enabled/disabled, cache hit/miss, chaining, and hook-change
  invalidation; and
- link/selector inspection proves the gameplay target includes neither an interpreter nor generated
  guest code.

This resolves the first wiring discriminator only. Representative gameplay is S008 and must pass
before S009 removes the old files.
