---
id: C056
kind: claim
status: holds
created: 2026-08-21
tags: widescreen,hud,re
---

## Claim

The widescreen `te_w` announcement frame and its direct `tet1`/`tet2` text scissor share one
post-projection EFB boundary.

## Evidence

`TGCConsole2::perform` at `0x8014083c` installs `unk544` and directly calls `J2DTextBox::draw`. A
parent-clip edit produced an identical crop, while raw pillar extension overran the frame. A
1280×960 capture showed the complete word inside a continuous announcement band.

## What would falsify it

A same-phase capture clips a glyph before the frame boundary, permits a glyph beyond it, separates
the frame from its fill, or shows a different runtime scissor interval.
