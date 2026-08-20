---
id: 8
title: Widescreen D.E.B.S. announcement backdrop splits into overlapping rectangles
status: resolved
symptom: The scrolling D.E.B.S. alert has a central translucent band plus separately aligned dark side rectangles in widescreen; 4:3 renders one continuous band.
tags: widescreen,hud,j2d,reported
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

`J2DWindow::draw_private` emits the frame at local x=0 and uses the outer rect only for its
width. The old override subtracted the widescreen pillar from `outer.x1`, which therefore did not
move the frame, while the content rect was expanded around another origin. The nine-slice frame
and translucent fill separated into the reported side boxes.

## What was tried / dead ends

The 2026-08-13 capture proved that the scrolling text remained inside the widened backdrop, but
coverage and clipping did not test frame/fill alignment. A 4:3 control rendered a continuous band,
ruling out the game's original window textures as the source of the detached boxes.

## Resolution

The tested `hud_window_layout` seam shifts the pane matrix left one pillar and adds two pillars to
both right edges, preserving the effective centres and authored insets. The 1280x960 windowless
capture now has one continuous band, all CTest targets pass, and the GPU-health gate is clean.
