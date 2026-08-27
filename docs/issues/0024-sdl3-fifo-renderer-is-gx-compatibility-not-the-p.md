---
id: 24
title: SDL3 FIFO renderer is GX compatibility, not the PC-native renderer goal
status: open
state_items: S003, S004, S005
symptom: The path labeled Native consumes GX/FIFO state and reproduces TEV, EFB-copy, and fixed-function semantics, so matching Aurora would only produce a second GameCube renderer rather than a renderer designed around PC-native scene, material, lighting, and effect semantics.
tags: renderer,architecture,recomp,decomp
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The 2026-07-23 renderer doctrine defined native as ownership of the GPU API while keeping GX as the shipping renderer abstraction. That definition confuses a native backend with a native renderer. The current `sms-recomp/runtime/render/` path consumes parsed FIFO/J3D capture plus GX raster, TEV, texture, and EFB-copy state; exact Aurora parity is therefore a compatibility implementation, not the requested endpoint.

## Correct boundary

The PC-native renderer must consume a renderer-neutral, game-semantic scene above GX: meshes and skeleton poses, materials and textures, lights, cameras, particles, 2D/UI, and named screen effects. Its PC shader/material/pass model is authoritative and may intentionally differ from GameCube fixed-function implementation details while preserving game content and behavior.

The recomp path reaches that interface through runtime overrides at verified high-level game draw/resource seams while retaining the recompiled bodies for A/B. The decomp path calls the same interface from native game code. Each runtime owns its own object-layout adapter; neither shares game objects or revives recomp/decomp interop. Guest-dependent behavior must also be named or implemented through decomp.

The existing SDL3 FIFO renderer remains a GX compatibility/reference instrument. Aurora and the compatibility path can validate source interpretation and coverage, but pixel identity to Aurora is not a success condition for the PC-native renderer.

## First semantic vertical slice

Start with `J2DPicture::drawSelf(int, int, Mtx*)` (`0x802cc7c0` in the recomp). The existing
`sms-recomp/overrides/diag_2d.cpp` override is the single registration owner and already proves this
is the point where transformed bounds and live opacity exist. Extend that owner; do not add a
shadowing override.

The recomp adapter must read a renderer-neutral picture value at function entry: pane/resource
identity, transformed rectangle, texture resource and decoded pixels, UV/binding/mirror/wrap policy,
four corner colours, black/white modulation, opacity, transform, clip, and ordering context. It must
not recover the material by running the guest body and snapshotting the resulting FIFO/TEV state,
which is what the current GX compatibility capture does. The decomp-side evidence owner is
`decomp/sms/src/JSystem/J2D/J2DPicture.cpp`; its native-layout adapter constructs the same value
without sharing object layout.

During bring-up the override always runs the original body and sends the semantic picture to an
offscreen PC-native pass for coverage and visual comparison. After the semantic pass is verified,
the selected native mode suppresses only that original picture draw so the visible result bypasses
GX. The retained body remains selectable as the reference and fallback.

## Implemented slice

`native-render/` now owns a GX-free `PictureCommand`, layered picture material, sampler semantics,
`J2DPicture::drawFullSet` crop/binding/mirror/flip resolver, and a guarded picture sink. Recomp's
existing `0x802cc7c0` registration reads raw big-endian guest fields through
`j2d_picture_adapter.cpp` at entry; decomp calls `sb_native_picture_submit` from the corresponding
native function. Both always continue into their original body. Focused controls cover exact mesh
order, crop/transform/UV behavior, mirror+flip behavior, endian field decoding, sampler decoding,
packed blend factors, an invalid sampler, an unmapped matrix, and sink refusal of invalid commands.
`native-render/src/picture_pass.cpp` is the first independent SDL3 consumer. A watched 16x16 GPU
test proves its semantic scissor, decoded 2x2 texture quadrants, half-alpha blend, exact-repeat hash,
and a changed-content/revision hash without consulting GX state. C077 records the combined adapter
and GPU evidence with its falsifier.

This is a rendered offscreen semantic slice, not live game presentation. Runtime producers do not
yet own decoded/versioned RGBA payloads or install a frame sink. They also deliberately carry
`clip.enabled=false`: clip enable, logical canvas/projection, and ordering belong to an enclosing J2D
traversal context and cannot be inferred from this function's stale pane scissor fields.

Next: own decoded/versioned RGBA resources, establish the J2D context stack, and extract one shared
SDL3 device/presenter owner for separate GX-compatibility and semantic targets. Then install the live
sink and render captured J2D pictures visibly while retaining the original bodies for A/B.

## Exit condition

The project goal, doctrine, UI vocabulary, codemap, and first implementation seam all distinguish GX compatibility from PC-native rendering; the native lane renders one representative semantic pass without consuming FIFO/TEV state, with the original guest/decomp draw body retained as a selectable reference.
