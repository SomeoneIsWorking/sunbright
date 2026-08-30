---
id: C092
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j3d,semantic
depends: native-render/src/j3d_mesh_decode.cpp#decode_j3d_mesh_element, sms-recomp/overrides/semantic_j3d_adapter.cpp#submit_semantic_j3d_shape, sms-boot/runtime/native_j3d_adapter.cpp#sb_native_j3d_shape_submit, decomp/sms/src/JSystem/J3D/J3DGraphBase/J3DShape.cpp#J3DShape::draw
reconfirmed: 2026-08-30
verified_at: 2026-08-30 07:03:39+00:00
---

## Claim

Both runtimes publish nonzero rigid unlit single-texture J3D model geometry through the renderer-neutral semantic sink while retaining their original draw bodies.

## Evidence

Guarded audits on 2026-08-30: recomp 60 frames submitted 8,150 models/4,724,700 vertices; decomp direct-to-Delfino 400 frames submitted 42,852 models/27,326,178 vertices with zero layout or decode failures.

## What would falsify it

A guarded rerun submits zero supported models, reports a layout/decode failure for the accepted family, or either runtime no longer executes its retained original draw body.

## Re-confirmed 2026-08-30

Final combined-tree guarded recomp audit on 2026-08-30: 60 presents/30 semantic frames, 6,512 submitted models and 2,519,484 decoded vertices, zero unreadable/layout/projection/non-rigid/decode/texture-table failures, guarded exit 0. The distinct earlier 60-frame cadence submitted 8,150 models/4,724,700 vertices. Decomp direct-to-Delfino 400-frame audit submitted 42,852 models/27,326,178 vertices with zero layout, rigid-matrix, or mesh-decode failures and guarded exit 0.

## Re-confirmed 2026-08-30

Post-integration guarded reruns after separating native pointers from guest numeric addresses: recomp exited 0 after 60 presents with 6,512 models/2,519,484 decoded vertices and zero unreadable/layout/projection/non-rigid/decode/texture-table failures; native decomp exited 0 after 180 frames with 11,858 models/7,194,726 vertices and zero layout/non-rigid/decode failures.
