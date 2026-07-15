# 2026-07-15 — File-select parity vs the oracle: mostly faithful, 3 real residuals

User directive: file-select parity, compare against the oracle. Built the oracle harness,
captured both sides at FULL RES, and compared. The file-select (stage-15 save-blocks,
reached by pressing START at the title) renders the full beach/ocean/sky/palm scene + the
"Select data." UI faithfully. There ARE real residuals (below).

## ⚠️ Downscaled/`_640` captures LIED — always compare at full res

Early `_640` downscales showed a WHITE background + "faded" Mario and I nearly chased a
"missing background" defect. Both were DOWNSCALE ARTIFACTS: the full-res native
(`scratch/shots/fsel_blocks_1400.png`) clearly renders the whole beach scene, and Mario is
fine (his big white gloves are correct). A naive pixel-diff was ALSO junk (67% "different")
because native dumps 1280x960 (480 lines) and the aurora replay dumps 1280x896 (448 EFB
lines) — scaling both to a common size vertically MISALIGNS every UI element. LESSON: view
full res; for a real pixel diff, capture both sides at the SAME height first.

## The oracle harness (durable, reusable)

File-select is INPUT-GATED (needs a START press) so the deterministic field-count fifo
recorder can't reach it. Added to the Dolphin fork (now a SUBMODULE): DolphinNoGUI
**`--pad-start-at <field> --pad-start-frames <n>`** (fork 2059b5b) injects a headless GC
START over a VI-field window (CSIDevice_GCController::GetPadStatus), driven by the existing
vi_end_field counter. Working recipe:
`--pad-start-at=7300 --pad-start-frames=20 --fifo-record-after=7800 --fifo-record-frames=3`
(START ≥7300 reaches the save-blocks = ~3040 draws; 6800 was too early = settled title,
1258 draws). Replay the .dff through aurora (`SB_FIFO_REPLAY=...dff SB_SYNC_PIPELINES=1`) =
pixel oracle. Cached: `scratch/oracle/fifo/fsel_try_7300.dff`.
Native: `SB_STAGE=15 SB_PAD_SCRIPT="600:START 610:-" SB_DUMP_FRAME=... SB_DUMP_FRAME_AFTER=1400`.

## Real residuals (native vs oracle, both full-res) — the parity worklist

1. **Ocean sun-glare missing.** Oracle sea has bright white sun-glint/foam across the
   mid-distance water; native sea is a uniform saturated turquoise (no glare). = the
   reflective-sea sun-glint/additive-ripple path (TMapObjWave / TMirrorCamera / sea notes)
   not fully rendering here.
2. **Palm tree under-lit.** Native palm TRUNK is a dark near-black silhouette + fronds dark
   green; oracle trunk is light/textured + fronds lighter. Native palm is too dark
   (lighting or the trunk texture/material).
3. **A/B/C file cubes under-lit.** Native cubes are darker/dimmer brown; oracle cubes are
   brighter golden. Same darkening as the palm — likely a shared scene-lighting/ambient
   difference on the file-select 3D objects.

Not defects: file-block content (native all "New" = blank save is correct; oracle slot 1 =
shine×01 from Dolphin's memcard) and Mario anim-pose (mismatched capture instants).

## ✅ RESOLVED (2026-07-15): residuals 2+3 fixed — `setLight` was missing the scene-ambient set

**Root cause (the REAL one, after ruling out the material path below):** `TLightCommon::setLight`
(@US 0x80229a30) ends by applying the scene ambient to the ch0 ambient register:
`GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(idx))` (disasm tail @0x80229c64-0x80229c88 —
`getAmbColor` takes the RAW idx r30, NOT the doubled light-getter index gi=idx*2 r31). Our
native port OMITTED that call. And `setLight` STARTS with `ReInitializeGX()`, which sets
`GXSetChanAmbColor(GX_COLOR0A0, black)`. So every setLight left the ambient register at 0.

Why that darkened the scene: the file-select map objects (palm, A/B/C cubes, MapStaticObj)
are loaded WITHOUT `J3DMLF_MaterialColorLightOn` (literal game-data flags: `0x10210000`/
`0x10220000`/`0x10020000`, `MapStaticObject kMdlF_PE1`, `{"amenbo_model1.bmd",0x10210000}` —
identical on retail), so their color block is `J3DColorBlockLightOff`, whose `setAmbColor`/
`load` are NO-OPS: per-material ambient is faithfully DISCARDED. These lit LightOff materials
(chanctrl `light=1`) rely entirely on the GLOBAL ambient register — which `setLight` is
supposed to set to the scene ambient and didn't. Result: not-directly-lit faces (palm trunk,
shadowed cube faces) rendered BLACK.

**Fix:** appended the missing `GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(idx))` to
`TLightCommon::setLight` (reference/sms `MarioUtil/LightUtil.cpp`). `TLightMario::setLight`
delegates to the base, so both are fixed. This is NOT file-select-specific — it restores
correct ambient for EVERY lit J3D scene (title 3D, gameplay). Faithful RE completion.

**Verified (before→after→oracle, all headless):** the palm trunk went from a black silhouette
to light green/tan; the A/B/C cubes from dim to bright golden (letters visible); both now MATCH
the Dolphin oracle. amb-trace: native now emits `80808000` (0x80 ambient, 15521×) + `28282800`
(0x28, StaticMapObj SunOpa) on the TLightDrawBuffer/Mirror marks — exactly the oracle's
`808080ff`/`282828ff` RGB. Full-frame: 20.1% of pixels brightened (delta +48/+66/+37 RGB over
changed pixels); mean brightness moved to bracket the oracle (G 149→162 vs 159, B 188→195 vs 191).
Screenshots: scratch/shots/ambfix_before.png vs ambfix2_after.png vs amb_oracle.png.

### ⚠️ Remaining sub-residual (secondary, NOT visible-blocking): ambient ALPHA = 0 vs oracle 0xff

Native emits `80808000` (a=0) where the oracle has `808080ff` (a=0xff). RGB matches (the
visible fix); only the ambient ALPHA differs. Cause: `getAmbColor` scales `c.a *= mAlphaScale`
and the ctor leaves the scale field 0. TWO RE nuances found (unfixed, next step): (1) `getLightColor`
reads the alpha-scale from **0x1C** (`mAlphaScale`) but `getAmbColor` reads it from **0x18**
(`unk18`) — the port uses `mAlphaScale` for BOTH; and (2) both fields are 0 at file-select in
native, yet the oracle alpha is 0xff, so the correct scale field must be set to ~1.0 at runtime
by some setter our port doesn't run. Ambient-alpha only feeds lit ALPHA-channel draws (ambSrc=REG),
so it's not visibly blocking here — but it IS a faithfulness gap. Fix path: correct `getAmbColor`
to read 0x18, then find who sets that field (verify the full 0x80229cec disasm for r3-reassignment
first — the `lfs 0x18(r3)` may not be `this`-relative).

## (ORIGINAL, now superseded) ROOT CAUSE hypothesis: AMBIENT COLOR loads as 0

Dual per-draw state dump (SB_DRAW_DUMP, native file-select vs oracle .dff replay) nailed it.
The dominant lit-perspective draw signature:
- ORACLE: `ch0[light=1 matSrc=0 ambSrc=0 mat=(1,1,1,1) amb=(0.50,0.50,0.50) mask=03]`
- NATIVE: `ch0[light=1 matSrc=0 ambSrc=0 mat=(1,1,1,1) amb=(0.00,0.00,0.00) mask=03]`

The ONLY difference is the **ambient color: 0.5 (oracle) vs 0.0 (native)**. With lit,
matSrc/ambSrc=REG, output = mat×(ambient + Σlights); native's zero ambient makes every
not-directly-lit surface (palm trunk, shadowed cube faces) render BLACK — exactly the
symptom. This is almost certainly NOT file-select-specific — it's every lit J3D scene
(the title just has few lit-3D draws so it hid there). HIGH VALUE fix.

Narrowing (do NOT rush — a wrong swap here breaks ALL model loading):
- The material ambient comes from `J3DMaterial::load` (J3DMaterial.cpp:274-276 emits
  `mAmbColor[i].color` via J3DGDSetChanAmbColor), set from `J3DMaterialFactory::newAmbColor`
  (J3DMaterialFactory.cpp:242) = `mpAmbColor[initData->mAmbColorIdx[stage]]` (or the default).
- The default is `j3dDefaultAmbInfo = {0x32,0x32,0x32,0x32}` (≈0.2) — so native's 0.00 is NOT
  the default fallback; native reads `mpAmbColor[idx] == 0` where the oracle's is 0x80. Same
  BMD, so native's ambient LOAD is wrong: either the u16 `mAmbColorIdx` reads byte-swapped
  (BMD MAT3 is big-endian) → wrong entry, or `block.mpAmbColor` offset resolves wrong (LP64),
  or the MAT3 swapper (`readMaterial`) doesn't cover the ambient array/indices. `matColor`
  reads white in both, but that's the newMatColor default (0xFF), so it doesn't prove the
  index path works.
- NEXT DIAGNOSTIC (before fixing): for one file-select material, print native's
  `mpAmbColorNum`, the `mpAmbColor[]` entries, and `initData->mAmbColorIdx[0]` — compare to
  the raw BMD MAT3 bytes. That says definitively index-swap vs array-offset vs default-path.

### RULED OUT (2026-07-15, SB_AMB_DBG trace in newAmbColor — kept as an env-gated diagnostic)

The trace shows the material factory LOADS the ambient CORRECTLY: `mat=0 st=0
ambIdx=0x0000 -> arr=(80,80,80,32)` = 0x80 = 0.5 (ch0). And `J3DGDSetChanAmbColor`
(JRenderer.cpp:46) EMITS it correctly (packs r<<24|g<<16|b<<8|a → XF ambient reg; not a
stub). So the material load + emission are NOT the bug — the 0.5 is correct at creation.

⇒ The amb=0 at DRAW time is a DOWNSTREAM OVERRIDE: something re-sets GXSetChanAmbColor to 0
for the scene draw-buffers. The dominant amb=0 draw markers are `DrawBuf Mirror Opa`,
`<TLightDrawBuffer::Opa>`, `DrawBuf MapOpa`, `buf?` — i.e. the SCENE-LIGHTING / draw-buffer
path (TLightDrawBuffer / TLightWithDBSet / the map light setup), which ties into the existing
lighting-porting gaps ([[light-dbset-porting-gaps-2026-07-04]], calcLightBorder,
[[tlightwithdbsetmanager-bitmasks-corrected]]). Real game emits amb=0.5 there; native emits 0.

NEXT: trace ALL GXSetChanAmbColor callers (value + caller) during a file-select frame to find
WHO sets ambient=0 for the Mirror/Map/TLightDrawBuffer draws (add a caller trace in aurora's
GXSetChanAmbColor or the XF-ambient register write). Then port/fix that scene-ambient setup.
Do this fresh — it's in the TLightWithDBSet lighting arc, not a one-liner.

## Residual 1 (ocean sun-glare/foam) — CHARACTERIZED: the sea-mirror EFB composite (deferred arc)

The oracle's bright white sun-glint/foam across the mid-distance water is the **sea-mirror EFB
composite** (shader key hi32 `0xeb5c8e74`, drawbuf `DrawBuf MapXlu`), fully characterized in
`debug_journal/2026-07-03_water_sea_mirror_efb_composite.md`. In GC it samples a pre-copied
EFB→TEXTURE snapshot of the sea-mirror render and blends it (bm=1/4/2) dreamily over the water —
that IS the reflection/glare. Under `SMS_NATIVE_PLATFORM` we do not emulate GC EFB→TEXTURE copies,
so the bound texture arrived near-black, the TEV (`GX_CS_SCALE_2`) saturated to white, and the
composite painted the whole frame white — so it was deliberately DROPPED (skip batches with
`(shaderKey>>32)==0xeb5c8e74`). Dropping it is faithful given "paint pure white ≠ the RE'd intent",
but the COST is exactly this residual: flat turquoise water, no reflection highlight.

**To resolve it = implement the EFB-copy sea-mirror reflection pass** (render mirror view →
EFB→TEXTURE copy → rebind as the composite's tex → run the real `eb5c8e74` TEV/blend). This is
WITHIN the renderer doctrine ("understood modern equivalent acceptable at genuinely opaque HARDWARE
seams — EFB copy mechanics, XFB present"). It's a SUBSTANTIAL aurora arc, not a one-liner, and it's
the same missing machinery as the title's mirror-capture + logo-reflection EFB copies. This is the
next file-select parity step; start it with fresh context (orient on aurora's EFB-copy handling
first). Do NOT re-attempt the retired `SB_FS_COMPOSITE` segmented-render/snapshot_efb path (falsified
2026-07-03) or any endpoint-gradient hand-tune of the sea (violates no-hand-tuning).

### RE-SCOPE (2026-07-15, orientation pass — the arc is SMALLER than "implement EFB copies")

Three findings that narrow it:
1. **Aurora ALREADY has EFB-copy machinery** — `GXCopyTex`, `GXSetTexCopySrc/Dst`, and a working
   `copy_tex()` resolve/cache (`extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp:128` — "Pass 1 (source)
   → GXCopyTex(clear) → Pass 2 samples the copy"). NOT a from-scratch EFB implementation.
2. **The 2026-07-03 `SB_SKIP_KEY` / `native/render/sms_boot_present.cpp` composite-drop is RETIRED**
   (gone with the one-runtime consolidation — that file no longer exists). The `eb5c8e74` sea-mask
   composite now flows through the NORMAL draw-buffer path ("sea-MASK material c97c48, key eb5c8e74"
   in `JDRDrawBufObj.cpp:113` / `J3DDrawBuffer.cpp:530`). So it is no longer force-dropped — it draws,
   sampling whatever texture is bound.
3. ⇒ **Real next diagnostic:** at native file-select, dump the `eb5c8e74` draw's bound tex0 (a live
   sea-mirror snapshot, or still near-black?) and check whether the sea-MIRROR render pass + its
   `GXCopyTex` run and populate that texture. If they run and copy_tex populates it → the composite
   should already show reflection (chase why it doesn't). If not → wire the mirror pass (TMirrorCamera
   / mirror draw-buffer → GXCopyTex into the composite's sampled texture). Tools: SB_BATCH_DBG,
   per-draw tex-id + draw-dump. Live render-debugging — best fresh.

- For a clean quantified pixel diff, first fix the capture-height mismatch (dump native at
  448 or the oracle at 480) — the 960-vs-896 misalignment inflated the earlier 67% number.
