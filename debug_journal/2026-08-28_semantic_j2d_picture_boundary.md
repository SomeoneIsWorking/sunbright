# Semantic J2D picture boundary above GX (2026-08-28)

The project-owned SDL3 FIFO renderer is still a GX implementation even when its pixels match
Aurora. The first genuinely PC-native boundary is now at
`J2DPicture::drawSelf(int, int, Mtx*)`: both runtimes publish a renderer-neutral picture value before
their retained GX body runs.

## What the function actually provides

The recomp entry at `0x802cc7c0` receives the picture in `r3` and a parent 3x4 matrix in `r6`.
Geometry is not simply `mGlobalBounds`. `drawFullSet` first crops/anchors the local pane extent from
texture-0 size, binding, layout wrap, mirror, and flip; `drawTexCoord` then applies
`parentMatrix * mGlobalMtx`. The shared resolver implements that exact value-level contract. The
integer x/y arguments cancel before local geometry reaches the transform.

The layered material is also game-semantic rather than an arbitrary TEV capture: texture 0 seeds the
sample, later textures mix using the already-derived packed blend factors, black/white remaps the
result, corner colour multiplies it, and live inherited `mColorAlpha` supplies opacity. The picture
fields previously named `unk104`, `unk114`, and `unk130` are now named `mBlendColorWeights`,
`mBlendAlphaWeights`, and `mFlip` in decomp.

## Boundary limits

The leaf entry alone does not carry active clip enable or logical canvas/projection. Those values
now come from an enclosing scope around the retained `J2DScreen::draw`: its supplied
`J2DOrthoGraph::mOrtho` is the logical rectangle, base `mBounds` is the physical viewport, and
`mbClipToParent` controls clipping. The null-context branch contributes its exact constructed
640x480 graph. At the leaf, the producer attaches `mClipRect`, which already contains hierarchy
intersection. It deliberately never uses `mScissorBounds`, which is physical/GX-oriented and can be
stale when clipping is disabled.

Texture identity, sampler state, and decoded/versioned RGBA content are now captured together at
draw entry. The sink receives the command and every matching image as one synchronous operation, so
it must copy the transient views before return and cannot defer a guest-pointer read until frame end.
Both runtimes decode before their retained GX body runs. The independent SDL3 `Semantic2dPass` rendered
a watched 16x16 control: known texture quadrants, clipping, half-alpha, repeat determinism, and
changed revision/content all produced the required answers without a kernel GPU fault. Both live
runtime collectors are now installed behind an explicit audit switch; the result remains offscreen
and does not establish visible PC-native game presentation.

## Asset decoding is not the renderer boundary

Game-authored textures still use GameCube tiled storage. Converting those files to ordinary RGBA8 is
an input codec, just as decoding PNG would be; it does not make the PC renderer a GX renderer.
`native-render/src/image_decode.cpp` therefore owns only span-based content decoding and revisioning.
It has no guest address, FIFO, GX command, TEV, Aurora, SDL, or GPU type. Runtime layout adapters own
where the bytes came from, and the semantic renderer owns only decoded image values.

The extraction also corrected two inaccuracies in the old compatibility decoder before reuse:
RGB565/RGB5A3 components expand by hardware bit replication, and CMPR uses the GX 3/8 blend while
retaining the midpoint RGB for its alpha-zero selector. Focused controls cover all eleven image
encodings, all three palette encodings, tiling and mip sizes, exact bounds, invalid indices, both
CMPR branches, and source/palette changes that must alter the content revision. This is enabling
resource work, not a claim that the game is visibly rendering through the PC-native path.

The decomp adapter's temporary decoded buffers require special ownership because it runs on the game
thread, where ordinary C++ allocation routes to JKR. Its entire decode-and-submit lifetime is now
inside `sb_host_alloc_push/pop`. A production-linked test constructs real native J2DPicture,
JUTTexture, and JUTPalette objects, verifies the allocation depth in the sink callback, copies the
transient spans, and checks exact C4+IA8 and I8 output. That test also exposed the native default
`JUTTexture()` constructor leaving seventeen fields indeterminate; the native-only empty state now
initializes them, while the non-native decomp constructor remains unchanged.

## Frame ownership

`native-render/src/frame.cpp` now owns the lifetime boundary after atomic submission. Its collector
is independent of SDL and both game runtimes: it stores one ordered `SemanticDraw` variant stream,
currently covering pictures and GC2D solid rectangles. Pictures retain each screen's distinct
canvas/viewport, and the collector copies new decoded RGBA resources into configured
operation/image/byte bounds. Resource/revision pairs are
coalesced only when dimensions and bytes agree; disagreement is a producer error, not a cache miss.
Seal exposes stable spans until the next frame begins. Controls mutate the caller's source after
submission, exercise duplicate and changed-revision answers, and force every capacity and lifecycle
failure. A guarded SDL3 control uses a nonzero physical sub-viewport and must differ from the same
logical draw on the full canvas. Direct `J2DPicture::draw` calls bypass the screen scope. The later
solid-rectangle slice proves mixed picture/fill order, but text, windows, generic fills, and 3D
remain absent; see `2026-08-30_semantic_solid_rectangle_ordering.md`.

## Exact frame seams and shared GPU ownership (2026-08-30)

Both runtimes now call a common `SemanticFrameBridge` at their real frame boundaries. The first
recomp attempt sealed before the retained `JDrama::TVideo::waitForRetrace` body. That was not the
frame boundary: retained wait and guest-scheduler work can still publish draws. Seal now happens in
`present_tail` immediately before `gxfifo_build()`, after that work, and the next begin happens only
after optional subframe presentations. The decomp begins at `sb_frame_seam_start`, seals inside the
host-allocation gate at `sb_frame_present`, and begins again after Aurora starts the next frame.

The bridge owns the semantic sink through a lease instead of a raw setter. This fixes two authority
failures in the initial design: a second owner could silently steal the collector, and an unrelated
caller could clear it. Begin/seal remain successful no-ops while inactive. Each runtime's explicit
semantic-frame audit now activates the bridge; the ordinary product path leaves it inert. This is
still an exact frame/lifetime seam and offscreen liveness proof, not evidence that a semantic frame
is visible.

The initial SDL host split also had multiple lifetime defects: it retained pointers to caller-owned
dispatch tables, initialized SDL video behind the host's back, exposed an unsupported sample-count
knob, allowed platform shutdown while targets still referenced the device, and duplicated the
recomp compatibility presenter's device/window authority. The shared platform now copies its call
table, requires host-owned SDL video initialization, fixes semantic targets at sample count one,
refuses shutdown with live targets, and owns the optional sole window claim/presenter. Device-only
initialization lets the semantic client own a target without claiming Aurora's window. Aurora's
independent WebGPU device remains the visible GX reference; its pipeline compiler is paused only
around SDL device creation to preserve the previously proven Vulkan-loader race fix. The GX
compatibility renderer consumes the same SDL platform with its own target; the old duplicate SDL
presenter/device implementation is gone.

`Semantic2dPass::encode` now borrows the host command buffer and target. Resource publication is a
transaction: newly staged textures enter the resident cache only after the caller confirms submit,
and cancellation rolls them back. Entries not referenced by the submitted current frame are
evicted, preventing every historical content revision from accumulating forever. A watched sRGB
GPU control exercises an exactly clear empty frame, known non-clear mixed picture/solid output,
opposite results after reordering, changed-revision residency, and duplicate-sequence refusal. The migrated GX compatibility path
completed 130 presents plus four exact-frame Aurora joins without a kernel GPU fault.

## Live offscreen runtime evidence (2026-08-30)

`SdlSemanticFrameClient` is now the shared host consumer. It owns a 640x480 no-depth target,
`Semantic2dPass`, fenced submission, and readback-until-nonclear; it consumes one sealed sequence once
and never claims or presents a window. Recomp composition lives in
`sms-recomp/host/render_composition.*`; decomp composition lives in
`sms-boot/runtime/semantic_render.*`. Both initialize before the first semantic begin, encode after
seal and before the next begin, and shut the semantic client/platform down before Aurora. Decomp's
encode and teardown paths stay inside the host-allocation gate.

A guarded recomp title run bounded at 100 presents submitted and completed 50 semantic simulation
frames. All 50 were nonempty, six mixed pictures and solids, and the stream carried 1,302 pictures
plus 14 solid rectangles. A guarded decomp title run bounded at 400 presents submitted and completed
400 semantic frames. Three hundred fifty were nonempty, eleven mixed both families, and the stream
carried 9,207 pictures plus 64 solid rectangles. Both launcher runs exited cleanly and the watcher
found no kernel GPU fault. Exact readback counts are recorded in the solid-ordering journal.

These numbers prove the real runtime producers, bounded collector, shared semantic pass, submission,
and readback are live. One non-clear sample does not prove appearance correctness, sustained output,
complete J2D coverage, or cross-runtime parity. Text, windows, generic J2D fills, direct picture
callers, 3D, mip chains, particles, lights, and effects remain outside the stream; presenting the
partial target would destroy authored interleaving and is therefore still forbidden.
