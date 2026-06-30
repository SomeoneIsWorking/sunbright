# File-select overbright ROOT CAUSE = multi-EFB-target render structure, not a blend knob (2026-06-30)

## TL;DR (proven by the GX command-stream oracle, not pixels)
The file-select overbright (native settled mean RGB +[64,41,23] over the Dolphin-GX oracle,
mean|delta| 42.7) is **not** a blend-equation / draw-order / coverage bug among the sky layers.
It is a **render-target STRUCTURE mismatch**: Dolphin renders the file-select frame across THREE
EFB targets with an EFB-clear between passes; the native renderer (sms-boot/nvk under SB_OWN_GXLIST)
flattens all of it into ONE never-cleared framebuffer.

The prior handoff's framing ("compare nvk vs Dolphin compositing of the SCREEN+additive sky layers;
blend equation / draw order / coverage") was the WRONG unit — those layers' colours and blend
factors are correct. The real divergence is one level up: the EFB copy/clear structure.

## How it was found — DIRECT element comparison via the oracle (not ablation knobs)
New tooling records the EFB-copy SEQUENCE from the Dolphin GX command stream (the sanctioned
oracle, `runtime/gx_parse.h` / `gx_capture.cpp`), each `BPMEM_TRIGGER_EFB_COPY` decoded for
copy-to-XFB vs copy-to-TEXTURE and the clear bit (`UPE_Copy`), tagged with the cumulative prim
count before it. `SUNBRIGHT_DBG_GXCOPY=1` prints it. The settled file-select frame:

```
[gxcopy] frame 4: prims=1170 dls=188 copies=3  seq:
   [@60     prims<=0    ->XFB CLR]    (prev frame's display copy / clear)
   [@11005  prims<=653  ->TEX CLR]    PRE-PASS: 653 prims → copy EFB to a TEXTURE, then CLEAR EFB
   [@108129 prims<=1100 ->TEX]        MAIN: draw to 1100 prims → copy EFB to a TEXTURE (no clear)
   (… rest → final XFB display copy, truncated at the boundary)
```

Native (sms-boot) does **zero** intra-frame copies/clears — it captures all ~1170 prims' J3DShape
draws into one batch list and composites them in a single `renderTevFrame` over one cleared target.

So the GC structure is:
1. **Pre-pass (unk40, ~653 prims)** → render scene to EFB → **copy EFB→TEXTURE_A** → **CLEAR EFB**.
2. **Main pass (mPerformListGX, 653→1100)** → render scene to the now-clean EFB → copy EFB→TEXTURE_B.
3. **Post (mPerformListGXPost)** → 2D/HUD + **full-screen quads that SAMPLE TEXTURE_A/B** (EFB-copy
   textures) to do the soft-focus/bloom composite, then the display copy.

## Why native is overbright (the two flattening consequences)
- **Double-draw:** native never honors the EFB CLEAR after the pre-pass, so the pre-pass scene
  (sky/sun/terrain, ~653 prims) stays composited UNDER the main pass — the whole scene is drawn
  ~twice. (Confirmed independently: per-batch attribution shows the sky dome b3==b52, sun rays
  b0==b49 etc., byte-identical bbox/verts, captured in BOTH phase 1=unk40 and phase 4=mPerformListGX.)
- **EFB-sampler quads sample WHITE:** the post-pass full-screen composite quads bind an EFB-copy
  texture that native cannot provide, so they sample the 1×1 default-white texmap. Identified
  structurally by their EFB/XFB-sized textures: b27/b45 sample a **320×224** texture (= XFB res),
  b12/b76 a **256×256** with **bm=1/4/2** (SRC_ALPHA / SRC_COLOR ≈ multiply-add, rgb 0.87) — a
  near-doubling brightness pass applied full-screen. That flat-white multiply is the dominant
  additive wash.

## Tooling built this session (the drilling is automated)
- **`NvkTevBatch::phase`** (gx_geom.h) + `sb_boot_capture_set_phase` (sms_boot_j3d_capture.cpp),
  stamped per perform-list in `TMarDirector::direct` (1=unk40, 2=unk38, 3=unk3C, 4=mPerformListGX,
  5=Silhouette, 6=mPerformListGXPost). `SB_BATCH_DBG` prints `phN` → attribute any batch to its pass.
- **`SB_ABLATE_PHASE=N[,N]`** (sms_boot_present.cpp) — drop a phase from the present composite.
- **Oracle EFB-copy sequence** (`GxFrameInfo::efb_copies`, gx_parse.cpp `OnBP`) +
  `SUNBRIGHT_DBG_GXCOPY` (gx_capture.cpp) — the render-target structure of the Dolphin oracle.
- Phase-ablation pixel sweep (cross-check only, NOT the method): baseline 42.7; keep ph1+ph4 (drop
  ph6 EFB-sampler wash) 13.6; keep only ph4 16.3; drop ph1 (lose pre-pass) 45.7. These corroborate
  the structural finding (ph6 EFB-sampler quads are the dominant wash) but the *diagnosis* came from
  the oracle copy structure, not the deltas.

## The FIX (the next task — own the EFB-copy-texture path)
The faithful fix is to honor the GC render-target structure in the native renderer:
1. Tap the EFB copies in sms-boot's GX seam (`GXCopyTex`/`GXSetCopyClear` + the clearing copy).
   Record each copy's destination texture address and whether it clears.
2. At a clearing EFB→TEXTURE copy: snapshot the scene-so-far into a host texture keyed by the
   destination address, then RESET the present composite (honor the clear) so the pre-pass stops
   double-compositing.
3. When a later batch binds a texmap whose address matches a recorded EFB-copy destination, sample
   the snapshot instead of the default-white texmap — so the post-pass bloom/soft-focus composites
   over the REAL scene, not white.
This requires the present to render in SEGMENTS at EFB-copy boundaries instead of one final
renderTevFrame. Interim, EFB-sampler batches (texmap == EFB-copy dest) should be LOUDLY skipped
rather than drawn flat-white (consistent with the CLAUDE.md EFB-readback-effect gap), but the real
fix is the segmented render + readback above.

## Verify
`SUNBRIGHT_DBG_GXCOPY=1` on build/sunbright (oracle) re-confirms the 3-copy structure.
A real fix drops `tools/render/fileselect_overbright.py` mean|delta| from 42.7 toward the
ph1+ph4-only floor (~13.6) WITHOUT ablating layers, with the EFB-sampler quads sampling real scene.
