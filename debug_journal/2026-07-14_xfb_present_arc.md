# XFB / display-copy present arc (2026-07-14)

The replay-vs-oracle "framing offset + bottom letterbox" residual is FIXED. Replay of
`title_settled.dff` vs Dolphin's playback of the same file: RMSE 0.2518 → **0.0681**,
framing pixel-aligned, deterministic (byte-identical reruns). aurora `5853fe5`,
sunbright fifo_player synthesis in the same-day commit.

## Ground truth (from reference/sms SDK source + decoded capture regs)

Full spec was researched into `scratch/xfb_spec.md` (gitignored scratch; durable facts
below). Decoded from the title capture's display copy (BP 0x52 with copy_to_xfb=1):
src 640×448@(0,0), stride 1280 B (=640 px YUYV), yscale 0x100 (unity → 448 XFB lines),
**gamma bits = 00 = GX_GM_1_0 (linear)**, vfilter = stock `[8,8,10,12,10,8,8]`/64
(GXNtsc480IntDf deflicker).

- `GXSetDispCopyDst`'s `ht` arg is dead on real HW; XFB line count =
  `GXSetDispCopyYScale` return (`GXGetNumXfbLines(srcH, yscale)`).
- **VI, not the copy, fills the TV picture**: VI stretches the XFB across the TV's
  fixed 4:3 picture — the XFB's own aspect never reaches the screen. That was
  aurora's bug: `calculate_present_viewport` letterbox-fit the copy's own 640×448
  (10:7). The present aspect is now the constant 4:3 (640,480) at the end_frame
  call site. NOTE a first design (latching the render mode's raw viWidth/viHeight
  at VIConfigure) was WRONG and was reverted same-day: SMS programs viWidth=660,
  viHeight=448 — overscan-domain scan-out values that a real TV and the Dolphin
  oracle both still show as the full 4:3 picture. Raw VI fields must not drive
  the viewport.
- The real XFB is YUYV — **no alpha channel**. aurora's RGBA8 display-copy texture
  now forces the opaque-alpha swizzle. (SMS's EFB alpha at title averages ~0.2 from
  dst-alpha stamps; leaking it washes any alpha-aware consumer.)

## Brightness excess: display-copy stage RULED OUT

The known native-title ~15-25-level whole-frame brightness excess is NOT the display
copy: gamma is linear (00) on every captured frame, and the 7-tap vfilter is
unity-normalized (redistributes between rows, adds nothing). Diagnose upstream
(TEV/lighting/blend). Do not re-open `GXSetDispCopyGamma` for this.

## What changed where

- aurora `command_processor.cpp` BP-0x52: copy_to_xfb=1 → `copy_tex(kDisplayCopyDest)`
  (was `Log.warn` stub). Native GXCopyDisp (dest-key route) and in-stream triggers
  (FIFO replay) now converge in copy_tex.
- fifo_player: synthesizes LOAD_COPY_SRC/DST for the display copy too (dstW from the
  stride reg ×32/2, dstH via the GXGetNumXfbLines formula, fmt=RGBA8 like native
  GXSetDispCopyDst); tracks BP 0x4D/0x4E; seeds all copy trackers from the .dff BP
  snapshot. Degenerate stride/yscale at a display trigger → abort (fail-fast).
- `SB_DUMP_FRAME` now dumps the present source = the display-copy texture (not the
  raw EFB) once a display copy has run, at internal render scale (e.g. 1280×896 for
  a 2× target). Apply the VI stretch (resize to 640×480) before comparing to Dolphin
  framedumps.
- Replay pumps `2 + frames` trailing presents so every queued dump readback resolves.

## Falsified / tooling landmines (do not re-chase)

- **"White-hazed replay frames" were a PNG-alpha viewing artifact**, not a render
  defect: the dump carries the texture's alpha; converting with alpha and viewing
  composites the scene against white (mean α≈0.2 → washed). Byte-identical dumps
  looked "good" or "hazy" purely by conversion path. Convert diagnostics with
  `-alpha off`. (Now moot for the display copy — alpha forced opaque — but applies
  to any alpha-carrying dump.)
- The old SB_DUMP_FRAME machinery raced under `SB_DUMP_FRAME_EVERY` (single static
  buffer/path reused across overlapping async maps → "map failed status=4"). Rewritten
  per-job in aurora; EVERY=1 = one dump per present, `.<seq>` suffix in queue order.
- `SB_DRAW_DUMP` had a hardcoded floor of 200 (draws 0-199 uninspectable — blocked
  the seagull localization). Removed; `SB_DRAW_DUMP=0` dumps from the first draw.

## GXGetNumXfbLines/GXGetYScaleFactor were silent 0-stubs (fixed + tested)

The VIConfigure fail-fast (transient, during the reverted latch design) surfaced a
banned-class bug: `sms-boot/runtime/sdk_stubs.cpp` had `GXGetNumXfbLines` return 0 and
`GXGetYScaleFactor` return 1.0f behind a comment claiming "no consumer on the Aurora
path" — falsified: `JDrama::CalcRenderModeXFBHeight` computes every render mode's
xfbHeight AND viHeight through them, so every mode carried xfbHeight=0/viHeight=0.
Both are now faithful SDK ports (from `reference/sms/src/dolphin/gx/GXFrameBuf.c`) in
aurora `GXFrameBuffer.cpp`, with a spec unit test (`platform-gx_yscale_test`: anchor
values incl. the captured title copy's unity yscale/448 lines + a sweep against a
verbatim SDK transcription). Domain gotcha: the y-scale register is 9 bits, so
yScale ≤ 0.5 wraps `(u32)(256/yScale) & 0x1FF` to 0 → divide-by-zero (PPC divwu is
silent, x86 traps); valid domain is yScale strictly within (0.5, 2.0].

## run.sh default = the working game

Plain `./run.sh` used to take the game's built-in fastboot default (Delfino Plaza,
stage 1) which OSPanics at the unported `TMapObjTree::initMapObj`. run.sh now opts
out of fastboot (SB_NO_FASTBOOT=1) when no explicit SB_STAGE/SB_SCENARIO/
SB_NO_FASTBOOT is given → vanilla GC-logo → title/attract boot, windowed. Verified:
headless run via run.sh defaults reaches the settled title, full-color logo,
correct 4:3 framing through the new display-copy present path.

## Remaining replay-vs-oracle deltas (title_settled)

1. **Seagull sprites missing** (two left-side birds absent; the O-ring bird renders).
   Prior localization: not in post-merge draws 200-584 (mirror/lens-flare/2D passes);
   candidates now inspectable in draws 0-199.
2. Thin edge outlines on clouds/letters in the diff map — consistent with the
   unported copy-time 7-tap vertical deflicker (softens 1px-tall detail in Dolphin)
   and resampler differences. Cosmetic-scale; port the vfilter in the EFB resolve
   pass (NOT the present stretch — two independent HW stages) for pixel-exactness.

## Seagull localization progress (2026-07-14, SB_NDC_DRAW)

New aurora tooling `SB_NDC_DRAW=<lo>[:<hi>]` (aurora d643dfa) probes any
[draw-dump]-indexed draw per-vertex. Findings on title_settled.dff draws 132-143
(the four bird instances, two invisible):
- All four project on-screen at the correct locations (NDC matches oracle bird
  pixels), correct depth (inZ), zero behind-camera vertices, per-vertex skinning
  matrix indices 0-8 all load correctly. **Geometry/matrix hypotheses FALSIFIED.**
- `SB_NO_DEPTH=1` (depth compare Always, no z-writes) changes ZERO pixels in the
  whole replayed frame (byte-level compare) — depth-kill falsified too, with the
  caveat that the instrument wasn't validated against a known depth-culled case
  in this scene (the title sky scene may genuinely have no depth overlap).
- Remaining prime suspect: the TEXTURE layer. All bird bodies share state
  (aComp GEQUAL 128 cutout) but bind different per-bird 64x64 texture addresses
  (animation frames); replay frames 1-2 carry exactly ONE TextureMap memupdate
  per frame (the flapping-bird atlas is the only animating texture). Next probe:
  log the bird texmaps' bind addresses + versions around draws 132-143 and diff
  the decoded texels of visible vs invisible birds' textures.

## NEW user-visible defect: blocky letter backgrounds during title fly-in

User report (2026-07-14, windowed ./run.sh): "earlier frames, the text has
blocky background". Reproduced headless: at ~present 320 (SB_DUMP_FRAME_EVERY=20
boot sequence, scratch/shots/bs_montage2.png tile 2) every SUPER MARIO SUNSHINE
letter is drawn with a translucent dark-blue RECTANGLE around it; by settle
(~present 440+) the blocks are gone and the logo is correct.
KEY FACT: the FIFO replay of title_settled.dff — retail's GX stream at the SAME
mid-fly-in phase — renders these letters with NO blocks through the same aurora.
So aurora rasterizes the retail stream correctly; the native game emits WRONG
GX state for the letter panes during fade-in (suspect: J2D/material-anim alpha
source — texture alpha not selected into the TEV alpha chain, or blend/acmp set
differently than retail during the alpha-fade phase). Next: SB_DRAW_DUMP the
native blocky frame's 2D letter draws and diff field-by-field against the
replay's #147-194 letter draws (which render correctly).

## Blocky fly-in letters — section FALSIFIED (kept for the record)

**FALSIFIED as the cause of the visible blocks** (same day): SB_SKIP_GHOST=1 left the
blocks intact, and the real cause was the decomp's phantom overlay-pane mBlack constant
(0x01006667 → 0x00FFFF00) — see `2026-07-14_blocky_letters_mblack.md`. The extra ortho
ghost dispatch described below IS still real as a structural/perf wart (retail has no
counterpart) and still needs RE + removal, but it did not paint the user-visible blocks.

Falsifiers run at native present 320 (the maximally blocky frame, retrace≈638,
`SB_TRACE_SEQ` maps presents↔retrace at 2/present):
- `SB_SKIP_GHOST=1` does NOT remove the blocks (scratch/shots/ghost_ab.png) — the
  currently-skipped phase-1 ghost is not (or not the only) source.
- Field-diff of the SAME letter draw (identical trans/sub-prim structure/textures,
  e.g. trans=(305.4,…)):
  - retail (title_settled.dff replay, renders box-free): drawn ONCE, `proj=P
    prj=[2.0416 2.7475 ...]`, amb=(0.5,0.5,0.5), aU=0, vp=(2,2 640x448).
  - native blocky frame: drawn THREE times — #2709 `proj=O prj=[0.0045 -0.0031
    -0.5 -0.5]` (stale 2D ortho), #2907 `proj=P prj=[2.0416 ...]` (the correct
    draw), #3000 `proj=O` AGAIN. An ortho-projected 3D cloud-letter mesh IS a
    big translucent screen rectangle — the blocks.
  - Frame totals: native 498 proj=O vs 402 proj=P post-merge draws; retail's
    frame has only the small 2D tail in ortho. The native frame loop dispatches
    the 3D DrawBufs multiple times under a stale ortho projection.
- At settle the ghost output happens to be invisible (hence the old "perf wart,
  bit-identical with SB_SKIP_GHOST=1" classification — TRUE at settle only).
  During fly-in it is THE user-visible "blocky text background" defect.

NEXT (proper fix, no bandaid): RE why the native JDrama draw-phase dispatch runs
the scene DrawBufs under the 2D/ortho phase (twice!) when retail does not —
likely the perform-list draw-phase routing (TDrawBufObj dispatch per phase) or a
missing phase gate in the ported director loop; SB_SKIP_GHOST's heuristic covers
only one of the dispatch sites. Fix = eliminate the extra dispatches at the
source, then delete SB_SKIP_GHOST entirely.
