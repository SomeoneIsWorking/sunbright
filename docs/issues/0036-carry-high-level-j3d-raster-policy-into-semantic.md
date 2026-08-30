---
id: 36
title: Carry high-level J3D raster policy into semantic model draws
status: resolved
symptom: PC-native 3D models use generic no-cull opaque depth-write behavior instead of each J3D material's authored culling, cutout, transparency, and depth policy
state_items: S004,S005
tags: native-render,j3d,material,raster
created: 2026-08-30
updated: 2026-08-30
---

Root cause: both runtime adapters classified the high-level J3D colour/TEV program but omitted
`J3DColorBlock::getCullMode` and `J3DPEBlock` policy. `Semantic3dPass` consequently hardcoded no
culling, less-or-equal depth testing with writes, no alpha rejection, and no blending for every
accepted model. This issue covers renderer-neutral capture and GPU execution of the exact common
J3D opaque, texture-edge/cutout, and translucent pixel-policy families; custom full pixel blocks
remain fallback unless their complete high-level policy is representable and verified.

### Resolution (2026-08-30)
Root cause fixed: both high-level J3D material adapters now publish cull, depth, alpha-cutout, and blend values; the shared classifier admits only the exact common opaque, texture-edge, and translucent policies; the SDL pass executes them. CPU controls, watched GPU controls, full Clang suites, and guarded nonzero recomp/decomp runs pass. Custom policies and broader material families remain explicit renderer gaps rather than approximations.

### Note (2026-08-30)
Final live falsifiers refined full-block admission: a non-null default fog object is not active fog when its type is GX_FOG_NONE, and GX depth-placement/dithering controls are deliberately outside the renderer-neutral PC material policy. Active fog is still refused. Final guarded runs restored 6,006 recomp and 2,278 decomp cutout/back-cull models.
