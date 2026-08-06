---
id: C032
kind: claim
status: holds
created: 2026-08-06
tags: interp60
depends: extern/aurora/lib/gfx/interp.cpp
---

## Claim

The 60fps water-refraction reprojection is ALREADY IMPLEMENTED by aurora's stream interpolation: begin_camera_delta computes V_lerp*V_cur^-1 and patch_camera_only applies it to every unpaired perspective draw, which is exactly the immediate-mode identity-PNMTX refraction quad. Per-region alternation, camera rotating: sea 1.03 on path A vs 4.99 on path C (the control).

## Evidence

docs/60fps/effects.md; extern/aurora/lib/gfx/interp.cpp begin_camera_delta; dumps scratch/render/A48d.rgba.* and C48.rgba.*

## What would falsify it

if a headed check shows the reflection SWIMMING relative to the surface, the reprojection is present but wrong — alternation rules out snapping, not swimming
