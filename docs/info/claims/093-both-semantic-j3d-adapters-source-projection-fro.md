---
id: C093
kind: claim
status: holds
created: 2026-08-30
tags: renderer,camera,j3d
depends: native-render/src/model_context.cpp#capture_j3d_scene_context, sms-recomp/overrides/semantic_j3d_scene.cpp#run_semantic_j3d_draw_dispatch, sms-recomp/overrides/semantic_j3d_adapter.cpp#submit_semantic_j3d_shape, sms-boot/runtime/native_j3d_scene.cpp#sb_native_j3d_scene_push, sms-boot/runtime/native_j3d_adapter.cpp#sb_native_j3d_shape_submit
---

## Claim

Both semantic J3D adapters source projection from high-level TGraphics draw dispatches without consulting GX projection/FIFO/compatibility state.

## Evidence

Controlled perspective/orthographic/empty-scope tests pass. Guarded fastboot runs exited 0 and submitted 6,468 recomp models plus 11,858 native-decomp models; source audit finds no GX projection-cache call in either semantic adapter.

## What would falsify it

Either semantic J3D adapter reads GX/FIFO/compatibility projection state again, nesting fails to restore a camera context, a live perspective run submits zero models, or an original draw/dispatch body no longer executes.
