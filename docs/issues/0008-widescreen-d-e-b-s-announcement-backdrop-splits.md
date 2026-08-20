---
id: 8
title: Widescreen D.E.B.S. announcement backdrop splits into overlapping rectangles
status: resolved
symptom: The widescreen D.E.B.S. alert had detached backdrop ends, then clipped text before the corrected box boundary.
tags: widescreen,hud,j2d,reported
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

`J2DWindow::draw_private` emits the frame at local x=0 and uses the outer rect only for its
width. The old override subtracted the widescreen pillar from `outer.x1`, which therefore did not
move the frame, while the content rect was expanded around another origin. The nine-slice frame
and translucent fill separated into the reported side boxes.

The follow-on text cutoff had a separate owner. `TGCConsole2::perform` bypasses the J2D child
traversal: it installs `unk544` as a raw GX scissor, then calls `J2DTextBox::draw` directly for
`tet1`/`tet2`. Expanding a parent window clip could not affect those calls. Adding the same pillar
to `unk544` was also incorrect because `te_w` is squeezed by the 2D projection while raw GX
scissor coordinates are not.

## What was tried / dead ends

The 2026-08-13 capture proved that the scrolling text remained inside the widened backdrop, but
coverage and clipping did not test frame/fill alignment. A 4:3 control rendered a continuous band,
ruling out the game's original window textures as the source of the detached boxes.

A drawSelf-scoped parent-clip change passed its geometry test but produced a pixel-identical UI
crop, falsifying the child-clip hypothesis. A symmetric raw-scissor extension exposed its own
coordinate mismatch by letting text run beyond the corrected frame.

## Resolution

The tested `hud_window_layout` seam shifts the pane matrix left one pillar and adds two pillars to
both right edges, preserving effective centres and authored insets. It separately projects that
frame's concatenated matrix span through the active 2D scale and uses the resulting EFB interval
for the console scroll/scissor path. The 1280x960 windowless capture has one continuous band and
shows the complete word `several` inside it; the GPU-health gate is clean.

### Resolution (2026-08-21)
Resolved both coordinate owners: te_w frame/content share one centered transform, while TGCConsole2's direct tet1/tet2 GX scissor is derived from the frame's post-projection EFB interval. The present-1200 windowless capture shows the complete word 'several' inside the continuous band; GPU health is clean.
