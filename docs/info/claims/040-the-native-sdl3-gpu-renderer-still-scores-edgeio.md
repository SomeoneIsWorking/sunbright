---
id: C040
kind: claim
status: falsified
created: 2026-08-12
tags: 
falsified_on: 2026-08-27
---

## Claim

The native SDL3-GPU renderer still scores edgeIoU 32.2% / lumaCorr +0.72 against the aurora oracle at N=59 — unchanged since 2026-07-23 despite everything landed since

## Evidence

./run-render.sh SBR_AB=1 SBR_QUIT_AFTER=4000 on 2026-08-12 printed '=== COMPARABLE @ N=59: edgeIoU 32.23% lumaCorr +0.7216 ===', against the 2026-07-23 figure of 32.2% / +0.688 recorded in debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md. Compared on the harness's own COMPARABLE line, which exists because the running mean drifts several points with frame COUNT alone.

## What would falsify it

any change under sms-recomp/runtime/render/ or sms-recomp/runtime/shaders/, or to the J3D capture seam; re-run the same command and read the N=59 line

## FALSIFIED 2026-08-27

The in-process A/B does not join the same frame: Aurora frame-sink readback arrives one or two presents later while render_compare.cpp retains only the latest native frame. SBR_AB_SELFTEST overwrites native from inside the callback and therefore bypasses this broken join. The N=59 score is not exact-frame evidence.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
