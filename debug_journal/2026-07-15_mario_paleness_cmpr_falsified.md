# 2026-07-15 — Mario "paleness": CMPR-texture conclusion FALSIFIED; localized to Mario-only render eval

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

## NEXT (decisive, not yet done)

Capture **live-native** file-select with Mario settled and compare his overalls to the oracle.
`SB_STAGE=15` boots to the TITLE view (renders faithfully); reaching the Mario save-select view
needs a pad-driven START to pan the camera (`SB_PAD_SCRIPT` with START presses; Mario settle ≈
`MARIO_STATUS_SLEEP`). If live-native overalls are deep-blue ≈ oracle → the replay-wash is an
oracle artifact and file-select is faithful (close it like the water). If live-native is washed
→ real bug, and the cause is aurora's texture SAMPLING for Mario (mip LOD / lod_bias), the same
axis as the open water-speckle item — attack that, not textures/lighting/TEV (all ruled out).

## Tooling added (kept, gated)
- draw-dump: `attnFn`/`diffFn` on the ch0 line; `[tex]` now reports **all 8 texmaps**
  (`hasTex`/fmt/dims) not just texMap0.
- `SB_RASC_VIZ`: outputs the lit COLOR0 channel (`in.cc0`) as the fragment color — splits
  "lighting wrong" from "TEV wrong".
- `tools/render/replay_diff.py`: the pixel-aligned replay-vs-oracle diff instrument above.

Do NOT re-open: CMPR/texture-byte load (proven identical), lighting eval (proven equivalent),
TEV op codegen (correct), ambient/matColor/konst (match). Supersedes the CMPR conclusion in
`4f67166`/`265b920`.
