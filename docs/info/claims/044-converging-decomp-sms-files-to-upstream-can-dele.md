---
id: C044
kind: claim
status: holds
created: 2026-08-12
tags: upstream,convergence,lp64
depends: tools/re/rebase_upstream.py#cmd_converge, debug_journal/2026-08-30_upstream_convergence_runtime_bisection.md
reconfirmed: 2026-08-30
verified_at: 2026-08-30 04:06:32+00:00
---

## Claim

Converging decomp/sms files to upstream can DELETE native work while building green. Three categories are invisible to a build check: a struct whose FIELD TYPES are the fix (J3D2 file-overlay blocks, where every 'pointer' is a 32-bit file offset and 8-byte host pointers break the on-disk layout), a hand-RE'd function that is correct but not yet called, and anything behind a runtime condition the smoke test does not reach.

## Evidence

2026-08-12: a 25-file convergence deleted TBathtubKillerManager::countActiveKillers (RE'd from US 0x8012f204) and a native-port declaration in AnimalBase.hpp, both green. A 48-file convergence adopted J3DModelLoader.hpp, J3DJointFactory.hpp, J3DMaterialFactory.hpp and J3DMaterialFactory_v21.hpp, built green, and segfaulted on every run; bisected by reverting halves and re-running. None of the six files contains SMS_NATIVE_PLATFORM or uintptr_t, so classify() called them free candidates.

## What would falsify it

a convergence that adopts one of these files and both builds AND runs, which would mean upstream has taken the fix. Does NOT falsify it: a green build alone, which is the exact check that missed all six.

## Re-confirmed 2026-08-30

2026-08-30 convergence compiled after 135 upstream adoptions, then failed bounded gameplay; commit-history review found six additional dormant fixes. Thirteen replacements were restored and the corrected tree completed 400 stage-1 frames.
