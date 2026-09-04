---
id: C096
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,pose
depends: native-render/src/model.cpp#build_model_pose, native-render/src/model.cpp#transform_vertex
---

## Claim

Renderer-neutral J3D poses use a compact palette for rigid and multi-matrix geometry; renderer input
contains semantic pose bindings rather than GX matrix slots or FIFO matrix loads.

## Evidence

Focused controls move only the vertex bound to the second pose matrix and reject invalid palette
indices, incomplete palettes, and mismatched bindings. The ordinary and multi-matrix paths agree on
the same compact-palette contract.

## What would falsify it

A compact palette moves the wrong vertex, accepts an invalid binding, or the renderer begins
consuming GX matrix-slot or FIFO-load state.
