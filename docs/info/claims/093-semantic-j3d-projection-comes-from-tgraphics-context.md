---
id: C093
kind: claim
status: holds
created: 2026-08-30
tags: renderer,camera,j3d,re
depends: native-render/src/model_context.cpp#capture_j3d_scene_context
---

## Claim

Semantic J3D projection belongs to the high-level `TGraphics` draw context, not GX projection,
FIFO, or compatibility-renderer state.

## Evidence

Controlled perspective, orthographic, empty-scope, and nested-scope tests prove capture and restore
of the renderer-neutral scene context. Source inspection confirms the semantic owner has no GX
projection-cache dependency.

## What would falsify it

Retail call flow establishes a different high-level projection owner, nested context restoration
fails, or semantic model submission begins reading GX/FIFO projection state.
