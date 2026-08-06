---
id: C032
kind: claim
status: falsified
created: 2026-08-06
tags: interp60
depends: extern/aurora/lib/gfx/interp.cpp
falsified_on: 2026-08-06
---

## Claim

The 60fps water-refraction reprojection is ALREADY IMPLEMENTED by aurora's stream interpolation: begin_camera_delta computes V_lerp*V_cur^-1 and patch_camera_only applies it to every unpaired perspective draw, which is exactly the immediate-mode identity-PNMTX refraction quad. Per-region alternation, camera rotating: sea 1.03 on path A vs 4.99 on path C (the control).

## Evidence

docs/60fps/effects.md; extern/aurora/lib/gfx/interp.cpp begin_camera_delta; dumps scratch/render/A48d.rgba.* and C48.rgba.*

## What would falsify it

if a headed check shows the reflection SWIMMING relative to the surface, the reprojection is present but wrong — alternation rules out snapping, not swimming

## FALSIFIED 2026-08-06

The reprojection is APPLIED but INCOMPLETE, which is worse than absent for this draw. GX_TG_POS texgen uses the RAW vertex attribute (lib/gx/shader.cpp:1327), and no interpolation path patches TEXTURE matrices — only position and normal. So the refraction quad is moved to the interpolated camera while its screen UVs still map to tick N: the reflection is sampled at the old mapping and drawn at the new position. User reports the water 'not sure if judder or wrong positioning' — it is positioning. Verified: with SBR_INTERP_CAMONLY toggled, the ONLY pixels that change on an in-between frame are the water surf band (2.16%, 99.5% top-decile), scratch/screenshots/campatch_delta.png

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
