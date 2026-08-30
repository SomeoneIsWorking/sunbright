---
id: C080
kind: claim
status: holds
created: 2026-08-28
tags: renderer,j2d,recomp,decomp
depends: native-render/src/frame.cpp#SemanticFrameCollector::append, native-render/src/semantic_2d_pass.cpp#Semantic2dPass::encode, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_context, sms-boot/runtime/native_j2d_context.cpp#sb_native_picture_context_push
reconfirmed: 2026-08-30
verified_at: 2026-08-30 06:16:40
---

## Claim

The shared PC-native J2D path preserves per-screen logical canvases, physical viewports, hierarchy clips, covered picture/GC2D-solid submission order, and owned decoded-image lifetimes without consuming GX/FIFO/Aurora state.

## Evidence

Root Clang build: 40/40 ctests pass, including frame/context/decomp production-linked controls; recomp Clang build: 28/28 ctests pass including guest context decoding; guarded native_render_semantic_2d_gpu_test distinguishes full-canvas from nonzero sub-viewport output and completed with no kernel GPU fault.

## What would falsify it

Falsified if a real J2DScreen draw produces different ortho/viewport/clip values than either adapter publishes, if collector order/content differs from submission, or if the semantic pass consults GX/FIFO/Aurora state.

## Re-confirmed 2026-08-30

Reverified after replacing the image-only frame with the ordered mixed image/solid-rectangle stream.
The root Clang build and 42/42 tests pass; the recomp Clang build and 28/28 tests pass. The watched
GPU control distinguishes clear, full-canvas, nonzero sub-viewport, and opposite mixed
picture/solid order results, while a fully clipped rectangle is a no-op. Live runtime frames
exercise the same owned canvas/image/order values without adding GX/FIFO/Aurora input to the
semantic client.

## Re-confirmed 2026-08-30

Reverified canvas, clip, order, and image lifetime after adding the active setup2D canvas: scoped screen controls and active direct-picture controls pass, full suites pass, guarded 400-present recomp/decomp audits complete, and the watched GPU ordering control remains fault-free.

## Re-confirmed 2026-08-30

Reverified after adding GlyphDraw and extracting the decomp J2D context owner: frame, sink, context, mixed-order GPU, recomp adapter, and production-linked decomp controls pass; the guarded Delfino run preserved one ordered picture/glyph/solid stream.

## Re-confirmed 2026-08-30

Reconfirmed after adding source-labelled J2D filled boxes and target-pixel clip space to the ordered frame: 43/43 root/decomp and 30/30 recomp tests plus the watched mixed-family GPU control pass.
