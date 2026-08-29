---
id: C080
kind: claim
status: holds
created: 2026-08-28
tags: renderer,j2d,recomp,decomp
depends: native-render/src/frame.cpp, native-render/src/picture_pass.cpp, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed, sms-recomp/overrides/j2d_picture_adapter.cpp#capture_j2d_context, sms-boot/runtime/native_picture_adapter.cpp#sb_native_picture_context_push
reconfirmed: 2026-08-30
verified_at: 2026-08-30 02:49:26
---

## Claim

The shared PC-native J2D path preserves per-screen logical canvases, physical viewports, hierarchy clips, painter order, and owned decoded-image lifetimes without consuming GX/FIFO/Aurora state.

## Evidence

Root Clang build: 40/40 ctests pass, including frame/context/decomp production-linked controls; recomp Clang build: 28/28 ctests pass including guest context decoding; guarded native_render_picture_gpu_test distinguishes full-canvas from nonzero sub-viewport output and completed with no kernel GPU fault.

## What would falsify it

Falsified if a real J2DScreen draw produces different ortho/viewport/clip values than either adapter publishes, if collector order/content differs from submission, or if the semantic pass consults GX/FIFO/Aurora state.

## Re-confirmed 2026-08-30

Current Clang builds pass 42/42 root and 27/27 recomp tests. The watched production GPU control still distinguishes clear, full-canvas, and nonzero sub-viewport results. Live runtime frames exercise the same owned canvas/image/order values without adding GX/FIFO/Aurora input to the semantic client.
