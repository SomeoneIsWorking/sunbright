---
id: 13
title: GX compatibility renderer submits GX_CULL_ALL triangles as back-face culled draws
status: resolved
symptom: recomp SDL3-GPU GX compatibility path renders front faces for GX draws whose cull mode is ALL; Aurora and Dolphin emit no fragments
tags: render,recomp,gx-compat,cull,parity
created: 2026-08-26
updated: 2026-08-26
---

## Root cause

SDL exposes rasterizer cull modes for none, front, and back, but not both faces. The native
pipeline mapped GX cull mode 3 (`GX_CULL_ALL`) to SDL back-face culling and submitted the draw, so
front faces still rasterized. Aurora and Dolphin both handle the unrepresentable state by dropping
triangle draws before the GPU backend.

## Control

The focused CPU test preserves NONE, FRONT, and BACK as submit-eligible positive controls and uses
ALL as the negative control. It aborted with exit 134 when the extracted shipping predicate kept
the old always-submit behavior, then passed after the predicate rejected mode 3.

### Resolution (2026-08-26)
The native shipping submission path now applies the CPU-visible raster admission policy before it
queues vertices, batches, textures, or GPU work.

The guarded stage-1 GX-compatibility run on 2026-08-26 was GPU-clean but dropped zero cull-all draws.
It therefore validates renderer health and the instrument's explicit no-coverage result, not live
scene parity for this state.
