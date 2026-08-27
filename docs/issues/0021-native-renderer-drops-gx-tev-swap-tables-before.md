---
id: 21
title: GX compatibility renderer drops GX TEV swap tables before the shipping shader
status: resolved
symptom: TEV CPU reference applies parsed RAS/TEX swap selectors but SDL3-GPU GX compatibility shader consumes unswizzled raster and texture colors
tags: render,recomp,gx-compat,tev,shader,parity
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

`dev_gxfifo.cpp` parsed `swapRas`, `swapTex`, and all four component-remap rows into
`SbrTevState`. The CPU TEV evaluator consumed them, but `SbrNativeTevUniform` had no swap-table
storage and its packed per-stage word omitted both selectors. The shipping fragment shader
therefore passed raw raster and texture colours into TEV even when GX selected a non-identity row.

## What was tried / dead ends

The old state oracle deliberately zeroed the alpha-combiner word's swap bits and compared no swap
rows, so its reported combiner agreement did not cover this state. The old whole-frame and
operation-attribution verdicts cannot measure the fix because their asynchronous Aurora callback
is not joined to the same native frame; C040/C041 and instruments I003/I008 were retracted or
distrusted rather than cited.

## Resolution

The shader uniform now carries all four rows and packs each stage's RAS/TEX selectors. The GLSL
applies the selected row before any TEV colour or alpha argument reads the sampled texel or
rasterised channel. The focused packer test first failed to compile because the shipping uniform
had no table, then passed after transport was added; the pre-existing SDK-derived TEV test remains
the independent semantic control for identity, AAAA, and RGAA rows. `glslc` compilation and
`spirv-val --target-env vulkan1.0` validate the regenerated embedded shader.
