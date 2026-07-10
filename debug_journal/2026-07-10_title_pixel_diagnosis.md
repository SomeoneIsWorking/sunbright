# 2026-07-10 — Title screen PIXEL-level diagnosis (native vs retail)

Grounded the title-parity work in actual pixels (prior work was structural draw-counts).
Native captured at 640×480 window (1280×960 internal, 2.5×) via `SB_DUMP_FRAME` +
`SB_DUMP_FRAME_AFTER=<presents>`, converted RGBA→PNG. Reference = the settled PRESS START
title `scratch/oracle/frames/check_3800.png` (NOT `title_gx_oracle.png`, which is the LATER
file-select screen — an easy wrong-reference trap).

## State check first (important)
- Native SB_STAGE=15 shows the **PRESS START title logo** (Super Mario Sunshine over sky),
  the correct state — NOT the file-select. `title_gx_oracle.png` (file-select) is a later
  state reached only after pressing START; don't diff against it for the title.
- Native frame 120 and 1200 are both the (broken) title; **frame 3000 is BLACK** — by then
  the attract loop (~45 s idle, CardLoad.cpp) has advanced to a THP attract movie native
  can't decode. So the title window is ~frame 300–2700; sample there, not later.

## The defect: blurry, oversized, blue-white-washed title
Retail: crisp "SUPER MARIO SUNSHINE" logo (~60% of frame) over a light-blue sky with white
clouds + a bright localized sun-flare, palm tree, rainbow trail, ©2002 NINTENDO.
Native (frame 1200): the SAME elements are present (SM letters, SUNSHINE, rainbow, ©2002
NINTENDO all identifiable) but **heavily blurred, the logo blown up to fill the screen, and
the whole frame washed to blue-white.**

Pixel stats (mean RGB / fully-white fraction):
- NATIVE  [210, 210, 254.7]  19.6% fully white  — **blue channel saturated everywhere.**
- ORACLE  [143.7, 177.2, 201.1]  7.8% white.
- Per-frame, NOT accumulation: frame 120 is already washed [190, 217, 241].

## Hypotheses FALSIFIED (do not re-chase for the visible defect)
- **Phase-1 ghost pass**: `SB_SKIP_GHOST=1` drops it (293→166 draws, ortho 125→32) →
  final image **BIT-IDENTICAL**. The ghost draws the 3D scene under stale ortho, which maps
  it offscreen/inert — it is wasted work but contributes ZERO pixels. (Added SB_SKIP_GHOST
  probe in MarDirectorDirect.cpp; unk40 holds only `drawBufferGroup,8`.) So the ghost is a
  perf/structural wart, NOT the parity blocker — stop treating "delete the ghost" as the fix.
- **EFB-copy-sampling quads**: `SB_SKIP_COPY_QUAD=1` → identical. Not the compositor quads.
- **Lens flare**: `SB_SKIP_MARK=LensFlare` → identical. Not the sun-flare/bloom.
- **Accumulation/bad-clear**: early frame already washed → per-frame, not cross-frame.

## Partial contributors CONFIRMED
- **Sky**: `SB_SKIP_MARK=Sky` drops the blue wash noticeably (B 254.7→222.7, R/G 210→170),
  but the white blobs + blurry oversized logo REMAIN. So the sky is over-bright/too-blue but
  is only part of it.
- **Screen texture is HALF-res** (`new JUTTexture(SMSGetGameRenderWidth()/2, …)`,
  ScreenUtil.cpp:218) — 320×240. Retail uses this too, so half-res alone isn't the extreme
  blur, but a broken composite that upscales it over the whole frame would be.

## NDC-probe finding: the 3D scene is MIS-FRAMED (maps outside NDC)

`SB_NDC_PROBE` at the title (min-verts 25–50, perspective + ortho): essentially NO 3D
draw lands inside the visible NDC box. The 202v sky sphere (scale-100000 `GXDrawSphere`)
maps to ndcX ±343..754 (drifting — the map==15 sky spin), `inXY=0`, both under ortho AND
perspective — invisible by geometry (retail draws the same 202v dome `cU=0`, also invisible,
so consistent). The largest in-view thing is a 42v `DrawBuf MapOpa` under ORTHO with a single
vertex barely inside (ndcX up to 138). So native's title 3D backdrop is projected almost
entirely OUTSIDE the frame; the visible blurry blue-white is the 2D/composite layer with no
correct 3D behind it.

Native sky posmtx `M=[-0.595 0 -0.804 -335.1 | 0.650 0.588 -0.481 -132.2 | 0.473 -0.809
-0.350 -96.1]` (a rotation + translation) where **retail's 202v sky posmtx is IDENTITY**.
The scene is drawn camera-relative, so this posmtx IS (view · model); a wrong view matrix
mis-frames the whole 3D scene. This points at the **title camera / view matrix (camera1 +
"J3D System Set View Mtx")**, NOT blend/bloom/fog. This is the same camera-relative math in
`TSky::perform` (the `// TODO: match this awfulness` hand-transcribed inverse-camera formula)
— re-examine it and the camera1 view matrix that feeds the title world pass.

## ROOT CAUSE (confirmed): native color-draws the title; retail draws Z-only + composites

Added blend/cU to the draw dump (aurora: `bm sf df cU aU`). Native settled title = 293 draws,
**~290 are cU=1** (color-writing). Retail settled title = 1258 draws, **only 71 cU=1** — the
entire 3D world+mirror is drawn `color_update=0` (Z-only) and the visible image is composited
from the EFB snapshot (the 26× 52-vert block after EFB copy #2). Retail's dome/sky raster:
`blend_enable=1, color_update=0`. Native's `DrawBuf Sky Xlu`: `cU=1, sf=1(ONE)/df=3(INVSRCCLR)`
— it PAINTS the sky (bright blue) where retail paints nothing, which is the blue-white wash.

So the camera is fine (framing ≈ correct; the up-at-sky camera settles cleanly — pos
(1095,328,-13) → target (532,1136,158), fovy 40 = retail's proj), blend is mostly normal,
additive draws are offscreen (LensFlare/Sky-sphere, proven inert). The defect is
**architectural**: native lacks retail's Z-only-then-EFB-snapshot-composite path for the title
and instead directly color-draws every pass. The blown-out/blurry result is that direct render
(over-bright sky blend + the 256×256 mirror pass + no snapshot composite) instead of retail's
composited frame.

**The fix = port the title EFB-copy compositor + color-update management** (the named work):
retail's `TEfbCtrl`/`TEfbCtrlTex` (`JDREfbCtrl.cpp`) set `GXSetColorUpdate(false)` for the 3D
passes and drive the mid-scene EFB copy; the 26-draw compositor block then paints the visible
backdrop by sampling that snapshot. Native's EfbCtrl objects aren't managing cU / aren't doing
the mid-scene copy, so everything falls through as a direct cU=1 draw. This is a subsystem port,
not a one-line fix. Diagnostics wired for it: SB_DRAW_DUMP_FRAME now prints vp + bm/sf/df/cU/aU;
SB_SKIP_GHOST, SB_CAM_DBG, SB_NDC_PROBE. NOTE: SB_SKIP_ORTHO and SB_SKIP_MARK=Mirror both HANG
the boot (fades/mirror-copy are load-bearing) — don't use them.

## Leading (unverified) hypothesis for next session
The logo/scene appears **oversized** (SM letters fill the screen vs ~60% in retail) +
blurry. That reads as a SCALE/PROJECTION divergence in the title's perspective pass (retail
world proj diag = [2.04163, 2.74748]) OR a broken screen-texture composite stretching a
half-res capture over the frame. Next: capture native's actual title perspective projection
+ the world-camera matrix and diff against retail's; and dump the intermediate screen-texture
/ EFB-copy contents to see what the composite samples. `SB_SKIP_ORTHO` HANGS the boot (fade
screens are ortho — a load loop waits on a fade that never renders), so isolate 3D-vs-2D some
other way. Do NOT diff against `title_gx_oracle.png`; use `frames/check_3800.png`.
