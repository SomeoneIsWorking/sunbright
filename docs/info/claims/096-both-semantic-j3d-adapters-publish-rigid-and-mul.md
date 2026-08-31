---
id: C096
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,pose,recomp,decomp
depends: native-render/src/model.cpp#build_model_pose, native-render/src/model.cpp#transform_vertex, sms-recomp/overrides/mtx_crosscheck.cpp#ov_j3d_shape_mtx_multi_load, sms-recomp/overrides/semantic_j3d_adapter.cpp#submit_semantic_j3d_shape, sms-boot/runtime/native_j3d_adapter.cpp#sb_native_submit_j3d_shape
---

## Claim

Both semantic J3D adapters publish rigid and multi-matrix model poses through one renderer-neutral compact palette; the reached recomp untextured diffuse-lit family submits without consuming GX matrix loads as renderer input.

## Evidence

CPU controls moved only the vertex selecting the second pose matrix and rejected invalid palettes. The guarded 120-present recomp audit changed the exact family from 77 perspective-ready/0 models and 77 non-rigid rejections to 77/77 and zero non-rigid rejections, exited 0 under the GPU watcher, and the high-level ordinary/multi-matrix bindings agreed with the independent retained GXLoadPosMtxIndx assertion.

## What would falsify it

Either adapter exposes GX matrix slots or consumes GX/FIFO matrix loads as renderer input; a compact palette changes the wrong vertex or accepts an invalid binding; high-level recomp bindings disagree with the independent retained load control; a reached supported multi-matrix family submits zero models; or an original matrix/draw body stops executing.
