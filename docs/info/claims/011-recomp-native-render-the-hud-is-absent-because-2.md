---
id: C011
kind: claim
status: falsified
created: 2026-07-29
tags: render
depends: sms-recomp/runtime/render/scene.cpp, sms-recomp/overrides/j3d_capture.cpp
falsified_on: 2026-08-12
---

## Claim

recomp native render: the HUD is absent because 2D/J2D geometry is NEVER CAPTURED, not because it is mis-shaded — 0 orthographic drawables of 839 captured

## Evidence

sbr_scene_report_2d() projection census; debug_journal 2026-07-29 iteration 16

## What would falsify it

the projection census reporting a nonzero orthographic count while the HUD is still missing

## FALSIFIED 2026-08-12

The 'never captured' half is now false by construction: SBR_FIFO_2D decodes 34601 of 34615 orthographic draws per plaza run (dev_gxfifo.cpp decode_2d_draw), where it captured 0. The claim's OTHER half is UNMEASURED, not confirmed — whether the HUD now appears needs a native-renderer run, which is gated behind SBR_RENDER_APPROVED and a human. Superseded by C043.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
