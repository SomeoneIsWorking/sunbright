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

## THE CONSUMER — found (RE, 2026-06-30)
`TEfbCtrlTex::perform` (reference/sms/src/JSystem/JDrama/JDREfbCtrl.cpp:80-99) is the EFB-copy
PRODUCER: `GXSetTexCopySrc/Dst` + `GXCopyTex(mImagePtr, doClear)`. `MarDirectorInitECT.cpp` creates
the file-select EFB textures: the sea **MIRROR** `"鏡描画ステージ"` (samples via `mirrorCam->unk60`,
a **GXTexObj** — `setTexAttb`, NOT a ResTIMG) and the pollution **"graffito"** textures
(`mImagePtr = &img + img->imageDataOffset`, a ResTIMG image). The CONSUMER binds that texture and
draws a full-screen composite quad — `b76` (256×256, bm=1/4/2 multiply, rgb 0.87) is the prime
suspect. Native makes `GXCopyTex` a no-op, so `mImagePtr` holds STALE guest RAM; the consumer decodes
that as its texture → the flat ~0.87 multiply wash. (The 0 J3D/imm address matches earlier are because
the mirror binds via GXTexObj, a path the capture's ResTIMG-based texsrc match doesn't cover.)

## Verify
`SUNBRIGHT_DBG_GXCOPY=1` on build/sunbright (oracle) re-confirms the 3-copy structure.
A real fix drops `tools/render/fileselect_overbright.py` mean|delta| from 42.7 toward the
ph1+ph4-only floor (~13.6) WITHOUT ablating layers, with the EFB-sampler quads sampling real scene.

## UPDATE 2026-06-30 (later) — segmented present BUILT; the EFB copy is NOT the dominant overbright
The segmented snapshot+resample present is implemented (native/render: `draw_tev_segment` +
`snapshot_efb` in gx_sdlgpu; `NvkTevBatch::Tex::efb_src`; per-frame snapshot registry; the present
splits the scene at the EFB-copy boundaries and binds the live snapshot to the EFB-sampler consumers).
Findings from running it at the SETTLED file-select (the tooling did the drilling — segment dumps,
not pixel knobs):

- **The 2 native EFB copies are: [0,49) clear → 256×256 dest; [49,79) no-clear → 320×224 dest.**
  Dumping each segment (`SB_SEG_DUMP=1`) shows segment **A0 [0,49) IS the full visible scene** (palm
  tree, island, OPTIONS sign, Mario) and **A1 [49,79) is a SECOND scene render** (no palm tree, a
  different camera/pass). So the "pre-pass" segment is NOT off-screen content — it holds the palm tree.
- **Honoring the EFB clear REGRESSES (42.7→45.7) because it drops segment A0's palm tree** — native
  captured it only before the boundary; the GC main pass that redraws it isn't faithfully split in
  our capture. So honor-clear is now **opt-in** (`SB_EFB_HONOR_CLEAR_SEG=1`); default = CUMULATIVE
  compositing (LOAD every segment, same coverage as one pass) + snapshots at each boundary.
- **The ONE matched EFB-sampler is the soft-focus imm quad** (320×224, blend 1/4/5, image==320×224
  dest). Binding it to the live scene snapshot instead of stale RAM is correct but **net-neutral
  (42.7)** at settle — it is NOT the dominant wash. The 256×256 MIRROR has NO native consumer (checked
  both the ResTIMG `src` AND the live `GXLoadTexObj` bound image via the new `sb_gx_bound_tex_image` —
  no match; the sea reflection samples it via a path the capture doesn't reach, a SEPARATE small gap).
- **THE DOMINANT OVERBRIGHT IS THE SKY/POST MULTI-LAYER BLEND, NOT THE EFB COPY.** The top-half white
  "checkerboard" is the sky-dome detail-texture layers (the 8×8-texture additive batches b0/b1/b2 and
  their phase-4 dupes b49/b50/b51 + the post-pass 256×256 quads b75/b76, blend 1/4/2/1/4/5). This is
  exactly the **"ti=10 additive / ti=9 premult white cloud layers … multi-layer-blend NO-ORACLE trap"**
  CLAUDE.md already flags as DO-NOT-EYEBALL. Phase-6 ablation drops 42.7→13.6 = the post pass owns the
  wash. The fix path is a per-blend-layer comparison vs the **GX command-stream** value oracle
  (blend equation / draw order / coverage per layer), NOT pixel ablation against the (imperfect) PNG.
- **The PNG oracle `fileselect_gx_oracle.png` is itself imperfect** (user, 2026-06-30): Mario's shadow
  is a white quad and the hidden FLUDD-Mario is visible. Treat its mean-delta as coarse, not gospel.

**Net of this session:** the EFB segmentation is FAITHFUL infrastructure (kept, default-on, net-neutral,
no regression) + reusable tooling (`SB_SEG_DUMP`, `SB_SKIP_BIDX`, `sb_gx_bound_tex_image`,
`sb_boot_capture_efb_copies`). The overbright's true owner is the sky/post multi-layer blend — the
next target, to be driven by command-stream per-layer comparison, not pixels.

## UPDATE 2026-06-30 (later, commit 3fb6917) — VALUE oracle built; overbright = ONE batch b76 (composite/occlusion)
Built the command-stream per-draw BLEND/TEV value oracle (the handoff's next task), driven by VALUE:
- `gx_parse.{h,cpp}` records per-draw GX pixel state (blend src/dst/subtract/enable/logicop, color/alpha
  update, numtevstages, EFB pass, projType) → `GxFrameInfo::draws`; `SUNBRIGHT_DBG_GXBLEND` emits it via
  the pure `gxblend_summary.h` (run-length-deduped, factor-named). `render_test` `gxblend` unit (TDD, 19/19).
- Native side: `SB_BATCH_DBG` now also prints per-batch TEV konst/tevreg + color/alpha-update + bfrag glsl.

**The overbright is ONE batch.** Ablation (localize-only; the DIAGNOSIS is the value oracle): drop the
post pass (ph6) 42.7→13.6; drop just **b76** 42.7→**14.2**; drop the HUD 42.7→42.9 (irrelevant). So b76
alone owns the wash.

**b76 identity + why it washes (by value):** full-screen quad, `bm=1/4/2` (SRCALPHA/SRCCLR), 3-stage TEV,
256×256 84×-tiled tex, drawn in BOTH ph1 (as b12) and ph6. Its generated frag (scratch/frames/bfrag_76.glsl)
saturates `src` to WHITE: stage-1 `prev.rgb = (rastemp=vColor 0.87→222) << 1 = 444 → clamp 255`. Then
SRCALPHA/SRCCLR over the scene = white wash.

**The oracle's matching draw writes NOTHING.** `SUNBRIGHT_DBG_GXBLEND` frame 6: the **pass2** (main scene,
between the 2 EFB copies) 3-stage SRCALPHA/SRCCLR is **`[noC][noA]`** = GXSetColorUpdate/AlphaUpdate FALSE,
11 verts, persp. So on GC this is an invisible occlusion/depth-only pass (sun-occlusion / lens-glow class).
Native draws it VISIBLE-WHITE → the overbright. b76 also has absurd NDC (`ndcX[-106119,124861]`, 165× off
screen) = mis-projected (a small/ortho pass blown full-screen), and lands in the WRONG passes (ph1+ph6, not
the oracle's pass2).

**Fix landed (faithful infra, no regression @42.7):** J3D batches now capture the LIVE GXSetColorUpdate/
AlphaUpdate global state (J3DPEBlock has no such field) instead of hardcoding 1 — 30 batches now correctly
write no alpha (`sb_gx_get_color_alpha_update`, gx_impl.cpp). This did NOT fix b76: GXSetColorUpdate(FALSE)
is called 250k× but is NOT active at b76's capture moment (`SB_DBG_COLUPD` trace). So native's perform-list
execution doesn't wrap b76's draw in colorUpdate(FALSE) the way GC does.

**NEXT (the real fix, well-defined):** identify b76's effect/model (it's in the scene draw buffers, drawn
by unk40 AND post — likely the sun-occlusion / lens-glow / specular-sheen "[noC]" occlusion volume) and
make native honor its colorUpdate=FALSE (and/or its correct ortho projection). Either (a) find the
perform-list entry / TViewObj (initECDisp lensGlow/specularSheen/composite3, or the scene occlusion model)
that issues GXSetColorUpdate(FALSE) and ensure it's live at the J3D capture tap, or (b) extend the oracle to
dump the per-stage TEV combiner so the white-saturation can be confirmed faithful vs a generation bug.
Verify with `tools/render/fileselect_overbright.py` (42.7 now) — target the ~14 floor WITHOUT ablating b76.

## UPDATE 2026-06-30 (latest, commits dd36378/e90ed49) — b76 IDENTIFIED = sea water (MapXlu); corrects [noC] guess
Built draw-buffer-name attribution (TDrawBufObj::perform stamps getName() → batch dbgName, SB_BATCH_DBG
`[batchbuf]`). **b76 (and b12, b75) are in "DrawBuf MapXlu"** = the translucent MAP buffer = the SEA WATER
surface. So b76 is NOT an occlusion/[noC] pass (that earlier match was wrong) — it is the sea-water
reflection composite (SRCALPHA/SRCCLR) that native renders opaque-WHITE.

**Exact white mechanism (traced through bfrag_76.glsl):** the src colour AND src alpha both saturate from
the VERTEX colour, not the texture:
- src.rgb: stage1 `prev.rgb = (rastemp = vColor 0.87→222) << scale-2 = 444 → clamp 255` = white.
- src.a:   stage1 `prev.a = (rastemp.a = col0.a 255) << scale-2 = 510 → clamp opaque` → o.a = 1.0.
- The bound texture's alpha is LOW (btex_76 alpha mean 9.8) — so the texture is NOT what makes it opaque;
  the VERTEX colour/alpha (222/255) doubled by the stage's scale-2 is. With src=opaque white, SRCALPHA/SRCCLR
  `fb = src·srcA + dst·srcC = white + dst·white → clamp white` = the wash, unavoidable for an opaque-white src.

So the divergence is upstream of the blend: native's `src` is opaque white; the Dolphin-GX oracle's sea
water is transparent blue/teal. Candidates, to settle by VALUE:
1. **native's vColor (CLR0) for this draw is wrong** (reads 0.87/1.0 near-white; the real sea-water vertex
   colour is darker/tinted). The fileselect line has prior CLR0-decode bugs — check the J3D CLR0 read for
   MapXlu. b12=1.0 vs b76=0.87 (per-vertex varying) → it IS vertex data.
2. **native's generated TEV is wrong** — the stage-1 `<< scale-2` and/or the RASC (vColor) `d` input. EXTEND
   THE ORACLE: dump the per-stage TEV combiner (color_env/alpha_env + tevscale, BPMEM_TEV_COLOR_ENV/ALPHA_ENV)
   from the command stream for this material, and diff against native's bfrag_76.glsl. If the oracle's stage1
   is scale-1 or a different `d`, native's sb_build_tev_state / sb_tev_gen_fragment mis-generates it.
3. **the sea water should sample the 256×256 MIRROR EFB reflection** (b76's tex is 256×256 = mirror size,
   efb_src nil → native binds a stale detail tex). Binding the mirror snapshot may change the combiner inputs
   that feed `src`. This is the long-noted "256×256 mirror has no native consumer" gap — same bug.

NEXT = extend the oracle with the per-stage TEV combiner dump (#2), compare to native's frag for the MapXlu
sea-water material, and fix whichever of CLR0 / TEV-gen / mirror-binding diverges. Verify with
fileselect_overbright.py (42.7 → ~14) WITHOUT ablating b76.

## UPDATE 2026-06-30 (latest, commit 004267a) — per-stage TEV oracle REFUTES CLR0/TEV-gen/mirror; cause = native paints a [noC][noA] volume
Built the per-stage TEV combiner value oracle the prior handoff's "next task" called for
(`gxtev_summary.h` pure decoder + render_test `gxtev` unit, 20/20; `SUNBRIGHT_DBG_GXTEV` in
gx_parse/gx_capture snapshots per-stage color_env/alpha_env + TEV regs per draw). Ran it on the
settled oracle (build/sunbright, frame 6) and diffed against native's `scratch/frames/bfrag_76.glsl`.

**RESULT — all three handoff candidates REFUTED by value:**
- The oracle's settled frame has exactly TWO SRCALPHA/SRCCLR (bm=4/2) draws:
  - **draw#1096 pass2(MAIN), tev=3, `[noC][noA]`, 11 verts, persp, regs all 255** — combiner
    `s0 d=c0.rgb; s1 d=ras.rgb a=ZERO b=c1.rgb c=prev sc=x2 clamp; s2 d=prev` = **byte-for-byte
    native's bfrag_76**. GXSetColorUpdate AND AlphaUpdate FALSE ⇒ writes NOTHING on GC (a
    depth/occlusion/mask volume).
  - **draw#1100 pass3(POST), tev=2, VISIBLE, reg1≈(194,242,190) blue** — the genuinely visible
    translucent composite; a DIFFERENT 2-stage combiner.
  There is **NO visible 3-stage SRCALPHA/SRCCLR draw** anywhere — that combiner ONLY ever appears
  as the invisible `[noC][noA]` volume.
- So native's **TEV-gen is faithful** (the 3-stage combiner is reproduced register-for-register)
  and **CLR0 is faithful** (vColor≈0.87 matches the white-reg occlusion volume, whose vColor is
  irrelevant since it writes no color). The combiner doesn't sample texture color ⇒ **not a mirror
  reflection** either. CLR0 / TEV-gen / mirror are all NOT the bug.

**THE ACTUAL CAUSE (structural, by value):** native paints (cU=1) a draw GC marks no-color-no-alpha.
Native batch (SB_BATCH_DBG settled): `b76 ph6 bm=1/4/2 cU=1 aU=0 key=eb5c8e74 drawbuf="DrawBuf
MapXlu"`, and `b12 ph1` is the SAME key/geometry. So native captures the SAME MapXlu volume in
**ph1 (unk40 pre-pass) AND ph6 (mPerformListGXPost)**, both with stale cU=1 — whereas GC draws the
matching combiner ONCE in **pass2 (MAIN scene, mPerformListGX = ph4)** with cU=0,aU=0. native does
NOT capture it in ph4 at all. The captured `cU=1,aU=0` is exactly `TMario::drawLogic`'s/water-volume
RESTORE state (cU=TRUE,aU=FALSE) — native reads colorUpdate from the wrong pass.

`MarDirectorPreEntry.cpp` pushes "DrawBuf MapXlu" with flag **0x480** (frameInit|setDrawBuffer, NO
draw bit 0x8) — same "entered-but-not-drawn-here" mechanism as the sky buffers (s28 journal). The
actual MapXlu DRAW (bit 0x8) fires from a different perform-list entry. native's draw-buffer flush
runs in ph1/ph6, not the GC main pass — so the [noC][noA] volume is composited visible + the EFB
pre-pass (ph1) is composited into the visible FB instead of being an off-screen copy.

**NEXT (the real fix — structural, the same EFB/pass-routing issue this journal opened with):** make
native draw the MapXlu buffer in the correct (main) pass with the live colorUpdate GC uses, so the
[noC][noA] volume writes no color; and/or stop compositing the ph1 off-screen pre-pass into the
visible framebuffer. NOT a combiner/CLR0/mirror fix. Verify: fileselect_overbright.py 42.7→~14
without ablating b76.

## UPDATE 2026-06-30 (latest, next session) — colupd-history probe PROVES it's draw-order, not a missing FALSE
Built `sb_gx_colupd_history` (gx_impl.cpp: a monotonic GXSetColorUpdate call counter + the call index
of the last GX_FALSE), wired into the `[b76]` debug line (delta = calls since last FALSE). Ran at the
SETTLED file-select:
- **ph6 b76**: `liveCU=1 liveAU=0 colupd_calls=891554 last_false@891551 delta=3`
- **ph1 b12**: `liveCU=1 liveAU=1 colupd_calls=891566 last_false@891551 delta=15`

So `GXSetColorUpdate(GX_FALSE)` **is** called (3–15 calls before the mask draw), but a `GX_TRUE` restore
runs *between* the FALSE and native's draw → native runs the mask OUTSIDE the GC colorUpdate=FALSE
window. NOT a missing FALSE; a draw-ORDER divergence.

### The full structure, settled by VALUE (oracle SUNBRIGHT_DBG_GXBLEND/GXCOPY/GXTEV, frame 6)
The whole file-select frame is an EFB-copy-TEXTURE composite, NOT a direct scene render:
- **pass1 (pre-pass, efb_pass 1, 653 draws)** = **entirely `[noC][noA]`** (depth/occlusion only) → copy
  EFB→TEX + CLEAR. The 3-stage SRCALPHA/SRCCLR mask is NOT here.
- **pass2 (main, efb_pass 2, ~531 draws)** = **mostly `[noC][noA]`** too; the 3-stage SRCALPHA/SRCCLR
  mask (`draw#1180`, tev=3, [noC][noA], 11 verts) appears ONLY here. Copy EFB→TEX.
- **pass3 (post, efb_pass 3)** = the genuinely visible image: `SRCALPHA/SRCCLR tev=2 x26 (1352 verts)`
  reg1≈(194,242,190) blue = the sea/scene composite (samples the EFB-copy textures) + the ortho HUD.

⇒ **GC's visible file-select image is the pass3 composite sampling EFB-copy textures.** pass1/pass2 write
almost no color directly — they build depth/alpha masks that are copied to textures. Native has no
EFB-copy textures, so it cannot reproduce pass3; instead native paints the `[noC]` pass1/pass2 draws
directly to get *something* on screen. The b76 overbright is one symptom of that: the MapXlu mask is a
total no-op on GC (cU=0, aU=0, zw=0 — writes nothing), but native paints it white, mis-projected
full-screen (ndcX[-106119,124861]).

### Why native draws the mask in ph1+ph6 (not ph4 like GC) — the precise divergence
- GC draws the 3-stage MapXlu mask ONLY in pass2 (= ph4 mPerformListGX); native's unk40 Draw-Buffer-Group
  flush (ph1) and the post pass (ph6) both draw it, ph4 does not. So native's MapXlu draw buffer holds
  the mask packet when unk40/post run, whereas GC's pre-pass buffer-group does NOT draw this mask.
- Net: native draws this no-op mask TWICE in the wrong passes, both with colorUpdate restored to TRUE,
  so it paints. The fix is **pass-routing / buffer-fill timing**: the MapXlu mask must be drawn (if at
  all) in the main pass within the cU=FALSE window, not in unk40/post — i.e. native's draw-buffer flush
  order must match GC's (the buffer-group draw in unk40 must NOT contain the main-pass-only mask).

### The honest conclusion (for the successor)
The b76 overbright is NOT independently fixable by a colorUpdate read — native reads every other field
right (aU=0, zw=0); only colorUpdate is wrong, and it's wrong because of the pass-routing/flatten. The
real, faithful fix is the **EFB-copy-texture composite** (the segmented present infra from the earlier
update): build the pass1/pass2 EFB→texture snapshots and make the pass3 composite sample them — then the
`[noC]` masks correctly write nothing and the visible image comes from the composite, as on GC. That is
large but is the only non-bandaid path. Tooling now in place to drive it by value: SB_B76_DBG +
sb_gx_colupd_history (native), SUNBRIGHT_DBG_GXBLEND/GXTEV/GXCOPY (oracle), fileselect_overbright.py.

## UPDATE 2026-06-30 (latest) — backtrace NAMES the mask's two native flush owners
Added `SB_B76_BT` (one backtrace per phase at the b76 capture). The MapXlu mask is flushed via
`J3DDrawBuffer::drawHead` in BOTH:
- **ph1**: `TMarDirector::direct+0x83b → TPerformList::perform → TViewObjPtrListT::perform → drawHead`
  = the **unk40 "Draw Buffer Group"** flush (the `unk40->push_back(drawBufferGroup,8)` pre-pass draw).
- **ph6**: `TMarDirector::direct+0x8e0 → TPerformList::perform → drawHead` = the **mPerformListGXPost**
  entry **`composite3` ("合成3")** (pushed 0x8 in initECDisp / MarDirectorInitECT.cpp:155). `合成3` is a
  SCENE-LOADED TViewObj (only ever `search`ed, never `new`ed in the decomp) that draws a J3DDrawBuffer —
  the screen composite. Its buffer still holds the MapXlu mask packet ⇒ native re-draws the no-op mask.

So native flushes the MapXlu mask buffer in **unk40 (Draw Buffer Group)** + **GXPost (composite3)** but
NOT mPerformListGX (the main pass) — the OPPOSITE of GC (oracle: mask only in pass2 = main). The fix is
to make native's J3D draw-buffer assignment/flush match GC so the mask flushes once in the main pass
within its cU=FALSE window (or implement the EFB-copy-texture composite so composite3 samples a snapshot
and the [noC] mask writes nothing). The contradiction to resolve first: on GC the SAME unk40 Draw Buffer
Group + GXPost composite3 run, yet the oracle shows the mask combiner ONLY in pass2 — so on GC the MapXlu
buffer must be EMPTY (already drained/frameInit'd) during the unk40 + GXPost flushes. Trace the MapXlu
buffer's frameInit/entry/draw order per pass (SB_FI_TRACE + a new entry/draw trace) to find why native's
buffer still holds the mask there. Tooling: SB_B76_BT, SB_B76_DBG (ring/colupd), SB_FI_TRACE, SB_DRAWBUF_INV.

## UPDATE 2026-06-30 (latest) — SB_DBHEAD_DBG buffer-flush map: MapXlu uniquely routed ph1+ph6
Added `SB_DBHEAD_DBG` (J3DDrawBuffer::drawHead trace: buffer ptr + capture phase + packet count) +
`sb_boot_capture_phase()`. Cross-referenced with `SB_DRAWBUF_INV` (MapXlu = buf 0x..200dc, 2 packets).
The settled per-pass flush map:
- MapXlu (200dc): **ph1 + ph6**  ← THE ANOMALY
- MapOpa (1fe3c), Sky Xlu (1fd7c), 20f40 (16pk), 210e0 (2pk): **ph1 + ph4** (main pass) — all siblings
- ec7c60, ec7b18: ph6 only (composite3's own buffers)

So **every scene buffer flushes in ph1 (unk40 pre-pass) + ph4 (mPerformListGX main) EXCEPT MapXlu,
which flushes in ph1 + ph6 (GXPost)**. MapXlu is uniquely routed to the POST pass, not the main pass.
The owner is `composite3` ("合成3", GXPost) — the SEA-REFLECTION composite that draws the translucent
map (sea water) after the mirror EFB texture is ready. That is plausibly FAITHFUL for the sea SURFACE.

**The bug**: the MapXlu buffer holds **2 packets** = the sea surface (tev=2, the visible blue composite
= oracle draw#1184, pass3) AND the tev=3 white-saturating MASK (= oracle draw#1180, pass2/MAIN). drawHead
draws BOTH together in ph6 → native paints the tev=3 mask white in the post pass. On GC the mask draws in
the MAIN pass (pass2, cU=FALSE no-op) and the sea in the POST pass (pass3, cU=TRUE visible) — they are
SEPARATED (different buffers or different draw times). Native has both in ONE buffer flushed once in ph6.

**NEXT (the real fix)**: separate the two MapXlu packets so the tev=3 mask draws in the MAIN pass (ph4,
cU=FALSE → writes nothing) and only the tev=2 sea surface draws in ph6 (composite3, cU=TRUE). Investigate
why native's MapXlu buffer contains the mask packet at composite3's ph6 flush — either (a) the mask should
be entered into a DIFFERENT buffer (one flushed in the main pass like MapOpa), or (b) GC frameInit's /
re-enters the MapXlu buffer between the main pass and composite3 so the post flush sees only the sea. Trace
the MapXlu entry() calls (which models, which pass) + frameInit timing vs MapOpa (the sibling that DOES
flush in ph4). Tooling: SB_DBHEAD_DBG, SB_DRAWBUF_INV, SB_FI_TRACE, SB_B76_BT/DBG. Verify:
fileselect_overbright.py 42.7→~14 WITHOUT ablating b76.

## UPDATE 2026-06-30 (latest) — CORRECTION: MapXlu-in-GXPost is FAITHFUL (game data); b76 is a degenerate near-plane volume
`SB_PL_DBG` (PerformList::load entry dump) shows the GAME'S OWN perform-list data routes
**`DrawBuf MapXlu` filter=0x8 (draw) into `PerformList GX Post`** — NOT `PerformList GX` (main).
The main list draws the MIRROR buffers (DrawBuf Mirror Opa/Xlu, MirrorSky, MirrorAlways) + MapOpa +
マップグループ + Sky; the post list draws ChrOpa, MapXlu, the 半透明優先 map buffers, StaticMapObj Sun/
ShadowXlu, beam mgr, etc. **So native flushing MapXlu in ph6 is FAITHFUL — NOT the bug.** (The earlier
"MapXlu uniquely routed ph1+ph6" anomaly is just that MapXlu's draw-bit entry lives in GX Post by design;
the ph1 flush is the unk40 Draw-Buffer-Group pre-pass.)

**⇒ The journal's per-stage TEV refutation compared b76 to the WRONG oracle draw.** b76 (native MapXlu
POST draw, bm=4/2 SRCALPHA/SRCCLR) must compare to GC's **POST-pass** MapXlu SRCALPHA/SRCCLR draw
(`draw#1184`, tev=2, reg1≈(194,242,190) BLUE/teal), NOT the MAIN-pass mask (`draw#1180`, tev=3). The
prior session matched by combiner-hash to draw#1180 and concluded "TEV-gen faithful" — that comparison
was against a different draw (the main-pass mirror/depth mask).

**What b76 actually is**: vc=15, z[-41056,+64870] (SPANS the near plane — verts behind the camera),
ndcX[-106119,124861] (near-plane-crossing projection explosion → covers the screen), tev=3 white-
saturating, rgb=0.87. GC's post MapXlu visible sea (draw#1184) is tev=2 blue, 52 verts, normal geometry.
So b76 is a SEPARATE degenerate near-plane-spanning volume that native fails to near-plane-CLIP (so it
covers the screen) AND paints white. Either (a) it's a [noC]/culled no-op on GC that native draws, or
(b) native mis-clips a volume GC clips away.

**NEXT (re-pointed)**: (1) find GC's match for b76 by VALUE — is there a 15-vert near-plane-spanning
SRCALPHA/SRCCLR tev=3 draw anywhere in the oracle, and is it [noC]/clipped? (2) Identify b76's model
(0x4332ae0, a MapXlu packet) — RE which sea/map sub-object it is and its geometry. (3) The prime fix
suspects are now NEAR-PLANE CLIPPING (native's clip lets a camera-spanning volume blow up full-screen)
and/or that this volume is a depth/occlusion no-op GC culls. Verify fileselect_overbright.py 42.7→~14.
Tooling: SB_PL_DBG (perform-list entries), SB_DBHEAD_DBG (buffer flush map), SB_DRAWBUF_INV, SB_B76_DBG.

## UPDATE 2026-06-30 (CORRECTION OF THE CORRECTION) — b76 IS draw#1180 (tev=3); the real bug = mask mis-entered into MapXlu
The previous update wrongly claimed b76 maps to draw#1184 (tev=2). **That was an error.** Verified:
`scratch/frames/bfrag_76.glsl` has **3 stages** (stage 0/1/2) → b76 is **tev=3**, which matches the
oracle's **draw#1180** (pass2/MAIN, tev=3, SRCALPHA/SRCCLR, **[noC][noA]**, ~11 verts) — exactly the
ORIGINAL journal match. draw#1184 (tev=2, blue) is the SEPARATE visible sea. So b76 = the tev=3 [noC]
camera-surrounding MASK, drawn by GC in the MAIN pass with colorUpdate/alphaUpdate FALSE (writes nothing).

**The genuinely NEW, correct finding (keep this):** `SB_PL_DBG` proves `DrawBuf MapXlu` is drawn in
**PerformList GX Post** (game data) — so native flushing MapXlu in ph6 with colorUpdate=TRUE is faithful
FOR THE SEA SURFACE (draw#1184, tev=2, the OTHER MapXlu packet, correctly visible). The BUG is that the
tev=3 [noC] MASK packet (b76) is ALSO in native's MapXlu buffer (which has **2 packets** per
SB_DRAWBUF_INV) → it draws with the sea's cU=TRUE in post → painted white. On GC the tev=3 mask is NOT in
MapXlu — it is drawn in the MAIN pass ([noC]) by a different buffer. native MIS-ENTERS the mask into MapXlu.

**Where the mask belongs on GC**: the main list (PerformList GX) draws DrawBuf Mirror Opa/Xlu, MirrorSky,
MirrorAlways, MapOpa, マップグループ — all [noC]-capable main-pass buffers. native's ph4 flushed only
MapOpa/SkyXlu/20f40/210e0 — **NO mirror buffers** (native doesn't populate the mirror reflection render).
So the prime hypothesis: the tev=3 mask is the MIRROR water-volume / silhouette mask that GC draws into a
Mirror buffer in the main pass ([noC]); native, not running the mirror, mis-routes it into MapXlu (post,
cU=TRUE) → white wash.

**NEXT (accurate)**: (1) Identify b76's model (0x4332ae0) and which scene object ENTERS it into the MapXlu
buffer — add an entry-pass trace (tap J3DDrawBuffer::entryMatSort/entryMatAnmSort, print J3DMaterial ptr +
backtrace, match the b76 material 0x..8568 in the SAME run). (2) Determine if it's the mirror/silhouette
mask and whether native should (a) draw it [noC] (honor the mask's intended colorUpdate) or (b) route it to
a main-pass buffer / not enter it into MapXlu. (3) Whichever, the SEA (tev=2) must stay visible. Verify
fileselect_overbright.py 42.7→~14 WITHOUT ablating b76.

## UPDATE 2026-06-30 (BREAKTHROUGH) — b76 = a map.bmd joint entered by TMapModel; it's the shine-shadow/pollution EFB-readback class
`SB_ENTRY_MAT` (entry-pass backtrace, targeting the b76 material ptr the capture publishes via
sb_b76_material) names the enterer DEFINITIVELY:
```
J3DDrawBuffer::entryMatSort <- J3DJoint::entryIn <- J3DMtxCalcBasic::recursiveEntry <- MActor::entry
  <- TJointModelManager::perform <- TPerformList::perform <- TMarDirector::direct
```
**`TMapModel` IS a `TJointModelManager`** (MapModel.cpp:124; mJointModelNum=1, initJointModel(
"scene/map/map")). So the b76 mask is a **joint/material inside map.bmd**, entered into a translucent
draw buffer by the MAP itself. It is NOT a separate composite/mirror object.

**⇒ This is the SHINE-SHADOW / POLLUTION-mask EFB-READBACK effect class** — the same family as CLAUDE.md's
`drawShineShadowVolume` (gated in TModelWaterManager::perform) / "pollution darkening" / sun-occlusion, and
the **PARKED [[delfino-lighting-wash]]**. On GC this map joint is a camera-surrounding volume drawn
**[noC]** (colorUpdate FALSE — writes no colour) whose visible effect is a SEPARATE EFB-readback darkening
composite. native has no EFB readback, draws the volume cU=TRUE → opaque-white full-screen wash. So the
file-select overbright and the Delfino wash are the SAME unimplemented effect, NOT a TEV/blend/routing bug.

**THE FIX (faithful, bounded)**: make native draw this map-joint mask with **colorUpdate=FALSE** so it
writes nothing — matching GC's DIRECT framebuffer result (the EFB-readback DARKENING remains a separate,
acknowledged parked gap; absence of darkening is far closer than a white wash). The mask is a specific
material in map.bmd; GC wraps its draw in GXSetColorUpdate(FALSE) via the shine-shadow/pollution effect
path (drawShineShadowVolume / TModelWaterManager). NEXT: (1) identify WHICH map.bmd joint/material this is
(extend SB_ENTRY_MAT to print the joint index/name + the material's TEV/texNo) and find how GC marks it
[noC] (the effect that wraps it, or a material flag). (2) Reproduce that [noC] for this joint natively
(own the shine-shadow path, or recognise the volume and honor its no-colour intent). (3) Keep the tev=2
SEA visible. Verify fileselect_overbright.py 42.7→~14 WITHOUT ablating b76. See memory
[[fileselect-overbright-screen-dome-white]] + [[delfino-lighting-wash]].

## UPDATE 2026-06-30 (FULL-PORT plan — user chose the full pass-structure port) 
Corrected: native's EFB copies DO fire (SB_COPYTEX_DBG): 2/frame — (1) 256x256 mirror clear=1
(鏡描画ステージ, = the pre-pass copy, oracle copy1 @653 prims), (2) 320x224 from 640x448 clear=0
(通常シーン描画ステージ soft-focus, = oracle copy2 @1184). The earlier "zero copies" was WRONG.

**The structure, fully mapped:**
- pre-pass (prims 0-653) = MIRROR reflection render → copy to 256x256 mirror tex + CLEAR EFB.
- main pass (prims 653-1184) = the scene incl. the tev=3 [noC] shine-shadow MASK (draw#1180) → copy
  to 320x224 soft-focus tex.
- post (prims >1184) = composite (samples the EFB textures) + the tev=2 SEA (draw#1184, cU=TRUE) + HUD.
- The mask geometry is a map.bmd joint (TMapModel/TJointModelManager). On GC it draws [noC] in the MAIN
  pass; native ENTERS it into DrawBuf MapXlu which flushes in GX Post (post, cU=TRUE) → white.

**Why native diverges:** the map's translucent joints all go to MapXlu (post). GC splits them: the mask
draws [noC] in the main pass, the sea cU=TRUE in post. The exact GC split mechanism (the map's main-pass
draw vs the MapXlu post draw — マップグループ 0x8 in PerformList GX vs DrawBuf MapXlu 0x8 in GX Post) is
the ONE remaining RE step. native's Mirror draw buffers are also EMPTY (SB_DRAWBUF_INV) — the mirror
reflection isn't populated. xluCount=0 so it's NOT the priority-buffer (半透明優先) system.

**THE FULL PORT (tasks #2/#3/#4, the user's chosen path):**
1. Pin the GC main-pass map-draw mechanism that issues the mask [noC] (trace マップグループ 0x8 vs the map
   MActor draw; is the mask drawn TWICE — main [noC] + post — or only main?). Likely the map MActor is
   drawn in BOTH the main pass (マップグループ) and post (DrawBuf MapXlu), and the colorUpdate differs.
2. Make native reproduce that: draw the map's main-pass pass with the live colorUpdate GC uses (so the
   mask's [noC] is honored), and ensure the post MapXlu draw is the sea only (or also [noC] for the mask).
3. Populate the Mirror reflection buffers (task #4) + segment the present honoring the mirror clear so the
   256x256 mirror tex holds the reflection, and bind it to the post sea-composite consumer (task #3).
Verify fileselect_overbright.py 42.7→~14. Diagnostic foundation (all committed, gated): SB_COPYTEX_DBG,
SB_DBHEAD_DBG, SB_ENTRY_MAT, SB_MAPXLU_DBG, SB_B76_DBG/BT, SB_DRAWBUF_INV, SB_PL_DBG; oracle GXBLEND/GXTEV/GXCOPY.

## UPDATE 2026-06-30 (DECISIVE — ends the b76-identity correction cycle; pins the exact structural fix)
Two measurements settle every open thread above.

**(1) Native side — SB_MAPXLU_PKT (new probe, committed) dumps the MapXlu buffer's packets at the
settled file-select (frame 566):**
```
[mapxlu-pkt] key=f19161bf… vc=30 ntex=1 vClr0=255,255,255,255 eyeZ[521,1609]                      ← SEA (normal geom, in front)
[mapxlu-pkt] key=eb5c8e74… vc=15 ntex=2 vClr0=255,255,255,255 eyeZ[-50800,56113] <-CAMERA-SPANNING ← MASK = b76 (degenerate volume)
```
So native's MapXlu buffer holds **2 packets**: the SEA (key f191, 30 v, eyeZ all +ve = real geometry)
and the MASK (key eb5c8e74 = b76, 15 v, eyeZ spans the near plane = camera-surrounding volume). Both
flush together in ph6 (GX Post, composite3) with the global colorUpdate restored to TRUE → native
paints the mask opaque-white = the overbright.

**(2) Oracle side — SUNBRIGHT_DBG_GXBLEND (build/sunbright, settled frame 4) shows the two
SRCALPHA/SRCCLR draws live in DIFFERENT EFB passes:**
```
pass2 (MAIN)  SRCALPHA/SRCCLR tev=3 [noC][noA] persp x3 (11 verts)   ← the MASK (b76). writes NOTHING.
pass3 (POST)  SRCALPHA/SRCCLR tev=2            persp x26 (1352 verts) ← the visible SEA composite.
```
There is **NO tev=3 SRCALPHA/SRCCLR draw in pass3** anywhere. So on GC the tev=3 mask is drawn
**only in the MAIN pass (pass2), `[noC][noA]` = a depth-only occlusion volume that writes no colour**;
the visible sea (tev=2) is the SEPARATE post-pass composite. The EFB soft-focus copy (通常シーン描画
ステージ, prim 1184) fires BETWEEN them: mask just before (main), sea just after (post).

**The bug, stated exactly:** native lumps the depth-only mask packet and the visible sea packet into
ONE MapXlu buffer and draws BOTH in the post pass (ph6) with cU=TRUE. GC draws the mask in the MAIN
pass as a no-colour depth volume and the sea in POST. Native's pass routing is wrong for the mask.

This DEFINITIVELY closes every prior "b76 = draw#1180 vs #1184 / CLR0 / TEV-gen / mirror" oscillation:
b76 IS the tev=3 main-pass depth-only mask (matches draw#1180), and the fix is neither a combiner nor a
blend nor a CLR0 change — it is the **pass split**. The bounded "honor live colorUpdate" fix already in
`fill_batch_material` (sms_boot_j3d_capture.cpp:350-356) CANNOT work: at the ph6 flush the global cU is
TRUE, and native does not run the soft-focus effect's GXSetColorUpdate(FALSE) that wraps GC's main pass.

**THE FIX (user-chosen full pass-structure port, now precisely scoped to ONE divergence):** the depth-
only mask packet must be drawn in native's MAIN-pass segment with colour writes OFF (matching GC's
no-colour result), and the sea packet stays the post composite. Native already segments the present at
EFB-copy boundaries (sms_boot_present.cpp:683-700) — the missing piece is splitting the MapXlu buffer's
two packets across the soft-focus copy boundary (mask→main segment [noC], sea→post segment) instead of
flushing both in ph6. Equivalently: stop entering the depth mask into the post-flushed MapXlu buffer;
draw it in the main pass as the depth-only volume GC does. The EFB-readback DARKENING that the mask sets
up remains the separately-parked [[delfino-lighting-wash]] gap — but "mask writes no colour" already
matches GC's framebuffer (no white wash). Verify: fileselect_overbright.py 42.7 → ~14 WITHOUT ablating b76.

Tooling added this session: SB_MAPXLU_PKT (native MapXlu per-packet dump). Oracle proof reproduced from
scratch/passes/oracle_blend2.log (SUNBRIGHT_DBG_GXBLEND frame 4).

## UPDATE 2026-06-30 (next session) — RE-CONFIRMED from oracle ground truth; one excursion corrected; raster probe rules out CLR0/lighting
Continued from bad76b0. Two solid results + one self-correction.

**(1) FRESH measurement — b76 is the dominant wash (re-verified).** `SB_SKIP_KEY=eb5c8e74` (drop the
b76 packet) on a settled file-select run: `fileselect_overbright.py` 42.7 → **14.2**. The 4×4 region grid
shows b76 covers the whole screen (bottom rows go to ~0 when dropped); the +108 top-row residual is the
SEPARATE sky-dome layer. Baseline 42.7 is **std-PRESERVING (additive)**, not std-compressing — consistent
with a full-screen blended layer, not a hard white-saturate.

**(2) SELF-CORRECTION — I briefly mis-refuted the [noC] diagnosis; the oracle blend log REFUTES my
refutation.** Mid-session I argued (from `SB_PL_DBG`: `DrawBuf MapXlu` draws ONLY in `PerformList GX Post`,
and J3D materials never call `GXSetColorUpdate`) that b76 must be a VISIBLE joint, not a [noC] mask. That
was WRONG. `scratch/passes/oracle_blend2.log` (SUNBRIGHT_DBG_GXBLEND, ground truth) settles it:
```
pass2  SRCALPHA/SRCCLR tev=3 [noC][noA] x3 (11 verts)   ← b76's counterpart: WRITES NOTHING
pass3  SRCALPHA/SRCCLR tev=2            x26 (1352 verts) ← the VISIBLE sea composite (EFB-texture sampler)
```
b76 (tev=3) maps to the **[noC][noA]** draw; the visible sea is **tev=2, 1352 verts, in pass3**. So the
journal's long-standing "b76 = tev=3 [noC] mask native paints white" is **CORRECT**. The visible sea native
is missing (its MapXlu only has 15v mask + 30v f191, NOT the 1352v tev=2 sea) is the pass3 EFB-texture
composite native can't reproduce.

**The useful refinement this excursion produced:** the [noC] is **PASS-LEVEL, not per-draw**. Nearly EVERY
pass2 draw in the oracle is `[noC]`/`[noC][noA]` (lines 81–115) — pass2 is the whole main-scene-render-to-EFB-
texture, run with `GXSetColorUpdate(FALSE)` set ONCE by the EFB-control object (`TEfbCtrl::perform(0x80)` →
`GXSetColorUpdate(!unk20.check(0x100))`, JDREfbCtrl.cpp:11; the `通常シーン描画ステージ` setup) — NOT a
colorUpdate object adjacent to the MapXlu draw (there is none; `SB_PL_DBG` shows MapXlu bracketed only by
`Map Draw SnapTime` timers). native never enters that pass-level [noC] state, so it paints all of pass2.

**(3) RASTER PROBE rules out CLR0 / lighting / mirror as the b76 "fix".** New gated probe `[b76-raster]`
(SB_B76_DBG, sms_boot_j3d_capture.cpp) prints the mask's raster inputs at the settled frame:
```
[b76-raster] lit=1 nlights=3 do_light=1 cc0=0706 matSrcVtx0=0 rawCLR0=255,255,255,255 matColor0=255,255,255,255
```
`matSrcVtx0=0` → the raster colour is the **material register (white)**, not per-vertex; lit=1. So b76's
colour is lit-white — but **its colour is IRRELEVANT** (on GC it writes no colour). Chasing CLR0/TEV-gen/
lighting/mirror-binding for b76 is pointless; the ONLY faithful fix is to make it **write no colour**, which
requires honoring pass2's pass-level [noC]. The bfrag (`scratch/frames/bfrag_76.glsl`) doubles vColor to
saturation (`2·vColor`), and its bound texture feeds **alpha only** — so neither a darker vColor nor the
mirror texture would matter; the draw must simply not paint.

**Why there is no bounded fix (and the real fix):** honoring pass2's [noC] makes native draw the whole main
scene with no colour → BLACK, because native relies on painting the [noC] passes visibly (it has no pass3
EFB-texture composite to show the scene). So [noC] honoring regresses (same class as honor-clear 42.7→45.7).
The ONLY faithful fix is the **EFB-copy-texture multi-pass composite**: render pass2 into the EFB (masks
[noC]), copy EFB→texture, then render pass3's tev=2 composite SAMPLING that texture (= the visible scene +
sea). native has partial infra (`draw_tev_segment`/`snapshot_efb`/EFB-sampler binding, sms_boot_present.cpp)
but the pass3 composite does not yet reproduce the scene from the snapshot (binding it is currently
net-neutral). That is the next, large, well-scoped task — the user's chosen full pass-structure port.

Net: measurement + diagnosis re-confirmed from GROUND TRUTH; b76 colour-fix paths (CLR0/TEV/lighting/mirror)
are DEAD; the fix is the EFB-copy-texture composite. Tooling added: `[b76-raster]` probe, fresh
`scratch/passes/{oracle_blend2.log analysis, pl_dbg.log, b76_raster.log, skip_b76 frame}`.

## UPDATE 2026-07-01 — divergence narrowed to a per-draw colorUpdate mismatch (infra confirmed working)
Continued from 10efee7. Fresh baseline + full pipeline trace (Explore agent map archived in this session).

**Fresh measurement (SB_OWN_GXLIST settled frame 566):** overbright = **42.7** (unchanged). b76 (key
eb5c8e74) confirmed a CAMERA-SPANNING volume: `[mapxlu-pkt] key=eb5c8e74 vc=15 ntex=2 eyeZ[-50800,56113]`
— a degenerate full-frustum volume, not visible scene content. The other MapXlu packet is f191 (vc=30,
eyeZ[521,1609]).

**The full segmented-present infrastructure ALL WORKS** (the journal previously implied EFB copies weren't
detected — WRONG; that was me not printing the count). Verified with SB_J3D_DBG `[efbcopy] mark`:
native records **2 EFB copies/frame**:
- copy0: `batch_index=49 dest=…79c0 clear=1 256x256 (phase 4)` = the sea MIRROR (鏡描画ステージ)
- copy1: `batch_index=79 dest=…3cda0 clear=0 320x224 (phase 6)` = the SOFT-FOCUS (通常シーン描画ステージ)
So `ncopies=2`, the segmented present runs (sms_boot_present.cpp:684), snapshots are taken at both
boundaries, and the 320×224 soft-focus imm quad DOES bind its snapshot (efb_src) — but it's net-neutral
because the snapshot already holds the cumulatively-painted scene (native paints everything).

**ORACLE GROUND TRUTH (scratch/passes/oracle_blend2.log, SUNBRIGHT_DBG_GXBLEND) — the pass split is exact:**
```
line 81-116  pass1+pass2: ~all [noC]/[noC][noA] (the OFF-SCREEN render-to-EFB-texture passes)
line 115     pass2  BLEND SRCALPHA/SRCCLR tev=3 [noC][noA] x3 (11 verts)  ← b76 counterpart: WRITES NOTHING
line 117     pass3  BLEND SRCALPHA/SRCCLR tev=2          x26 (1352 verts)  ← THE VISIBLE composite (colour ON)
```
The GXBLEND "passN" labels are by EFB-COPY BOUNDARY, not perform-list phase. So the visible file-select
frame on GC is a POST-PROCESS COMPOSITE: pass1+pass2 render the scene off-screen (colorUpdate FALSE),
copy EFB→texture, then pass3's 1352-vert tev=2 quad samples those textures and is the ONLY colour-writing
draw. native lacks pass3 and instead paints pass1+pass2 directly → overbright; the soft-focus quad then
re-adds them → double-bright.

**THE DIVERGENCE IS NOW A VALUE, not a deduction.** native draws the b76 mask in phase 6 with **liveCU=1
liveAU=0** (`[b76] phase=6 … liveCU=1 liveAU=0 last_false@…(delta=3) ring=1110101010110111`). The oracle
has the SAME draw as **[noC][noA]** (cU=FALSE). SB_COLUPD_BT shows every phase-6 GXSetColorUpdate caller is
**enable=1** (TPerformList::perform+0x45, SMS_DrawInit, TModelWaterManager::perform+0x1b0,
TZBufferCatch::perform+0x25). So native restores cU=TRUE before the MapXlu flush where the oracle keeps it
FALSE — native's colorUpdate sequence diverges from Dolphin's at this exact draw. The main-pass [noC] is set
by `TEfbCtrl::perform(0x80) → GXSetColorUpdate(!unk20.check(0x100))` (JDREfbCtrl.cpp:11); native runs that
in phase 4 but the window has closed (cU restored TRUE) by the phase-6 MapXlu flush.

**WHY there is no bounded mask-skip:** native paints pass1+pass2 to SHOW the scene at all (it has no pass3
composite). Skipping/[noC]-honoring the b76 mask alone (SB_SKIP_KEY=eb5c8e74 → 42.7→14.2) removes the
dominant wash but is only a measurement, not a faithful fix — the residual 14.2 sky-dome layer + the fact
that the whole pass2 is [noC] mean the faithful fix is the EFB-copy-texture composite (user-chosen full
pass-structure port): render pass1+pass2 to an OFF-SCREEN target (honoring per-draw [noC] so the snapshot is
clean), then clear the visible FB and render pass3 = the composite sampling the snapshots.

**NEXT (TOOLING-FIRST):** the manual log-reading cycle keeps oscillating on the pass/phase labeling. Build a
DETERMINISTIC per-draw native-vs-oracle GX-state diff (colorUpdate/alphaUpdate/blend/tev per draw, aligned by
draw order within a frame) using the oracle's gx_stream/gx_parse capture (runtime/gx_*.cpp) + a matching
native per-draw dump. That turns "native cU diverges somewhere" into the EXACT draw + the missing
GXSetColorUpdate(FALSE)/extra TRUE. THEN either (a) fix native's colorUpdate sequence so b76 captures [noC],
or (b) implement the off-screen-render + pass3-composite split. Do NOT do another speculative blend ablation.

## UPDATE 2026-07-01 (DETERMINISTIC DIFF BUILT — b76 was a red herring; divergence is the WHOLE pass structure)
Built the per-draw native-vs-oracle GX-state diff the prior NEXT called for. It ends the b76/pass/phase
oscillation outright by comparing the two engines by VALUE, deterministically, across EVERY draw — not the
single b76 mask.

**Tooling (committed):**
- Oracle ordered per-draw dump: `SUNBRIGHT_DBG_GXDRAW=1` on build/sunbright → `[gxdraw] fr=.. i=.. pass=..
  cU=.. aU=.. be=.. src=.. dst=.. sub=.. tev=.. proj=.. v=..` (one line per draw, in draw order, frames 4-7;
  gx_capture.cpp after the GXBLEND block; record_draws now also enabled by GXDRAW in gx_parse.cpp).
- Native ordered per-draw dump: `SB_GXDRAW=1` on build-native/sms-boot → same `[gxdraw]` fields per BATCH in
  flush order (sms_boot_j3d_capture.cpp `end_scene`). Added `num_stages` to NvkTevBatch+MatEntry so the TEV
  stage count (the join key) is on the batch.
- `tools/render/gxstate_diff.py` — groups BOTH engines' draws by GX-state SIGNATURE (be,src,dst,sub,tev) and
  reports each engine's colorUpdate/alphaUpdate value-set + draw/vert counts + passes/phases per signature.
  Signature is the only valid cross-engine join (native merges shapes by material+phase → ~79 batches; oracle
  is ~1170 per-primitive draws — index-align is impossible). `tools/render/gxstate_diff.sh` captures both +
  diffs. Artifact: `scratch/passes/gxstate_diff_result.txt`.

**THE DECISIVE RESULT (settled file-select, oracle frame 6 vs native settled frame):**
```
ORACLE colorUpdate by EFB pass:        NATIVE colorUpdate by phase:
  pass1 cU0: 653 draws, 3518v          phase1 cU1: 31 batches, 11331v
  pass2 cU0: 435 draws, 3035v          phase4 cU1: 29 batches, 11286v
  pass2 cU1:  12 draws,   96v          phase6 cU1: 19 batches,  7551v
  pass3 cU0:  11 draws,   44v          (NOTHING is cU0 on native)
  pass3 cU1:  59 draws, 1484v
```
- **The oracle writes COLOUR for only ~1580 verts** (pass3's 1484v visible composite + pass2's 96v). pass1
  (653 draws — the MIRROR reflection render) and pass2-main (435 draws) are **entirely cU=0** = off-screen
  render-to-EFB-texture (copied out, then the visible pass3 composite SAMPLES them). Confirmed pass-level, not
  per-draw: ALL of pass1 and 97% of pass2 are cU=0.
- **Native writes COLOUR for ALL 30168 verts** (everything cU=1), across ph1/ph4/ph6, and **ph1 (11331v) ≈
  ph4 (11286v) = the scene painted TWICE** (the unk40 pre-pass + the main pass, both composited visibly).
- **The b76 mask is NOT special.** The diff shows **11 signatures** with the identical cU divergence (oracle
  cU0 / native cU1), e.g. `SRCALPHA/INVSRCALPHA tev=5` (443 oracle draws p1 cU0 vs 30 native batches cU1),
  `ONE/INVSRCCLR tev=1` (76 cU0 vs 2 cU1), … and the tev=3 SRCALPHA/SRCCLR mask is just ONE of them. Chasing
  b76 alone was the wrong unit the whole time — it is the entire off-screen pass structure native flattens.
- **Native LACKS the visible pass3 composite entirely.** The oracle's `SRCALPHA/SRCCLR tev=2` (26 draws,
  1352v, p3, cU1 — the visible sea/scene composite that samples the EFB textures) and `SRCALPHA/INVSRCALPHA
  tev=3` (30 draws, 120v, p3 ortho) are **oracle-only** — native has no batch of that signature. Native shows
  the scene only because it paints the off-screen [noC] passes directly.

**⇒ This DEFINITIVELY confirms the structural diagnosis and kills the "fix b76's colorUpdate" path (3a):**
honoring cU per-draw makes native draw the off-screen passes with no colour → BLACK, because native has no
pass3 composite to show the scene. The ONLY faithful fix is path (3b), the EFB-copy-texture composite:
render pass1 (mirror) + pass2 (main) OFF-SCREEN honouring cU=0, copy EFB→texture at each boundary (native
already records both copies: 256×256 mirror clear=1, 320×224 soft-focus clear=0), then render the pass3
composite (cU=1) SAMPLING those snapshots as the visible image. Native has the snapshot infra
(draw_tev_segment/snapshot_efb/efb_src) but does NOT yet (a) suppress painting the off-screen passes nor
(b) reproduce the 1352v+120v pass3 composite draws.

**ONE unresolved RE question the composite port hinges on (next step):** if pass1+pass2 are cU=0 (write no
colour), what colour does the pass3 composite SAMPLE from the EFB-copy textures? Either the EFB copies grab
the CLEAR colour + an alpha/depth mask (so pass3 reconstructs colour via its TEV sampling a detail/gradient
texture, not the scene), OR a small set of cU=1 draws (pass2's 12 / pass3's own) build the visible colour and
the [noC] passes are pure depth/silhouette masks. Resolve by dumping the EFB-copy FORMAT (color vs
z/alpha) from the oracle command stream (SUNBRIGHT_DBG_GXCOPY + the copy's pixel-format BP) and reading the
pass3 composite's TEV combiner (SUNBRIGHT_DBG_GXTEV draw#1184) — both tools already exist. THEN implement the
composite. Verify every step with `tools/render/fileselect_overbright.py` (42.7 now) AND re-run
`tools/render/gxstate_diff.sh` (the off-screen signatures must flip native cU1→cU0, and a native pass3
composite signature must appear). Do NOT add another speculative blend ablation.

### 2026-07-01 (same session, RE follow-up) — the RE question RESOLVED: the beach IS a cU=0 render-to-texture composite (confirms the fix is the composite)
Pushed on the "if pass1/pass2 are cU=0, where's the colour?" question with the per-stage TEV oracle + the
rendered PNG, and it resolves cleanly (no parse bug):
- **The bit is NOT inverted / not misread.** Verified against known-visible draws: the ortho HUD (pass3,
  proj=1) and the sea (pass3, SRCALPHA/SRCCLR) BOTH read **cU=1** correctly; the beach/palm/Mario/blocks
  geometry (pass1, 653 draws, verified raw) reads **cU=0 AND aU=0**. So the visible scene geometry genuinely
  renders with colorUpdate=FALSE. The whole frame has only **71 cU=1 draws** (pass2's 12 DSTALPHA composites +
  pass3's 59) — far too few to BE the beach. The beach is therefore NOT drawn directly to the visible FB.
- **EFB-copy structure (SUNBRIGHT_DBG_GXCOPY, frame 6):** 3 copies — `@0 ->XFB CLR` (display prev frame +
  clear), `@653 ->TEX CLR` (after pass1 → copy to a texture + clear), `@1100 ->TEX` (after pass2 → copy to a
  texture, no clear). So pass1 (653 cU=0 draws) renders the scene to EFB, copies it to a TEXTURE, clears;
  pass2 (cU=0) renders more to EFB, copies to a 2nd texture; pass3 composites.
- **pass3 composite TEV (SUNBRIGHT_DBG_GXTEV draw#1100, tev=2):** outputs the RASTER colour `reg1≈(194,242,
  190)` teal modulated by the bound texture's ALPHA only — that specific draw is the flat teal SEA surface.
  The 26 SRCALPHA/SRCCLR sea quads (1352v) + the ortho HUD are pass3's cU=1 set.
- **CONCLUSION:** the file-select is a true **render-to-EFB-texture composite**: the scene is rendered with
  colorUpdate=FALSE to fill the EFB, copied to textures at the copy boundaries, and the visible frame is built
  by pass3 quads sampling those textures (plus the directly-drawn sea + HUD). colorUpdate=FALSE during the
  scene render is faithful BECAUSE the EFB content is consumed via the copy, not shown directly. This
  CONFIRMS (does not contradict) the committed conclusion: the only faithful native fix is the EFB-copy-
  texture composite — there is no per-draw colorUpdate tweak that helps, because honouring cU=0 with no
  composite = black, and native already paints everything to approximate the composite (→ overbright +
  double-draw). The display mechanism to reproduce: render pass1→texA, render pass2→texB, then render pass3
  sampling texA/texB as the visible image.
- **STILL TO PIN before implementing:** which pass3 draw BLITS the scene texture (texA/texB) to screen — it's
  NOT the SRCALPHA/SRCCLR sea quad (that's flat teal). Look for the early pass3 cU=1 large-coverage draw with
  a non-SRCALPHA/SRCCLR blend (NONE or SRCALPHA/INVSRCALPHA) whose bound texmap == an EFB-copy dest. Then the
  native segmented present (draw_tev_segment/snapshot_efb/efb_src already exist) must: render the cU=0 scene
  into segment A (off-screen), snapshot to texA at the copy boundary, CLEAR, render segment B → texB, then
  render the pass3 composite binding texA/texB. Native currently composites cumulatively (every segment LOAD)
  → it shows the scene but over-bright/double; the fix is to make pass3 the ONLY visible draw + bind snapshots.

## 2026-07-01 (later) — INTERLEAVED per-pass framebuffer differ built (passdiff.py); divergence = PASS1; fix structure pinned
User pushed back on the two-separate-processes + aggregate-signature diff: "is it not possible to
build something that runs interleaved rendering and automatically surfaces where the divergence
comes from?" Right call. Built it (commit 7e94265). True per-DRAW lockstep is infeasible (native
walks J3D objects → 79 merged batches; oracle consumes the GX FIFO → 1170 prims; no shared draw
index — that's why gxstate_diff can only join by signature). But the engines DO share the PASS
boundaries (the EFB copies). So the differ compares each engine's FRAMEBUFFER at those boundaries.

**Tooling (committed):**
- ORACLE: `SUNBRIGHT_DUMP_EFB=1` (main_sdl.cpp → Config::GFX_DUMP_EFB_TARGET) → Dolphin dumps every
  intra-frame EFB→texture copy DECODED to `<home>/.local/share/dolphin-emu/Dump/Textures/efb1_n######_WxH_F.png`.
  No GC-tiled decode needed — Dolphin's own copy pipeline writes the PNG. Reproduce the SETTLED
  file-select: real-time `run.sh` (NOT headless — needs the probe) + `SUNBRIGHT_DUMP_EFB=1` +
  `SUNBRIGHT_STAGE=15`, then `/pad?do=start&ms=250` once to leave PRESS-START, settle ~40s. (Headless
  fastboot stage 15 stays on the TITLE = same beach + logo overlay; press Start to reach the option
  scene = beach + "Select data"/ABC blocks/OPTIONS.)
- NATIVE: `SB_PASS_DUMP=1` (sms_boot_present.cpp) → cumulative FB at each copy boundary →
  `scratch/frames/pass{k}_native_NNNN.ppm`.
- `tools/render/passdiff.py` groups oracle dumps by dims, matches native pass-k, prints per-channel
  mean delta + writes side-by-side (native|oracle|heat) PNGs to scratch/passes/passdiff_*.png.

**The 3 oracle EFB copies SEEN AS IMAGES (settled file-select):**
- `256x256_5` (pass1, MIRROR/鏡): NEAR-EMPTY — black except a tiny Mario reflection at top-center.
  mean ≈ (1,1,1). The file-select mirror reflects almost nothing.
- `320x224_4` (pass2, soft-focus/通常シーン): THE CLEAN SINGLE SCENE — beach, palm, sea, island,
  Mario, A/B/C blocks, OPTIONS sign. NO 2D menu banners. mean ≈ (122,172,184). THIS is what native
  must match (the scene rendered ONCE).
- `640x448_15` (pass3, FINAL): pass2 + the 2D blue menu banners ("Select data." / Corrupt/New/New)
  = the visible frame. mean ≈ (123,163,191).

**DECISIVE per-pass result (native vs oracle):**
```
pass1: native meanRGB (174,200,208) [the WHOLE bright scene]  vs  oracle (0.9,0.5,0.4) [empty mirror]  → delta 193  ← DIVERGENCE
pass2: native (194,217,212)  vs  oracle (130,177,184)  → delta 72  (overbright, additive offset)
pass3: native (182,200,214)  vs  oracle (123,163,191)  → delta 66
```
**Native draws the FULL main scene with the MAIN camera in the unk40 (ph1) mirror pre-pass** — where
GC renders an (almost empty) reflection off-screen to a 256x256 target. That scene stays in the
visible FB and the later passes pile on → the overbright + double-draw.

**WHY a simple `SB_ABLATE_PHASE=1` is NOT the fix (tested):** overbright UNCHANGED (66→66; 42.7→45.7
on the old metric, WORSE). ABLATE_PHASE sets sceneFiltered=true → DISABLES the segmented present →
the imm soft-focus quad (efb_src consumer) finds no snapshot → binds WHITE → washes the whole upper
scene (the "lost palm" in the ablate frame is the palm HIDDEN under the white soft-focus quad, not
dropped). So the soft-focus snapshot path is CENTRAL and must stay live — the fix CANNOT be a phase
ablation; it must be the proper segmented off-screen render.

**THE FIX (now fully pinned, matches the oracle pass structure):** rework the segmented present to
render OFF-SCREEN with a CLEAR at the phase boundaries, so each snapshot is clean:
  1. ph1 (unk40 mirror) → render off-screen → snapshot copy0.dest (texA mirror) → CLEAR.
  2. ph4+ph6 (main + chr) → render off-screen → snapshot copy1.dest (texB = the CLEAN scene, incl
     Mario/palm/blocks, with NO ph1 double-draw because of the clear) → CLEAR.
  3. VISIBLE frame = the composite quads (the imm soft-focus quad sampling texB + the sea quad
     sampling texA) + the 2D menu banners. The scene is shown ONCE, via texB — exactly pass3.
The crux vs the earlier failed honor-clear: clear at the PH1/PH4 PHASE boundary (use batch.phase),
NOT at copy0's batch index (which cut mid-ph4 and dropped ph4 content). NEXT: confirm the native imm
soft-focus quad covers the frame and faithfully re-displays texB (check its coverage/blend), then
implement; verify each pass goes green with passdiff.py + the overbright number.

### 2026-07-01 (same session) — the per-pass differ DISPROVES the "purely a composite/double-draw" diagnosis
Implemented the phase-boundary off-screen composite (SB_FS_COMPOSITE, sms_boot_present.cpp: render ph1
off-screen → snapshot mirror → CLEAR at the ph1End phase boundary → render ph4+ph6 → snapshot the
soft-focus texB at nScenePushed → composite). It did NOT fix the overbright, and the per-pass differ +
two phase-isolation runs (SB_ABLATE_PHASE + SB_SKIP_IMM, the 3D scene WITHOUT the soft-focus quad)
showed WHY — the prior committed diagnosis was incomplete:

1. **ph4+ph6 ALONE is still overbright.** Rendering only the main+post phases (drop ph1, drop the imm
   soft-focus quad) → the sky checker + sea are STILL washed white (scratch/frames/diag_ph46only.png).
   So a large part of the overbright is INTRINSIC to the single main-pass render — an additive/SCREEN
   blend fidelity issue (the long-known multi-layer-blend trap), NOT the ph1 double-draw or the
   composite. The render-to-EFB-texture composite cannot fix this; it's a per-pass TEV/blend problem.
2. **The palm + A/B/C blocks render in the WRONG phase.** They appear ONLY in ph1 (unk40), never in
   ph4/ph6 (diag_ph46only has Mario+island+beach+OPTIONS but NO palm, NO blocks). On GC the palm/blocks
   are in the MAIN scene (oracle pass2 = the 320x224 soft-focus has them). So native's perform-list
   phase assignment puts main-scene geometry into the unk40 MIRROR pre-pass. Any fix that renders ph1
   off-screen (correct, since GC's pass1 mirror is near-empty) DROPS the palm/blocks. Root cause likely
   in how native's SB_OWN_GXLIST drives the draw buffers across the perform lists (a buffer flushed/
   emptied by unk40 so mPerformListGX gets nothing) — a draw-buffer/perform-list mechanics bug, separate
   from the composite.

NET: the file-select overbright is NOT one bug. It is at least (a) a per-pass additive/SCREEN blend
over-brightness (intrinsic, reproduces in ph4+ph6 alone) AND (b) a phase-misassignment that puts
palm/blocks in unk40 instead of the main pass — on TOP of (c) the ph1 double-draw the composite would
fix. The SB_FS_COMPOSITE composite is committed but gated OFF (default = legacy cumulative) until (a)
and (b) are fixed, because alone it drops the palm/blocks. The interleaved per-pass differ (passdiff.py)
is what made this legible — each cause is now a separate, attackable image. NEXT: attack (a) first
(compare ph4+ph6 single-pass vs oracle pass2 320x224 via passdiff — it's a clean per-pass blend diff
now), since it's independent of the phase-misassignment and the composite.

### 2026-07-01 (cont.) — bug #1 ROOT-CAUSED via the blend drill: additive sky cloud painted as visible colour where GC has it [noC]
Built SB_BLEND_DRILL (one run dumps the main-pass scene with each blend class dropped/isolated, +
blend_drill.py ranks vs oracle pass2). It localized bug #1 precisely and the chain is now complete:

- **The sky overbright is the SRCALPHA/ONE additive cloud layer.** Drill: dropping it cuts sky delta
  91.4→36.7 (-54.7). The ONE/INVSRCCLR SCREEN layer is a DARKENING layer (drop → +42, WORSE).
  drill_only_1_3.png (SCREEN+opaque) = the CLEAN blue gradient sky + teal sea (matches the oracle).
  drill_only_4_1.png (additive+opaque) = a harsh black/white CHECKER of cloud puffs (the overbright).
- **The renderer HONORS color_update** (gx_sdlgpu.cpp:165, color_write_mask from b.color_update). So a
  batch captured cU=0 writes no RGB. The problem is the CAPTURE: native records the additive sky as
  **cU=1**; the oracle (gxstate_diff + oracle_blend2) has SRCALPHA/ONE as **[noC][noA]** (cU=0, aU=0 —
  a depth/mask-only draw in the off-screen passes). So GC writes NO colour for the additive sky; the
  visible soft clouds come via the EFB-texture composite. Native paints the depth-only checker directly
  → the dominant sky overbright.
- **Confirmed separable & quantified:** SB_ABLATE_BM=4/1 (≡ honoring cU=0 on the additive sky, since
  cU=0 with no composite = not painted) = drill_drop_4_1 = clean blue sky, delta 91→37. So correcting
  the colorUpdate CAPTURE recovers most of the sky overbright independent of the composite; the
  composite then supplies the soft clouds (the residual ~37).

ROOT CAUSE (named): native's captured per-batch colorUpdate is TRUE for the off-screen [noC] depth/mask
draws (additive sky + the other 10 cU-divergent signatures from gxstate_diff) where GC's is FALSE.
native DOES track GXSetColorUpdate (gx_impl.cpp) and reads it live at batch-open (sms_boot_j3d_capture
.cpp:358), so the bug is that the game's TEfbCtrl colorUpdate(FALSE) bracketing of the off-screen passes
is NOT in effect at those draws under SB_OWN_GXLIST (either not run, or set-then-reset before the draw).

THE FIX (two parts, both real): (A) reproduce the TEfbCtrl colorUpdate(FALSE) bracketing so native
captures cU=0 for the off-screen scene/sky/mask draws → the honored write-mask stops the over-paint
(sky 91→37, and the other 10 signatures). (B) the EFB-texture composite to supply the visible colour
the off-screen passes no longer paint (the soft clouds, the scene). (A) is the higher-value, more
separable half. NEXT: SB_DBG_COLUPD to see whether GXSetColorUpdate(FALSE) fires during the scene at
all (→ "native never brackets" vs "wrong timing"), then fix the bracketing. New tools committed:
tools/render/blend_drill.py + SB_BLEND_DRILL.

### 2026-07-01 (cont.) — SB_DBG_COLUPD: the FALSE bracketing FIRES (82445×) — it's a TIMING clobber → the bug REUNIFIES with the composite
SB_DBG_COLUPD on the settled file-select: GXSetColorUpdate is called **82445 FALSE / 424691 TRUE**. So
the game's colorUpdate(FALSE) bracketing of the off-screen passes IS running in native — native is not
missing it. The bug is TIMING: at the moment native opens the additive-sky (and other off-screen) batch,
colorUpdate has been restored to TRUE. The most likely mechanism: every J3DMaterial load/shape draw sets
GXSetColorUpdate(TRUE) from its PE block (that's most of the 424691 TRUE calls), CLOBBERING the
EFB-control's pass-level FALSE. On GC the off-screen pass's FALSE takes precedence over the per-material
value for the whole pass.

⇒ **The diagnosis REUNIFIES.** Bug #1 (sky additive overbright) is NOT a separate per-pass blend bug —
it is the SAME root cause as the double-draw and the composite: native renders the off-screen
render-to-texture passes directly into the VISIBLE framebuffer, because the off-screen-pass
colorUpdate=FALSE is clobbered per-material and there is no composite to supply the visible colour. The
additive sky is just the most visible instance. The genuinely separate issue is bug #2 (palm/blocks
captured in ph1/unk40 instead of the main pass).

⇒ **The fix is the off-screen composite, done right** (SB_FS_COMPOSITE direction): render ph1 (mirror)
and the main scene to OFF-SCREEN targets — which inherently keeps them OUT of the visible FB and
sidesteps the per-material colorUpdate clobber entirely (no need to fight the cU sequence) — snapshot
each, then build the visible frame from the composite quads + 2D menu. The remaining blockers for that
composite (all identified): (1) the soft-focus snapshot must be taken at end-of-scene, not the recorded
mid-scene copy index (fix staged in SB_FS_COMPOSITE); (2) bug #2 — palm/blocks are in ph1, so an
off-screen ph1 drops them (must move them to the main pass or composite ph1's colour too); (3) the
composite quads must faithfully re-display texB as the visible scene. This is the large, well-scoped
rendering-architecture task the prior sessions pointed at — now with every sub-blocker named and a
per-pass differ (passdiff.py) + blend drill (blend_drill.py) to verify each step by VALUE.
