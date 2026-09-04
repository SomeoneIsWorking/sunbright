---
id: C077
kind: claim
status: holds
created: 2026-08-28
tags: renderer,semantic,j2d
depends: native-render/src/semantic_2d_pass.cpp#Semantic2dPass::encode
---

## Claim

The renderer-neutral J2DPicture command carries decoded RGBA content, canvas, viewport, clip,
transform, colour, and UV policy without consuming FIFO, TEV, or Aurora state.

## Evidence

The watched semantic 2D GPU controls distinguish clipped 2×2 quadrants, half alpha, changed image
revisions, full-canvas versus sub-viewport output, mixed-operation order, and a fully clipped no-op.
An exact repeat remains byte-identical and the controls completed without a kernel GPU fault.

## What would falsify it

The pass reads GX-era state, accepts a command without matching image content, changes operation
order, or fails a known-equal or known-different GPU control.
