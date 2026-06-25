# File-select sky "light rays" — the material pipeline is FAITHFUL; cause is geometry, not blend (2026-06-25)

## Symptom
On the settled file-select (`scratch/frames/suns_fixed.png`) the sky shows ~4 bright vertical
white light-shaft columns. The GX oracle (`scratch/oracle/fileselect_gx_oracle.png`) has NONE.
The handoff flagged this (residual #3) as the prime "sky additive wash" suspect and repeatedly
warned it is thrash-prone (multi-layer blend, no-eyeball rule).

## What was RULED OUT this session (all by measured VALUES, new tool `SB_FOG_DBG`)
The ray batch is `bm=1/4/1` = GX_BM_BLEND src=GX_BL_SRCALPHA dst=GX_BL_ONE (additive), drawn by
`drive_sky()` (空グループ flag 0x20E → sky.bmd MActor entry). Per-material dump (`SB_FOG_DBG=1`,
`native/render/sms_boot_material.cpp`) for the three `1/4/1` sky materials:

```
blend=1/4/1 fogType=0 nstg=1 chan0Ctrl=0x700 matC0=255,255,255,255 s0.cenv=0x08f8af s0.aenv=0x08f2f0
blend=1/4/1 fogType=0 nstg=2 chan0Ctrl=0x700 matC0=255,255,255,255 s0.cenv=0x00f8af s0.aenv=0x00f2f0
blend=1/4/1 fogType=0 nstg=2 chan0Ctrl=0x700 matC0=136,129,156,255 ...
```

1. **NOT fog.** `fogType=0` (GX_FOG_NONE) on every additive ray material. Our renderer implements
   no fog, but these materials carry none either, so fog is not why the real game's rays are
   invisible. (Verified BEFORE implementing fog — saved building it for nothing.)
   NOTE: a *different* material — `blend=1/4/5 fogType=2`, range startZ=199999 endZ=200000
   nearZ=10 farZ=300000 color=255,0,128 — IS fog-enabled (GX_FOG_PERSP_LIN). We render it without
   fog. Its band sits at z≈200000 (beyond the ~100000 sky scale) so it's effectively a no-op haze
   here, but it is a real future fog task (separate from the rays).
2. **NOT the combiner.** `cenv=0x08f8af` decodes (per `runtime/render/tev_shader.cpp` decode_cc/ac:
   a=(>>12)&f, b=(>>8)&f, c=(>>4)&f, d=&f, bias=(>>16)&3, op=(>>18)&1, clamp=(>>19)&1) to:
   color = ZERO + lerp(ZERO, TEXC, RASC) = **TEXC × RASC**; alpha `aenv=0x08f2f0` =
   **TEXA × RASA**. That is textbook `GX_MODULATE`, decoded correctly (matches the
   `sb_modulate`/render_test golden).
3. **NOT matColor.** chan0Ctrl=0x700 → bit0 (matSrc) = 0 = GX_SRC_REG → raster CLR0 = matColor =
   (255,255,255,255) white opaque. Faithful (GX reads REG → the same white).
4. **NOT texture decode.** The 8×8 sky tex is `fmt=0x0` = GX_TF_I4 (4-bit intensity; GX
   replicates intensity to RGB **and** alpha), mipEn=0, minF/magF=1 (GX_LINEAR). Decoded content
   (`scratch/frames/btex_03_combo.png`) is a blocky black/white intensity pattern with RGB==alpha,
   exactly as I4 should decode. Bright cells → white rgb + high alpha → bright additive shaft.
5. **NOT the wrong material table.** The capture reads `j3dSys.getMatPacket()->getMaterial()`
   (`sms_boot_j3d_capture.cpp:300`) — i.e. the PER-PACKET material that J3DMatPacket::draw set,
   which IS the `setMaterialTable`-swapped material for a shared-table model (the same per-packet
   discipline as the beach-texture fix). So we are reading the same material GX drew with.

## Conclusion
Every stage of the ray draw — per-packet (swapped) material, white REG matColor, GX_MODULATE
combiner, I4 bright texture, additive SRCALPHA/ONE blend, no fog — is FAITHFUL to GX. A faithful
pipeline drawing the same geometry would make GX show the same shafts. Since the oracle does NOT,
the divergence is NOT in the material/blend/texture path. It is one of:
- **(geometry/positioning)** the 6 ray billboards (vc=24) are positioned/oriented by
  `TSky::perform`'s `setBaseTRMtx` (Map/Sky.cpp:19-58, camera-inverse + map-15 Y-rotation by
  `unk48`, seeded -30°, +0.035°/frame). If our camera-inverse or rotation phase differs, the
  billboards face the camera as full-height shafts where the real game has them edge-on / swept to
  the horizon (the oracle's faint upper-left wisps are the likely "settled" position of this same
  geometry). Our drive_sky reproduces the rotation but the exact `gpCamera->unk1EC` inverse and the
  per-frame phase are unverified against the oracle.
- **(over-entry)** drive_sky enters sky.bmd with flag 0x20E; if the real file-select perform list
  enters the sky group without the billboard shapes (or with a `gpMapObjManager->unk68` sky
  material override that hides them — Map/Sky.cpp:99-103), our forced entry over-draws.

## DO NOT re-chase (this session ruled these out with values)
fog, the TEV combiner decode, matColor/raster source, I4 texture decode, the per-packet material
table. The next attempt must target sky-billboard GEOMETRY: dump the 6 ray-quad WORLD positions +
the `setBaseTRMtx` our drive_sky builds vs the oracle's, and check `gpMapObjManager->unk68` (is a
map-15 sky material override present and does it hide these shapes?). Value-first; never eyeball
the wash.

## New tooling (committed)
`SB_FOG_DBG=1` — one-shot per-material dump in `sb_build_tev_state`: blend mode, J3D fog block
(type/start/end/near/far Z/color/adjEn), num TEV stages, chan0 ctrl + matColor, and stage0
texmap/colorchan/color_env/alpha_env. The value-proof for "is a material fogged / what is its
stage0 combiner" without a rebuild per question.
