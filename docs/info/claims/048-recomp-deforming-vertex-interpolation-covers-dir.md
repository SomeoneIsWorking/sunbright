---
id: C048
kind: claim
status: holds
created: 2026-08-13
tags: recomp,interpolation
depends: extern/aurora/lib/gfx/interp.cpp#patch_vertices
---

## Claim

Recomp deforming-vertex interpolation covers direct f32 texture coordinates, not only XYZ

## Evidence

interp_pairing selftest exact unaligned ST midpoint; run-safe 1250 ticks: flag extras 1300568, sea extras 6711328, GPU clean (debug_journal/2026-08-13_announcement_flag_water.md)

## What would falsify it

a run with SBR_LERP60=1 shows flag or sea extras=0, or a frame comparison shows their UVs still advance only on simulation ticks
