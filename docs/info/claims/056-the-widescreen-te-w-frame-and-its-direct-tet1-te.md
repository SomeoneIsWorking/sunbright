---
id: C056
kind: claim
status: holds
created: 2026-08-21
tags: recomp,widescreen,hud
depends: sms-recomp/overrides/hud_window_layout.cpp#project_frame_to_scissor
reconfirmed: 2026-08-21
verified_at: 2026-08-21 02:23:31
---

## Claim

The widescreen te_w frame and its direct tet1/tet2 scissor share one post-projection EFB boundary

## Evidence

TGCConsole2::perform at 0x8014083c installs unk544 then directly calls J2DTextBox::draw; a parent-clip edit produced a pixel-identical crop, and a raw pillar extension overran the frame. hud_window_layout_test requires the frame projection result; the 1280x960 present-1200 capture shows the full word several inside the continuous band; run-safe reported zero GPU events.

## What would falsify it

A same-phase widescreen capture clips a glyph before the te_w boundary, lets a glyph cross beyond it, separates the frame/fill, or the runtime scissor differs from the helper's projected frame interval

## Re-confirmed 2026-08-21

Confirmed at commit b6da808: hud_window_layout_test passes the exact projected-frame endpoints, the 1280x960 present-1200 capture shows the complete word several inside the continuous band, and run-safe recorded zero GPU events.
