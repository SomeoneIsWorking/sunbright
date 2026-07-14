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
- **VI, not the copy, fills the TV picture**: VI stretches xfbHeight copied lines
  across viHeight physical scanlines (480) unconditionally — the XFB's own aspect
  never reaches the screen. That was aurora's bug: `calculate_present_viewport`
  letterbox-fit the copy's own 640×448 (10:7) instead of the render mode's
  viWidth×viHeight (640×480). Now `g_sbViWidth/Height` (latched at VIConfigure,
  default 640×480) feeds the viewport aspect.
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

## Remaining replay-vs-oracle deltas (title_settled)

1. **Seagull sprites missing** (two left-side birds absent; the O-ring bird renders).
   Prior localization: not in post-merge draws 200-584 (mirror/lens-flare/2D passes);
   candidates now inspectable in draws 0-199.
2. Thin edge outlines on clouds/letters in the diff map — consistent with the
   unported copy-time 7-tap vertical deflicker (softens 1px-tall detail in Dolphin)
   and resampler differences. Cosmetic-scale; port the vfilter in the EFB resolve
   pass (NOT the present stretch — two independent HW stages) for pixel-exactness.
