---
id: 32
title: Native semantic preview has no J3D model geometry
status: resolved
symptom: The explicit native preview shows ported 2D/UI but every J3D world and character model is absent.
state_items: S004,S005
tags: renderer,semantic,j3d,geometry,material,recomp,decomp
created: 2026-08-30
updated: 2026-08-30
---

The first 3D vertical slice must publish one renderer-neutral J3D mesh/material/pose/camera command from both runtimes and draw it through a PC-native depth-tested pass. Runtime adapters may decode J3D model asset primitives, but native-render must not consume FIFO, BP/XF registers, TEV programs, EFB choreography, or compatibility-renderer state. The retained recompiled/decomp draw bodies remain active for A/B. Start with a mechanically recognized simple material family; refuse unsupported families explicitly and measure the coverage boundary.

### Resolution (2026-08-30)
Both recomp and native decomp now publish rigid unlit single-texture J3D meshes, matrices, decoded RGBA textures, and vertex colour to the shared PC-native depth-tested pass while retaining their original draw bodies. Final guarded post-integration audits submitted 6,512 recomp models over 60 presents and 11,858 decomp models over 180 frames; a broader 400-frame decomp audit submitted 42,852. Broader material/raster semantics remain tracked in project state.
