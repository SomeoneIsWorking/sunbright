---
id: C094
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j3d,material,raster
depends: native-render/src/j3d_unlit_material.cpp#classify_j3d_raster_policy, native-render/src/semantic_3d_pass.cpp#Semantic3dPass, sms-recomp/overrides/semantic_j3d_material_adapter.cpp#capture_guest_j3d_material_state, sms-boot/runtime/native_j3d_material_adapter.cpp#sb_native_capture_j3d_material_state
---

## Claim

Both semantic J3D adapters carry high-level cull, depth, alpha-cutout, and blend policy into the PC-native renderer without reading GX/FIFO raster state.

## Evidence

CPU controls cover compact and exact full-block policy families plus one-field refusals; the watched SDL GPU control distinguishes cull, alpha threshold, blend, and depth-write answers; guarded live runs exited 0 with 6,006 recomp and 2,278 native-decomp cutout/back-cull model submissions.

## What would falsify it

Either adapter reads GX/FIFO raster state, an exact supported policy maps differently across layouts, a one-field custom policy is accepted, a guarded GPU control no longer produces the known-different cull/alpha/blend/depth answer, a live perspective run submits zero supported models, or an original draw body stops executing.
