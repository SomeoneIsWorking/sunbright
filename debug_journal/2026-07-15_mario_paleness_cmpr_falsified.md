# 2026-07-15 — Mario "paleness": CMPR-texture conclusion FALSIFIED; localized to Mario-only render eval

## ✅✅ RESOLVED — root cause: aurora's XF chanctrl attnFn decode had bit9/bit10 SWAPPED

The wash was **aurora misdecoding the GX channel-control attenuation function**. Real GC/decomp
encoding (`reference/sms/src/dolphin/gx/GXLight.c` GXSetChanCtrl, cross-checked vs Dolphin
`XFMemory.h` `attnfunc` `BitField<9,2>` {None,Spec,Dir,Spot}):
- **bit 9  = (attn_fn != GX_AF_NONE)**
- **bit 10 = (attn_fn != GX_AF_SPEC)**

aurora's decode (`command_processor.cpp`) tested `!bit10 → NONE` first, i.e. it had bit9/bit10
swapped, so a **SPEC** channel (bit9=1, bit10=0) was read as **NONE**. Under NONE, aurora forces
`attn=1, diff=1` and adds the light's FULL color as a constant; under SPEC it computes an
attenuated specular highlight (mostly dim). Mario's **COLOR1** is a `GX_AF_SPEC` highlight light
(L2, white); TEV **stage 4** (`chan=5`=COLOR1A1) pulls COLOR1 RASC into the combine, so the
phantom full-white add blew him toward white. Localized by `SB_TEV_STOP`: overalls stay mid-tone
through stages 0–3, jump to blown-white at stage 4.

**Fix** (aurora): correct the decode to the real hardware convention, and fix the matching
`GXLighting.cpp` encode (bit9/bit10 + the `diffFn` zero-condition, which the decomp keys on
`attn==SPEC`, not `==NONE`) so encode↔decode round-trip and match hardware. General fix — affects
EVERY object using GX_AF_SPEC or GX_AF_NONE, not just Mario.

**Verified**: replay overalls **[127,164,195] (washed) → [10,69,175]**, oracle **[19,68,169]** —
near-exact. Side-by-side `scratch/texcmp/fix_side.png`: aurora Mario now matches the oracle
(deep red hat, saturated blue overalls, brown shoes, proper shading). `dd-ch1` now shows
`attnFn=0` (SPEC) for Mario. No regression: title replay still faithful (mean [139.6,174.4,200.6]
vs boot-oracle ~[143,178,204]); whole-frame replay meanABS 27.96→27.37.

Note: the remaining left-vs-right background difference in `fix_side.png` is the SEPARATE, known
water-speckle REPLAY-ONLY artifact — not Mario, not this bug.

### ⚠️ SCOPE CORRECTION — the fix is verified on the REPLAY, but LIVE-NATIVE Mario is UNCHANGED

Re-captured live-native file-select with the fixed binary; Mario is PARTIALLY improved but not at
parity. A live-native draw-dump (`SB_DRAW_DUMP_FRAME` at the settled frame; the boot log is binary
to grep — use `grep -a` for the Shift-JIS perform-list names) shows the fix DID apply —
**live-native ch1 now decodes attnFn=0 (SPEC)**, same as retail. In the settled crouch capture the
overalls **shoulder straps are now correctly deep blue** (the fix helped), but the **bib/body reads
pale/white** (0 deep-blue px in a tight crop) with dark blotches.

### ✅ LIVE-NATIVE ROOT CAUSE FOUND (measured from oracle): L2 specular attenuation not set

Compared the port's live-native Mario draw config against the retail `.dff` replay (both now go
through the same fixed renderer, so any output diff = a GX-STATE diff the port's game logic
produces). Ruled OUT ambient: the amb=1.0 draws are the MIRROR pass (`mark='DrawBuf Mirror Opa'`),
the MAIN-scene Mario (`mark='buf?'`) is **amb=0.5, same as retail** — ambient is NOT the cause. The
real divergence is **light L2's attenuation** (L2 = Mario's COLOR1 GX_AF_SPEC highlight light):
- **retail L2**: `cosAtt=(0,0,1) distAtt=(25,0,-24)` — exactly `GXInitLightShininess(50)` (specular).
- **live-native L2**: `cosAtt=(1,0,0) distAtt=(1,0,0)` — constant attenuation → SPEC attn evaluates
  to ~1 (full) → L2 adds full white → washes Mario. (Same end-symptom as the decode bug, now the
  cause is the port not setting L2's specular attenuation.)

Located: `reference/sms/src/MarioUtil/LightUtil.cpp:277-281` (TLight setLight, GX_LIGHT2 block):
```
GXInitSpecularDir(&obj, -nrm.x, -nrm.y, -nrm.z);   // 277
GXInitLightColor(&obj, col);
GXInitLightAttn(&obj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);  // 281 — CONSTANT; overwrites specular
```
Line 281 sets constant attn where retail sets the shininess-50 specular coefficients. The FIX is to
give L2 the specular attenuation (`GXInitLightAttn(&obj, 0,0,1, 25,0,-24)` = `GXInitLightShininess`
50, matching the oracle).

**De-risked (tick 3, read-only RE — all confirmed):**
- **Path confirmed.** `TLightMario::setLight` @0x80229610 is a byte-identical twin of
  `TLightCommon::setLight` @0x80229a30 (CodeWarrior emitted the same body for both polymorphic
  overrides); the port dispatches Mario's setLight to the base (LightUtil.cpp:390→392). So the
  L2 block at :270-283 IS Mario's light-2 setup. (Funcs: both in reference/sms_gmse01_funcs.txt.)
- **`GXInitSpecularDir` sets ONLY `ldir`/`lpos`, NOT cosAtt/distAtt** (reference/sms GXLight.c —
  verified). So L2's attenuation comes ENTIRELY from the `GXInitLightAttn` at :281. Retail's
  L2 `distAtt=(25,0,-24)` is too specific to come from anywhere else → line 281's args are the bug.
- Upstream doldecomp/sms is NOT locally comparable (only the commit object is fetched, tree=0
  files); a full `git fetch upstream` would be needed to diff — deliberate sync, not done here.

**Remaining before the fix (deliberate work, NOT autonomous):** disasm 0x80229a30's L2 block in
Ghidra to confirm whether retail calls `GXInitLightAttn(0,0,1,25,0,-24)` inline (→ line 281 is a
constant-transcription bug, fix in place) OR `GXInitLightShininess(&obj, 0.5)` as a separate call
(→ port the shininess call for byte-faithfulness). Then edit, and re-capture live-native to verify
overalls → deep blue. Do NOT claim the shipped Mario is fixed until that passes — only the aurora
decode bug is fixed + replay-verified so far.

---
_(original investigation notes below — kept; the "differing TEV input" NEXT was correct: it was
COLOR1's specular light being misdecoded)_



Follow-up to the same-day commits `4f67166`/`265b920`, which concluded the Mario overalls
paleness was "narrowed to CMPR texture pixels → GC big-endian CMPR load bug." **That
conclusion is FALSIFIED here** — verified, not argued. This entry records what is actually
proven, the aligned instrument that finally made measurements trustworthy, and the real
remaining lead.

## The measurement-discipline reset

Every paleness ratio in the history (the "1.24×", "2× diffuse", etc.) came from **misaligned**
side-by-sides: the matched shot `replay_matched.png` is a 1286-wide odd split; the native
aurora replay renders **1280×896** (2× of the 640×448 EFB) while the Dolphin playback dump is
**640×480** (VI-scaled 448→480). Naive-resizing one onto the other (esp. 896→480) lightens
Mario and contaminates every crop — the same failure mode already retracted once. **No ratio
is trustworthy without pixel alignment.**

### The instrument (kept): `tools/render/replay_diff.py`

Renders are only cleanly comparable through the **matched .dff replay** (aurora vs Dolphin
FIFO-player playback of the *same* dff = identical recorded GX stream → pixel-aligned modulo
resolution). The tool: 2×-box-downscales the aurora 1280×896 dump → 640×448, resamples the
Dolphin 640×480 → 640×448, emits `_aur.png`/`_dol.png`/`_heat.png` + a per-region median
table. **Static geometry near-0 in the heatmap == alignment is trustworthy; only then are the
color numbers meaningful.** Inputs: `repaligned.rgba` (aurora `SB_DUMP_FRAME` of
`fsel_try_7300.dff`, `SB_DUMP_FRAME_AFTER=2` since the replay is only 3 frames) vs
`scratch/shots/dolphin_fsel_true.png`.

## PROVEN (mechanical, this session)

1. **Textures decode byte-identically.** Dumped every fmt5(RGB5A3)/fmt14(CMPR) texture from
   native-BOOT (`SB_TEX_DUMP`, cap 4000) and from the replay; content-hash / MAD comparison:
   **every native texture matches a replay texture at MAD 0.00.** The CMPR-load-byteswap
   hypothesis is dead — native and the disc-recorded dff feed aurora's CMPR decoder the same
   bytes. (Also: the one distinct 64×128 CMPR texture is a scene PROP (chrome sphere/screw),
   not overalls — the "64×128 overalls" identification in `4f67166` was wrong.)

2. **Paleness is REAL and Mario-specific (aligned, same-pixel).** In the geometry-aligned
   images the heatmap difference is edge-only (sub-pixel AA/resample) everywhere EXCEPT
   Mario + the water. Per-region medians (aur vs dol): **sky [1.0,1.0,1.0]**, sand
   [1.02,0.99,1.01], palm-trunk [0.98,0.97,0.96], cubeA [1.02,0.97,0.96] — all match. Mario
   overalls (blue-in-both mask) aur **[127,164,195]** vs dol **[69,118,173]**; Mario blows
   toward white. ⇒ NOT global gamma/color-space (sky is exactly 1.0). Mario-only.

3. **Lighting eval is equivalent to Dolphin.** Mario config (from the extended draw-dump):
   `attnFn=1 (GX_AF_SPOT), diffFn=1 (GX_DF_SIGN), mask=03 (L0+L1)`, amb=(0.5,0.5,0.5),
   matColor white. Lights: **L0** white cosAtt=(1,0,0)/distAtt=(1,0,0) → attn=1 constant;
   **L1** white cosAtt/distAtt all-zero = the dead light. aurora `lighting_func`
   (shader.cpp:820) vs Dolphin `LightingShaderGen.cpp` are line-for-line equivalent for this
   config. The dead-light guard (`select(0, cos_attn/dist_attn, dist_attn>0)`) DOES fire for
   L1 → L1 contributes 0. The WGSL `Light` struct (vec3f fields) lands at offsets 0/16/32/48/64
   = exactly the C++ `Vec4`-per-field 80-byte layout, so `dist_att` reads (0,0,0) for L1 — **no
   std140 misalignment**. Decisive corroboration: the **lit palm (mask=01, L0-only, CLAMP)
   matches the oracle perfectly** → aurora's single-light lighting is correct.

4. **TEV op codegen correct; all texmaps bound.** Mario body material = tev=5 with compare-op
   (op=10 COMP_GR16_GT) + subhalf-bias (bias=2) at the RASC-modulating stages, sampling 4
   texmaps (256²CMPR + 64²/8²/32² fmt0), **all `hasTex=1`** (no empty slot). aurora's
   `tev_op` (shader.cpp:378) implements add/sub/all 8 compare ops/bias/scale/clamp per GC spec.

5. **RASC (aurora's lit COLOR0 channel, via new `SB_RASC_VIZ`) for Mario is near-white**
   (overalls [200,200,206], hat [254,255,255]). Consistent with white lights × white matColor;
   Dolphin clamps the same way, so RASC is not by-itself the divergence.

## Where that leaves it (the real lead)

Textures decode identically, lighting is equivalent, TEV ops are correct, texmaps are bound,
RASC ≈ same → the only remaining variable is the **per-pixel texture SAMPLE VALUE** (which
texel/mip/filter), not the decode or the combine math. Several Mario textures use
**minFilt=5 (GX_LIN_MIP_LIN, trilinear mips)**. The file-select **water speckle is a proven
REPLAY-ONLY mip artifact** (live-native water matched true Dolphin; the dff's upper-mip data is
garbage/absent and aurora's LOD over-selects). **All the Mario paleness measurements above use
the REPLAY** → the leading hypothesis is that Mario's wash is the SAME replay-oracle mip
artifact, i.e. NOT a shipped-game bug. (Caveat: a uniform wash fits mip-blur less cleanly than
the water's speckle, and aurora's 2× render would select *finer* mips — so this is a lead, not
a conclusion.)

## DONE — live-native settles it: the wash is a REAL shipped-game bug (mip/LOD FALSIFIED)

Captured live-native file-select with Mario via `SB_STAGE=15` + `SB_PAD_SCRIPT="600:START
610:- 900:START 910:-"` (title→save-select camera pan), `SB_TURBO=1`, dump at present 1600
(`scratch/texcmp/livenative2_full.png`). Result:
- **Water is smooth turquoise, NO speckle** → confirms the water speckle IS replay-only.
- **Mario is STILL washed** (pale-pink hat, near-white overalls) in live-native → the paleness
  is a REAL render bug present in BOTH aurora paths (replay + live-native), while Dolphin
  renders him correctly. The "replay-mip-artifact" hypothesis above is **FALSIFIED**. (Verify,
  don't assume — this is why.)

Then **mip/LOD sampling FALSIFIED directly**: new `SB_FORCE_LOD0=1` (clamp every sampler to
LOD 0, `gx.cpp` TextureBind::get_descriptor) leaves the replay overalls at [127,164,193] —
unchanged from [127,164,195]. Mario's wash is NOT mip over-selection.

## Rule-out ledger (all VERIFIED this session — do NOT re-open)
texture decode · lighting eval · TEV op codegen · mip/LOD sampling · global gamma. All match
the oracle / are equivalent to Dolphin. The lit palm (mask=01) matches; RASC ≈ white in both.

## NEXT (narrowed): a differing TEV *input*, Mario-specific
The wash is real, Mario-only, in aurora's per-draw eval, and NOT decode/lighting/TEV-ops/mip.
Remaining Mario-specific variables that can shift the color: (a) the alpha-**blend** bm=1
bf=4/5 (SRCALPHA/INVSRCALPHA) — if aurora's Mario output ALPHA differs from Dolphin, he blends
with the bright background (note: a naive blend-over-sand/sky does NOT fit — the measured B
channel goes UP 173→195, so not a plain background bleed); (b) **KONST selection** (kcSel/kaSel
→ wrong konst reg or rgb-vs-aaa); (c) **TEV swap tables** (tevSwapRas/tevSwapTex); (d)
**texcoord gen** (samples a different texel). Concrete first step: `SB_TEV_STOP=0..4` on the
ACTUAL on-screen Mario draw (prior tevstop shots were mis-cropped background) via the aligned
instrument to find the stage where the wash emerges, then audit that stage's konst/swap/inputs
against the GC spec. Also worth a direct check: dump Mario's output ALPHA (framebuffer A or a
TEV-alpha viz) vs what the blend expects.

## Tooling added (kept, gated)
- draw-dump: `attnFn`/`diffFn` on the ch0 line; `[tex]` now reports **all 8 texmaps**
  (`hasTex`/fmt/dims) not just texMap0.
- `SB_RASC_VIZ`: outputs the lit COLOR0 channel (`in.cc0`) as the fragment color — splits
  "lighting wrong" from "TEV wrong".
- `tools/render/replay_diff.py`: the pixel-aligned replay-vs-oracle diff instrument above.

Do NOT re-open: CMPR/texture-byte load (proven identical), lighting eval (proven equivalent),
TEV op codegen (correct), ambient/matColor/konst (match). Supersedes the CMPR conclusion in
`4f67166`/`265b920`.
