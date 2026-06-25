# 2026-06-25 — Native engine: white letterbox bars → black (imm 2D honors real GX TEV)

## Symptom
Delfino-Plaza gameplay (default fastboot) rendered the **top ~8% and bottom ~8% of the
screen pure white** (the cinematic entry-cutscene letterbox bars). Measured: top/bottom 8%
bands = 100% pixels >250. The dominant foreground defect after the drawHead-self-loop unblock.

## Decomposition (value-first, never eyeballed)
`SB_BATCH_DBG` showed the lower-mid wash is the plaza **pavement** (key `224004d9`, lit to
white at grazing minification — a SEPARATE, subtler issue, NOT fixed here). But the pure-white
top/bottom bands SURVIVED `SB_SKIP_KEY=224004d9` (scene skip), so they are NOT scene geometry.

New `SB_IMM_DBG` (per-immediate-batch screen coverage/colour/tex) localized them to one
immediate batch: two full-width quads at `y[0.759,1.071]` (top) and `y[-1.071,-0.759]`
(bottom), an **8x8 texture, vertex colour white, drawn on top no-depth** = the bars.

New `SB_IMM_TRACE` (backtrace the native GXBegin caller) named the source:
`J2DPicture::drawTexCoord` ← `J2DPane::draw` ← `J2DScreen::draw` — a 2D UI letterbox picture.

## Root cause
`J2DPicture::setTevMode` (reference/sms) builds a multi-stage combiner:
 - stage0: out = TEXC (intensity texture)
 - ramp stage: out = lerp(C0=`mBlack`, C1=`mWhite`, CPREV) — the intensity→colour ramp
 - corner stage: modulate by raster (corner colour)
Captured registers for the bars: `C1(mWhite)=0,0,0,255`, `C0(mBlack)=0,0,0,0`, texture
intensity = `0xff` (1.0) ⇒ ramp output = C1 = **black**. The bars are meant to be black.

Our immediate-mode 2D path did NOT run the real combiner — it hardcoded `out = texture ×
vertexColour` (the `g_tex_frag` modulate approximation). For the bars that gives
`white_tex × white_vtx = WHITE`, ignoring the C0/C1 ramp entirely.

## Fix (own the path — not a bandaid)
Make the immediate-mode 2D path honor the **full captured GX TEV combiner**, exactly like the
J3D scene path already does:
 - `gx_imm_impl.cpp`: `snapshot_tev()` builds an `NgxTevState` from `state().tev` at each
   GXBegin (the GXState TEV block is already stored in the NgxTevStage bit layout — near-direct
   copy; the imm counterpart of `sb_build_tev_state`). Stored per-prim in a per-frame
   `vector<unique_ptr<NgxTevState>>` (stable pointers; cleared at consume) and pointed-to by
   `SbImmBatch::tev`.
 - `sms_boot_present.cpp`: for textured imm batches, generate the real combiner fragment via
   `sb_tev_gen_fragment(*ib.tev)` (cached by fragment hash) and set `push.tevreg`/`push.kcolor`
   from the captured S10 TEV colour + konst registers — instead of the modulate hardcode.
   `SB_IMM_MODULATE=1` forces the old path for A/B.
This subsumes both the passthrough and modulate hacks and is strictly more faithful — it also
correctly drives every other J2DPicture intensity-ramp draw (banner/HUD digits/marks).

### GOTCHA (fixed) — static-init crash
First cut stored the per-frame TEV states in a global `std::deque<NgxTevState>`. `std::deque`'s
DEFAULT CONSTRUCTOR allocates its internal map — which ran at **static-init time, before the
game OS/JKR heap is up** → `operator new` → `OSLockMutex` → the native-thread hash map with 0
buckets → SIGFPE (modulo-by-zero). `bt` pointed at `_GLOBAL__sub_I_sb_gx_imm_begin`. Replaced
with `vector<unique_ptr<>>` (default ctor allocates nothing; pointers stay stable on push_back).
Lesson: no container whose ctor allocates may be a global in a TU linked into the game image.

## Verification
- Top/bottom 8% bands: 100% >250 (white) → 100% <12 (BLACK). `scratch/frames/fix_best.png`
  shows correct black cinematic framing over the rendered plaza.
- 2 clean 40s gameplay runs, exit 137 (no crash), `SB_DRAWBUF_CHECK=1` 0 cycle trips.
- No 2D regression: reachable file-select loading banner frames are BITWISE identical
  old(`SB_IMM_MODULATE=1`)-vs-new (meanAbsDelta 0.00). Settled `SB_FILESELECT` state draws
  only the gradient (scene_verts=0, textured imm=0) — the menu scene isn't loaded in that path
  (pre-existing gap), so nothing textured to regress.
- All 13 native render/platform unit tests pass.

## NOT fixed (next)
The lower-mid pavement (key `224004d9`) blows toward white at grazing foreground angle (lit
white matColor × pale tile texture). Separate issue; trilinear mips already on, so likely
lighting/exposure or texture-content. Left for next pass.

## New diagnostics (all gated, off by default)
`SB_IMM_DBG=N` (per-imm-batch coverage/colour/tev), `SB_IMM_TRACE=1` (one-shot backtrace +
TEV-register + texture dump for the bar-shaped quad), `SB_IMM_MODULATE=1` (force old modulate).
