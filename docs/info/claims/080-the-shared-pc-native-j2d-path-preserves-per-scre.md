---
id: C080
kind: claim
status: holds
created: 2026-08-28
tags: renderer,j2d
depends: native-render/src/frame.cpp#SemanticFrameCollector::append, native-render/src/semantic_2d_pass.cpp#Semantic2dPass::encode, native-render/src/sdl_semantic_frame_client.cpp#SdlSemanticFrameClient::encode_last_sealed
---

## Claim

The PC-native J2D path preserves per-screen logical canvases, physical viewports, hierarchy clips,
mixed picture/glyph/solid submission order, and owned decoded-image lifetimes without consuming
GX, FIFO, or Aurora state.

## Evidence

Collector and GPU controls distinguish clear, full-canvas, nonzero sub-viewport, opposite mixed
operation order, changed image content, and a fully clipped no-op. The active-canvas, glyph, and
source-labelled filled-box controls exercise the same ordered frame contract.

## What would falsify it

Collection changes command order or ownership, a real J2D draw produces different canvas or clip
values than the semantic contract, or the pass consults compatibility-renderer state.
