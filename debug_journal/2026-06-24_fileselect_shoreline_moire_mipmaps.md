# File-select shoreline MOIRE fixed — generate mip chains + GC-faithful min/mag filter split (2026-06-24)

## Symptom
The file-select beach/shoreline band (between the teal sea and the tan beach) rendered with
harsh fine DIAGONAL moire — see `scratch/frames/_old_baseline/boot_0400.ppm`. The GX oracle
(`scratch/oracle/fileselect_gx_oracle.png`, the Dolphin-GX ground truth) shows that band SMOOTH,
so the moire is a real renderer bug, not faithful behavior.

## Root cause (named)
Two-part:
1. **The renderer reused `magFilter` for BOTH magnification and minification.** `sb_resolve_textures`
   set `linear = (t->magFilter == 1)` and that single flag picked the sampler (NEAREST vs LINEAR)
   for min *and* mag. GC tracks the **minification** filter separately (`ResTIMG.minFilter`, which
   also encodes the mip mode) and the renderer ignored it.
2. **No mip chain + heavily-tiled grazing surface.** Measured via `SB_J3D_DBG` texres dump (now
   prints `minF/magF/mipEn/aniso`): EVERY file-select texture is authored `minF=1` (GX_LINEAR),
   `mipEn=0`, `mipmapCount=1` — i.e. bilinear with NO mipmaps. The shoreline (batch b30) tiles its
   texture 20..51× across a near-horizon plane → many texels per pixel → textbook
   bilinear-without-mips minification moire. (The earlier handoff said "sampled NEAREST"; the real
   value is GX_LINEAR — but the no-mip part was the operative cause.)

## Fix (PC-native renderer, no bandaid)
`native/render/nvk.cpp` + the texture-plumbing path:
- **`makeTexture` now generates a full mip chain** (compute `mipLevels`, add `TRANSFER_SRC` usage,
  upload base level, then successive 2× `vkCmdBlitImage(VK_FILTER_LINEAR)` downscales; per-level
  layout barriers; view exposes all levels).
- **Per-texture sampler cache (`Impl::getSampler`)** keyed by the GC filter/wrap descriptor — splits
  MAG (`magFilter`) from MIN (`minFilter`), maps the GX min-filter enum to Vulkan min/mip modes,
  and applies GC wrap modes (CLAMP/REPEAT/MIRROR) which were previously hardcoded REPEAT.
- **Anti-aliasing policy:** since SMS authors these surfaces GX_LINEAR-no-mips, a literal port would
  still alias. We generate the mip chain ourselves and treat GX_LINEAR min as **trilinear**
  (`useMips = minFilter != 0`, `mipLinear` for the linear modes). Point (GX_NEAR) stays crisp/no-mip
  so HUD glyphs / pixel-art are unaffected. Anisotropy is wired (device feature enabled,
  `getSampler` honors `ResTIMG.maxAnisotropy`) but only engages when the texture requests it.
- Filter/wrap/aniso now flow `SbTexImage` → `NvkTevBatch::Tex` → `getSampler`
  (`sms_boot_material.{h,cpp}`, `sms_boot_j3d_capture.cpp`, `nvk.h`).

## Verification (TOOLING-FIRST, measured — not eyeballed)
The shore foam animates, so same-frame-number A/B across runs is phase-noisy; measured **high-
frequency (Laplacian) energy** in the shoreline band (y 290..400, x 0..460) instead:
- baseline (no mips):      **lap_energy 128.7**
- trilinear mips (this fix): **lap_energy 53.5  → 58% reduction**; sea + beach come out smooth,
  HUD/blocks/OPTIONS text stay crisp.
- forced 8× anisotropy floor: 61.3 (NO improvement over trilinear here, `band_std` identical →
  inert on this scene) → **reverted** to GC-faithful aniso. Kept only the proven trilinear win.
No Vulkan validation errors; run completes, no crash.

## Residual (separate artifact, NOT this bug)
Thick LOW-frequency white DIAGONAL streaks remain in the shoreline band (they survive the HF
reduction, so they are not minification aliasing). The oracle shows gentle ~horizontal shore foam;
ours is diagonal — likely a foam-texture UV orientation / multi-layer blend issue on that surface.
Out of scope for the moire fix; next investigator can chase it with `SB_BATCH_DBG` on b30.

## Reusable
- `SB_J3D_DBG` texres dump now reports `minF/magF/mipEn/aniso` per texture — use it to read GC
  filter intent before touching sampling.
- The shoreline-band Laplacian metric (`scratchpad` one-liner) is the moire verification number.
- ⚠ The sms-boot stderr log contains binary bytes → use `grep -a` (plain `grep` silently treats it
  as binary and prints nothing).
