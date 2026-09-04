---
id: C094
kind: claim
status: holds
created: 2026-08-30
tags: renderer,j3d,material,raster
depends: native-render/include/sunbright/native_render/j3d_material_state.h#j3d_texture_number_for_map, native-render/src/j3d_unlit_material.cpp#classify_j3d_raster_policy, native-render/src/model.cpp#transform_vertex, native-render/src/semantic_3d_pass.cpp#Semantic3dPass
---

## Claim

The semantic J3D contract carries independent texture-slot and UV selection plus high-level cull,
depth, alpha-cutout, straight or premultiplied alpha, and linear view-depth fog without reading GX
or FIFO raster state.

## Evidence

CPU controls cover compact and full policy families, independent slot/UV choice, and one-field
refusals. Watched GPU controls produce known-different cull, alpha-threshold, blend, depth-write, and
fog answers. The fog control leaves a no-fog triangle red and turns the same triangle halfway
through blue fog into the expected colour-space-correct purple. Premultiplied-alpha controls preserve
more source red over opaque blue while matching the straight-alpha destination contribution.

## What would falsify it

The classifier reads GX/FIFO state, couples texture slot to active stage count, accepts an unsupported
one-field variant, or a known-different GPU control ceases to differ.
