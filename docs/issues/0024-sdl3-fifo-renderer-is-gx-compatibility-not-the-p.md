---
id: 24
title: SDL3 FIFO renderer is GX compatibility, not the PC-native renderer goal
status: resolved
symptom: The path labeled Native consumes GX/FIFO state and reproduces TEV, EFB-copy, and fixed-function semantics, so matching Aurora would only produce a second GameCube renderer rather than a renderer designed around PC-native scene, material, lighting, and effect semantics.
state_items: S003, S004, S005
tags: renderer,architecture,recomp,decomp
created: 2026-08-28
updated: 2026-08-30
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

`native-render/src/image_decode.cpp` now owns the asset-input conversion shared by both future
runtime producers: all eleven tiled image encodings and IA8/RGB565/RGB5A3 palettes become ordinary
RGBA8 before the renderer sees them. It accepts byte spans rather than guest addresses or GX state,
validates the complete source and palette range, refuses out-of-range indices, uses bit-replicated
component expansion and GX CMPR interpolation, and derives a revision from the exact consumed
content. The recomp GX compatibility path reuses this implementation through a guest-memory adapter;
that reuse does not move FIFO/GX ownership into `native-render/` or count as semantic rendering.
C078 records the CPU controls and its falsifier.

The two game-layout adapters now complete the resource handoff at the high-level draw entry. Recomp
copies exact big-endian guest texel/palette ranges through `GuestByteReader`; decomp reads native JUT
fields while a balanced host-allocation gate owns its temporary vectors. Both decode and hash before
the retained GX body runs, then atomically submit the command with one matching image per texture
layer. A sink cannot accept a command with absent, short, or mismatched image content. Production
controls cover recomp RGBA8 and C4+IA8 layouts plus changed revisions/short-palette refusal, and real
decomp J2DPicture/JUTTexture/JUTPalette objects with C4+IA8/I8 pixels, stable/changed revisions,
transient-span copying, and allocation-gate depth. C079 records the temporal contract.

This is a rendered offscreen semantic slice, not live game presentation. Runtime producers now own
decoded/versioned RGBA for the duration of an atomic submission, and both hosts can explicitly
activate the frame sink without replacing Aurora's visible GX frame.
The enclosing context is now taken from each retained `J2DScreen::draw`: logical `mOrtho`, physical
`mBounds` viewport, `mbClipToParent`, and the exact null-context 640x480 default. The picture leaf
attaches its final hierarchy `mClipRect`; it never uses physical/stale `mScissorBounds`. Mipmapped
resources are refused rather than silently represented as a single level.

`native-render/src/frame.cpp` now supplies the renderer-neutral storage owner: it bounds draws,
unique images, and decoded bytes; copies transient pixels; preserves submission order and each
draw's distinct canvas/viewport;
coalesces only exact resource/revision/content matches; rejects conflicting identities; and seals a
stable semantic frame for a renderer client. Its changed revision, multiple-canvas, and every
limit/lifecycle refusal control run through the production collector. A guarded GPU control maps the
same logical picture into a nonzero sub-viewport and must differ from the full-canvas frame (C080).

`native-render` now owns one process-level `SemanticFrameBridge` and one SDL GPU platform instead of
runtime-local copies. Both runtimes call the bridge at their exact frame boundaries. The recomp
seals after retained wait/scheduler work and immediately before `gxfifo_build()`, then begins after
optional subframe presentations; the decomp begins at `sb_frame_seam_start` and seals inside the
host-allocation gate at `sb_frame_present`. The bridge remains inert unless host composition
explicitly activates the audit, so ordinary output does not change. Its sink lease refuses a second
owner and prevents an unrelated caller from clearing the active sink.

The SDL platform copies its dispatch table, requires host-owned SDL video initialization, owns the
only GPU device/window claim/presenter, and refuses shutdown while client frame targets remain. The
old recomp presenter/device implementation is deleted; the GX compatibility renderer now consumes
this shared platform with its own target. `PicturePass` encodes into a borrowed command buffer and
target, and its image-cache transaction is committed only after the caller reports submission or
rolled back after cancellation. Current-frame residency is bounded instead of retaining every
historical revision. CPU lifecycle controls, the watched sRGB picture GPU control, and a bounded
130-present GX/Aurora run pass. C081 records the exact explicitly activated bridge and C082 the
shared GPU/submission ownership contract.

Host composition now activates the bridge and encodes its sealed frame through an offscreen semantic
target. The remaining visible-presentation gap is tracked separately: expand the unified order
stream to text/windows/fills before bypassing GX, because a picture-only overlay cannot preserve
their interleaving. Retain the original bodies for A/B.

## Exit condition

The project goal, doctrine, UI vocabulary, codemap, and first implementation seam all distinguish GX compatibility from PC-native rendering; the native lane renders one representative semantic pass without consuming FIFO/TEV state, with the original guest/decomp draw body retained as a selectable reference.

### Note (2026-08-28)
Added the shared renderer-neutral texture asset decoder: all eleven tiled encodings, three palette encodings, exact range/index checks, hardware component expansion and CMPR interpolation, mip sizing, and content revisions. The GX compatibility adapter now reuses it while retaining guest/GX ownership. This enables semantic resource publication but does not count as a live PC-native render pass (C078).

### Note (2026-08-28)
Both J2DPicture adapters now decode exact JUT texel/palette bytes and atomically submit matching versioned RGBA images with each semantic command before the retained GX body. Recomp and production-linked decomp controls cover exact pixels, changed revisions, refusal paths, transient lifetime, and the decomp host-allocation gate (C079).

### Note (2026-08-28)
Added the bounded SDL-free semantic frame collector and rejected a frame-wide canvas during its control review. Both runtimes now scope the retained J2DScreen body and attach its copied logical ortho rectangle, physical viewport, clip-enable state, and the leaf's final hierarchy clip to every ordered draw. The collector owns decoded image bytes, coalesces only identical resource revisions, rejects conflicting keys and every configured limit, and preserves multiple canvases per frame; a nonzero sub-viewport GPU control proves the distinction (C080).

### Note (2026-08-30)
Added one shared semantic frame bridge at the exact recomp and decomp frame boundaries and one shared SDL GPU platform for the device, optional window presenter, and independent client targets. Explicit host audit composition now activates the bridge offscreen; ordinary output remains inert. The GX compatibility renderer was migrated to the shared platform and its duplicate presenter/device owner was removed.

### Resolution (2026-08-30)
Root cause was the old doctrine equating a native GPU backend with a native renderer. The project now classifies FIFO/TEV reproduction as GX compatibility, keeps it as a reference, and implements a retained-body above-GX J2DPicture pass shared by recomp and decomp. Both runtime hosts activate and submit that pass offscreen; visible presentation remains separate atomic work.
