# 2026-07-14 — Blocky fly-in letter backgrounds: decomp's phantom mBlack constant

User-visible defect (windowed `./run.sh`): during the title fly-in every SUPER MARIO
SUNSHINE letter drew inside a translucent dark-blue RECTANGLE; blocks gone by settle.
FIXED — reference/sms `src/GC2D/CardLoad.cpp` overlay-pane init `mBlack = 0x01006667`
→ `0x00FFFF00`. A/B at present 320: `scratch/shots/blocky_fix_ab.png` (blocks gone,
white cloud letters + cyan glow matching retail's fly-in).

## Chain of evidence

1. **Attribution.** With `SB_SKIP_GHOST=1` the blocks REMAIN (falsifies the earlier
   "extra ortho scene dispatches are the blocks" note — see correction below). The
   frame's remaining ortho block (post-merge draws #1781–#1812, stale marker
   `'DrawBuf ChrXlu'`) is 32 screen-space GX_QUADS with identity posmtx, screen-pixel
   translations, and 40x46…99x106 glyph textures — the `p_0X` fly-in overlay letter
   panes. Block edges in the screenshot sit exactly at these quads' rects.
2. **State diff vs retail.** The FIFO replay (title_settled.dff) draws the SAME quads
   box-free with an IDENTICAL 3-stage TEV: stage0 out=TEX, stage1 out=lerp(C0,C1,TEXC)
   / lerp(A0,A1,TEXA), stage2 out=prev×RAS (chan COLOR0A0). Only DATA differed:
   - TEVREG0 (= J2DPicture mBlack): native (0.004,0,0.4,**0.404**) = 0x01006667;
     retail (0,1,1,**0.000**) = 0x00FFFF00.
   - chan-0 ambient alpha: native 0.71 (=180/255, the pane fade mid-flight — LEGIT);
     retail 0.00 (its capture is at a faded-out phase).
   Background texel ⇒ texA=0 ⇒ final alpha = mBlack.a × paneAlpha. Retail mBlack.a=0
   kills the quad background at EVERY fade phase (alpha test GREATER 0); native's
   0.404 × fade = the dark-blue block.
3. **Retail ground truth.** Full-boot Dolphin framedump (recipe below): the fly-in
   letters condense as WHITE clouds; no dark boxes at any frame.
4. **US disasm** (scratch/bin/sms.dol @ 0x8016e980, pane-creation loop in
   TCardLoad::load):
   ```
   lis  r3, 0x100
   addi r28, r4, 0x6667    ; r28 = 0x66666667 — the /10 magic for p_10+ pane keys
   addi r21, r3, -0x100    ; r21 = 0x00FFFF00
   ...
   stw  r3(=r21), 0x140(r4) ; pane->mBlack = 0x00FFFF00
   ```
   `0x01006667` appears NOWHERE in the US DOL (byte + immediate-builder scans). The
   decomp fused the `lis 0x100` with the `0x6667` tail of the divide-by-10 magic into
   a phantom color constant. The case-3 whiten ramp `(r<<24)+0xFFFF00` (verified
   identical in US disasm @ 0x8016c350) starts from r=0 — consistent ONLY with the
   0x00FFFF00 init (and with the captured settled TEVREG0 = (0,255,255,0)).

This is decomp-transcription skew, not region skew: the same fused-immediates shape
exists in the JP function, so the fix is unconditional (no `#ifdef`).

## Correction to 2026-07-14_xfb_present_arc.md

Its "blocky letters ROOT CAUSE LOCALIZED — extra ortho scene dispatches" section is
FALSIFIED as the cause of the visible blocks (SB_SKIP_GHOST left them intact; the
mBlack data was the cause). The ghost pass (unk40 head dispatch under stale ortho,
no retail counterpart) REMAINS a real, separate structural wart — still to be RE'd
and eliminated, after which SB_SKIP_GHOST gets deleted.

## New durable tooling

- `tools/re/dol_extract.c` — extracts main.dol from any disc image (ISO/RVZ/…) via
  the nod prebuilt already fetched by the aurora build (build cmd in its header).
  Regenerates `scratch/bin/sms.dol` after a scratch wipe (this session needed it;
  the previous copy died in the wipe).
- Full-boot pixel oracle: `DISPLAY=:99 dolphin-emu-x11 -b -u scratch/oracle/boot_user
  -v OGL -e <rom> -C Dolphin.Movie.DumpFrames=True -C Dolphin.Core.EmulationSpeed=0
  -C Dolphin.Interface.UsePanicHandlers=False` (panic handlers OFF is required —
  first attempt hung 0% CPU on a hidden dialog). Landmine: Dolphin PRUNES old
  framedump PNGs while running (first ~2000 frames were deleted mid-capture); pull
  the frames you need promptly. Boot timeline on this capture: intro movie ends
  ≈ frame 800; title fly-in ≈ 3160–3360 (attract loops movie↔title).

## Verification

- Native present-320 dump (was the maximal-block frame): blocks gone, letters render
  as white clouds w/ cyan glow — matches retail fly-in appearance
  (`scratch/shots/blocky_fix_ab.png` before/after, `scratch/shots/boot_flyin_dense.png`
  retail reference).
- No SB_SKIP_GHOST needed for the clean result (run used defaults).
