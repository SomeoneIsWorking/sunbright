---
id: C044
kind: claim
status: holds
created: 2026-08-12
tags: upstream,convergence,lp64
depends: tools/re/rebase_upstream.py#cmd_converge, debug_journal/2026-08-30_upstream_convergence_runtime_bisection.md
reconfirmed: 2026-09-28
verified_at: 2026-09-28 05:20:00+00:00
---

## Claim

Converging decomp/sms files to upstream can DELETE native work while building green. Three categories are invisible to a build check: a struct whose FIELD TYPES are the fix (J3D2 file-overlay blocks, where every 'pointer' is a 32-bit file offset and 8-byte host pointers break the on-disk layout), a hand-RE'd function that is correct but not yet called, and anything behind a runtime condition the smoke test does not reach.

## Evidence

2026-08-12: a 25-file convergence deleted TBathtubKillerManager::countActiveKillers (RE'd from US 0x8012f204) and a native-port declaration in AnimalBase.hpp, both green. A 48-file convergence adopted J3DModelLoader.hpp, J3DJointFactory.hpp, J3DMaterialFactory.hpp and J3DMaterialFactory_v21.hpp, built green, and segfaulted on every run; bisected by reverting halves and re-running. None of the six files contains SMS_NATIVE_PLATFORM or uintptr_t, so classify() called them free candidates.

## What would falsify it

a convergence that adopts one of these files and both builds AND runs, which would mean upstream has taken the fix. Does NOT falsify it: a green build alone, which is the exact check that missed all six.

## Re-confirmed 2026-08-30

2026-08-30 convergence compiled after 135 upstream adoptions, then failed bounded gameplay; commit-history review found six additional dormant fixes. Thirteen replacements were restored and the corrected tree completed 400 stage-1 frames.

## Re-confirmed 2026-09-28 — with no build to hide behind

The 205-commit upstream sync (fork point 40c2594b -> 6ae2aa86) is the strongest
evidence this claim has, because for the first time the build could not vouch for
the result: `sms-boot` is no longer in the parent build and `run.sh` refuses the
decomp runtime by design, so nothing in the tree compiles `decomp/sms` at all.
The whole tree now has to be argued for from the diff.

A plain `git merge -X theirs upstream/main` resolved all 141 content conflicts
and, per the marker scheme, looked like a clean convergence. It was not. A
line-level check of every line this fork added since the fork point found native
work missing from 39 files, including the entire `sb_host_alloc`
host-allocation gate in JKRHeap.cpp (8 occurrences -> 0), four `SUNBRIGHT-KEEP`
and "Native port of" provenance notes, and the STOPGAP in MarNameRefGen_NPC. A
further four were found afterwards by reading the pointer-cast residue:
Camera.hpp lost its `intptr_t` cast, JSupport.hpp now casts an offset to `s32`
where this fork used `uintptr_t`, JASBankMgr.cpp lost a `(u32)(uintptr_t)` width
assignment, and JKRDvdArchive.cpp lost an initialiser and a VERSION_SELECT
assert.

This is the claim's three categories, all three at once, and the fourth one
mattered more: the reason a build was the only detector is gone. `check_dangling`
style marker counting is a *negative* signal — no marker means "free candidate" —
and a negative signal that has already misclassified six files cannot be the
basis for deletion. What replaced it counts a strong marker's occurrences on our
side against the merged side and treats a drop as blocking, which is the one
direction that cannot silently pass.

What would falsify it now: a merge that adopts one of these 43 files and both
builds AND runs with the fix present. Nothing else.
