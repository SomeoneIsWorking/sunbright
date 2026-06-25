# File-select: GX texgen matrices ported + sea-stripe root cause CORRECTED (2026-06-25)

## Win — the capture dropped GX texgen matrices entirely (FIXED)
`native/render/sms_boot_j3d_capture.cpp` built each vertex's UVs as a **pass-through of the raw
vertex texcoord attribute** (`tv.uv[t] = s.tex[t]`) — the GX texgen stage (`J3DTexGenBlock`:
per-texgen source + `J3DTexMtx` matrix) was never applied. Any material whose texgen binds a
non-identity `J3DTexMtx` (scrolling/scaling water, scaled signs) rendered with the WRONG texture
coordinates.

FIX (mirrors the proven main-build `runtime/overrides/ngx_j3d_shape.cpp` `texgen_uv`): capture the
texgen block into `MatEntry` (per slot: type/src/mtxsel + the material's OWN `mTotalMtx`@+0x64 — the
matrix J3D bakes into the per-frame DL replay, animated materials re-patch it each frame) and apply
it per-vertex: TEXn input = (s,t,1,1); MTX2x4 → s'=row0·in, t'=row1·in; MTX3x4 → divide by row2·in;
SRTG/COLOR src → the lit colour channel. **Identity-safe**: 33/37 file-select materials have an
identity `mTotalMtx` → no-op (== old pass-through); only the foam (key eb5c8e74, tg0 scale 1.2 +
bias 0.815, both sourcing TEX0) and ~3 others (a 4× and a 2× scaled material) now transform. Verified
the foam UV shifted 20.84→25.83 exactly per the 1.2×+0.815 matrix; full frame unregressed.

This is a general correctness fix, committed regardless of the sea-stripe outcome below.

## CORRECTION — the diagonal sea stripes are NOT the foam overlay b30 (handoff was WRONG)
The prior handoff/journal (`2026-06-25_fileselect_sea_foam_stripes_characterized.md`) claimed the
diagonal teal/white sea stripes are the foam overlay **b30** (key eb5c8e74). **REFUTED**: skipping
b30 entirely (new `SB_SKIP_KEY=eb5c8e74`) leaves the diagonal stripes intact (they only change
hue). The stripe source is the **base sea b25** (key 224004d9, rgb 0.49,0.82,0.78 teal, opaque
bm=0/1/0), whose texture is tiled **~32× in u, ~60× in v** (UV[17.06,49.01;5.48,65.83]) across a
plane receding to the horizon (z 0.988→0.99988). At the grazing horizon that heavy tiling minifies
to many texels/pixel → textbook minification aliasing → the diagonal moiré.

## What it IS and ISN'T (measured, value-first)
- **IS minification aliasing**: `SB_LOD_BIAS=4` (new mip-LOD-bias knob) makes the stripes vanish
  (the sea over-blurs to its average) — proving the stripes are too-low-mip-LOD aliasing of the
  tiled sea, not blend/TEV/texture-content.
- **b25 is mipped already** (minF=5 = GX_LIN_MIP_LIN; the batch dump now prints minF/mag/wrap), so
  it is NOT a missing-mip-chain problem (the shoreline-moire class). Our trilinear auto-LOD simply
  under-selects the mip at the grazing horizon.
- **Anisotropic filtering does NOT fix it** (`SB_FORCE_ANISO=16`): it makes the stripes SHARPER, not
  smoother (aniso keeps detail along the long/view axis, revealing the tiling). So AF is the wrong
  tool here; the handoff's earlier "aniso = no improvement" finding generalizes.
- **It is fundamentally the native-resolution gap vs the oracle.** The GX oracle renders at 1280×956;
  at that pixel density the horizon band has many more pixels, each spanning fewer tiles → far less
  minification → smooth. sms-boot renders the same sea at 640×480, where the thin horizon band
  minifies extremely and aliases. A global LOD bias over-blurs the whole scene (cubes too); the
  clean ways to match the oracle are (a) render sms-boot at a higher internal resolution, or (b) a
  TARGETED higher LOD bias for the grazing sea material only — both are tuning, not a faithfulness
  bug, and historically thrash-prone (CLAUDE.md). Deferred as such, NOT a TEV/blend defect.

## Foam (b30) TEV — characterized for the record (not the stripes)
b30 is a faithful additive foam overlay: 3-stage TEV, output ≈ white, blend bm=1/4/2
(src=SRCALPHA dst=SRCCLR) → `out = o.a + sea` (adds white by the computed alpha). Both its alpha
textures (dumped via the new per-batch all-texture dump `btex_<bi>_t<ti>_{rgb,a}.ppm`) are very
dark/sparse (alpha ~0 with faint wisps), so its real contribution is subtle — correct.

## New tooling (committed, env-gated, off by default)
- `SB_TEXGEN_DBG` — per-material texgen dump (count, per-slot type/src/mtx + mTotalMtx).
- `SB_MAT_DUMP_ALL` — write every material's generated TEV fragment to `scratch/frames/mat_<key>.txt`
  (the first-3 `mat_glsl` dump missed the foam).
- `SB_SKIP_KEY=<hex32>` — drop scene batches whose shaderKey high-32 matches (artifact bisection).
- `SB_FORCE_ANISO=N` / `SB_LOD_BIAS=f` — sampler experiments (proved the stripe mechanism above).
- batch dump (`SB_BATCH_DBG`) now prints `minF/mag/wrap` per tex0 and dumps ALL bound textures
  (`btex_<bi>_t<ti>_*`), not just tex0.
- ⚠ batch dump only fires when a dump is active: pair `SB_BATCH_DBG=N` with `SB_SEL_DUMP=1`
  (present_hook returns early on non-dumping frames before the batch block).
