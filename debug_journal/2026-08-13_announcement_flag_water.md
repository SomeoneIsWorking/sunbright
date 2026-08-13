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
pane `te_w`; its content rect was 427 units wide. The fix widens both outer and content rectangles
at `J2DWindow::draw_private` by the shared widescreen pillar, then restores them. A content-only
attempt was rejected because the unchanged outer clip cut the extension off. The final code restores
`unkEC` before `J2DWindow::drawSelf` applies it to child panes, which keeps `tet1`/`tet2` structurally
clipped inside the box through every scroll phase. Pixel bounds in the tick-1202 1280x960 capture:
box x=61..1085, near-white text x=149..999 (88 px left and 86 px right padding), nearest FLUDD HUD
pixel x=1120 (34 px clear gap). The measurement scanned all 58 announcement rows and 194 columns to
the right; it matched 7,486 text pixels and 137 HUD-bearing columns rather than silently reporting
an empty result.
