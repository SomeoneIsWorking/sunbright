---
id: C041
kind: claim
status: falsified
created: 2026-08-12
tags: 
depends: sms-recomp/runtime/render/native_render.cpp, sms-recomp/runtime/render/render_compare.cpp
falsified_on: 2026-08-27
---

## Claim

no TEV-stage ablation recovers the native renderer's edgeIoU gap, and no draw in a plaza frame samples a texture unit above 0

## Evidence

scratch/logs/ablate5.log, 2026-08-12: the round-robin ablation sweep sampled all 15 variants against paired baselines over 17 scored frames. Largest positive paired delta +1.3 edgeIoU (tev->passthrough, n=1); texfetch->white -5.6. 'pin unit1->0' through 'pin unit7->0' and 'texmap->unit0' all render BYTE-IDENTICAL frames (checksums in ablate.log confirm on two independent frames), as does control:no-op. The same run reports 'unit 1: 0 distinct addresses' and 0 TX_SETIMAGE0 writes for unit 1. So the ~68% edgeIoU gap is not in texgen, texmap routing, konst, the alpha test or per-unit binding.

## What would falsify it

a plaza frame that binds a texture unit above 0, or an ablation reaching n>=5 with a paired delta above +2

## FALSIFIED 2026-08-27

The round-robin ablation sweep compares each operation on a different game frame, and a control:no-op failure is logged without suppressing the ranking. Its leading positive row had n=1. Until variants share one tagged frozen frame and the control fails closed, the sweep cannot exclude TEV or texture operations.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
