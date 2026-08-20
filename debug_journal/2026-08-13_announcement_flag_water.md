# Announcement width and deforming-UV interpolation

The existing interpolation audit reported the flag and sea vertex populations at effectively 100%,
yet both visibly jittered. The report only measured whether XYZ was patched. Retail
`TMapObjFlag::draw` emits XYZ+ST; `TMapObjWave::draw` emits XYZ+CLR0+ST+ST and advances the texture
offsets every simulation tick. Their geometry was smooth while their UV animation still stepped at
30 Hz.

Aurora now derives every DIRECT f32 NRM/TEX byte offset from the live VAT and interpolates those
values beside XYZ. Offsets are bytes because direct matrix-index attributes can make later floats
unaligned. The self-test places ST at byte offsets 13 and 17 and requires exact halfway XYZ and ST
values. Runtime evidence (`scratch/logs/recomp_announcement_fixed2.log`, local/untracked): flags
11,169/11,178 with 1,300,568 extra values; sea 1,241/1,242 with 6,711,328 extra values; GPU clean.

For the scrolling announcement, `/2dclass` identified text panes `tet1`/`tet2` and `J2DWindow`
pane `te_w`; its content rect was 427 units wide. `J2DWindow::draw_private` does not position its
frame at the outer rect's `x1`: it emits at local x=0 and uses only `x2-x1` for the width. The first
fix subtracted the pillar from both rects' `x1` while leaving the pane transform unchanged. That
made the frame grow rightward from its old origin while the content grew around another origin,
which is the pair of detached translucent side boxes reported on 2026-08-21. The old pixel scan
proved text coverage and clipping only; it never tested that the nine-slice and fill shared a
coordinate system, so its conclusion was too broad.

The corrected transform moves the pane's global X translation left by one pillar and adds two
pillars to the right edge of both outer and content rects. Their effective centres and authored
insets therefore stay fixed. `hud_window_layout_test` exercises the shipping transform and failed
against the old symmetric-rect mutation before passing with the centered transform. A windowless
1280x960 capture at present 1200 shows one continuous translucent band, matching the 4:3 control's
structure.

The remaining early text cutoff was not `J2DWindow::clip`. The DOL's
`TGCConsole2::perform` (`0x8014083c`) draws the HUD screen first, constructs a scissor from
`unk544` (guest offset `0x548`), calls `GXSetScissor`, and then invokes `J2DTextBox::draw`
directly for `tet1`/`tet2`. Widening the parent child clip produced a pixel-identical UI crop,
which falsified that hypothesis. Symmetrically adding the 107-unit HUD pillar to `unk544` then
made the text exceed the box, revealing the second coordinate mismatch: `te_w` passes through the
0.75-wide 2D projection, while GX scissor coordinates do not.

The shipping path now projects the widened frame's actual concatenated matrix span into EFB
coordinates and feeds that interval to the console's scroll update and scissor draw. The helper
test failed first with an empty projection result and now requires the exact projected endpoints.
The windowless 1280x960 present-1200 capture shows the full word `several` inside the continuous
frame. The run exited normally and the kernel reported no GPU timeout, reset, or fault. Caller
rectangles, pane transforms, and the retail scissor are restored after each draw.
