---
id: C075
kind: claim
status: falsified
created: 2026-08-28
tags: render,gx-compat,parity
depends: sms-recomp/runtime/render/render_compare_join.cpp, sms-recomp/runtime/render/render_compare_metric.cpp, sms-recomp/runtime/render/native_render.cpp, extern/aurora
falsified_on: 2026-08-30
---

## Claim

The exact-frame GX-compatibility/Aurora A/B join currently measures the recomp SDL3-GPU GX compatibility renderer at edgeIoU 28.32% and luma correlation +0.4095 over N=3 matched frames in the guarded Delfino run. This is compatibility-reference evidence only and does not measure progress toward the PC-native semantic renderer in G003/G004.

## Evidence

2026-08-28: env SBR_RENDER_APPROVED=1 ./run-render.sh SBR_FRAME_RATE=native-60 SBR_AB=1 SBR_AB_EVERY=30 SBR_AB_SELFTEST=1 SBR_AB_AT=3 SBR_QUIT_AFTER=130 SBR_RUN_SECS=90 SBR_LUCENT_DEBUG=ab exited 0. The identity self-control scored 100% edgeIoU/+1.000 at frame 31, exact frame IDs joined N=3 at 28.32%/+0.4095, 130 presents completed, and the watcher reported no Vulkan validation, device fault, GPU reset, or CPU signal.

## What would falsify it

Any change to GX compatibility renderer output, Aurora sink scheduling/metadata, the exact-frame join, comparison metric, scene/camera, sample cadence/count, or renderer dimensions requires a same-contract rerun.

## FALSIFIED 2026-08-30

The 2026-08-30 same-contract rerun produced edgeIoU 28.17% and luma correlation +0.4110 at N=3, not 28.32%/+0.4095. The exact-number claim expired as designed; the instrument identity control still passed at 100%/+1.000 and the replacement measurement is C083.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
