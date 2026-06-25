# File-select diagonal sea stripes — localized to the foam overlay b30 (2026-06-25, characterized, NOT yet fixed)

## Symptom
The file-select sea shows diagonal teal/white stripes; the GX oracle's sea is smooth teal.

## Localization (SB_BATCH_DBG=-1, the new camera-settle-gated batch dump; scratch/frames/seabatch.log)
- **b25** = the base sea: rgb=0.49,0.82,0.78 (correct teal), bm=0/1/0 opaque, 256x256 tex, UV
  u[17.06,49.01] v[5.48,65.83]. CORRECT — do not touch (handoff confirmed).
- **b30** = the stripe source (key eb5c8e74): a full-width white overlay over the sea, ndcY[0.251,1.0],
  bm=1/4/2 (src=GX_BL_SRCALPHA, dst=GX_BL_SRCCLR — the blend IS faithfully mapped in nvk
  gx_blend_factor: case 2 dst → VK_BLEND_FACTOR_SRC_COLOR, verified), ntex=2, 256x256, with ASYMMETRIC
  UV tiling u[20.84,51.64] (≈31×) v[0.00,7.76] (≈8×). Drawn by the scene's own perform(0x8) (faithful
  path, not a forced drive).

## Key finding — the foam TEXTURE is dark/sparse, so the stripes are aliasing or UV, not texture content
Dumped b30's 256x256 tex0 (btex auto-dump cap raised 64→256; scratch/frames/btex_30_{rgb,a}.ppm.png):
it is a DARK, sparse water/foam/sparkle pattern — mostly black with faint specular specks, RGB≈alpha.
Under bm=1/4/2 the blend is out = src·srcα + dst·src.rgb, so a sparse dark texture yields ~black where
dark and white-saturated where it specks. Tiled 31×/8× across the sea and minified, those sparse
specks band into the diagonal black/white streaks.

## Two candidate causes (NEXT session — value-first, decide between them)
1. **Minification aliasing** — b30's texture may not be mipmapped. The shoreline-moire fix only mips
   textures whose GC `minFilter` requests it (LIN_MIP_LIN). Check b30's 256x256 tex minF/magF/mipEn
   via SB_J3D_DBG; if it's GX_LINEAR (no mip) our sampler aliases the sparse specks → stripes. (The
   prior handoff CLAIMED "NOT minification aliasing"; re-test that claim — the blend math above makes
   aliasing plausible.)
2. **Wrong UV texmatrix** — the 31×/8× ASYMMETRIC tiling looks wrong; real foam UV is likely more
   isotropic. The sea/foam UV texmatrix may depend on the DEFERRED TMapObjWave wave state
   (m24/m28/m3c/m40/m64/m68 — the animated sea displacement getHeight left unported, see
   debug_journal/2026-06-25_fileselect_mario_low_wave_getheight.md), which we leave at 0/default → a
   wrong GXSetTexCoordGen/texmatrix. RE the sea material's texgen in reference/sms and compare our
   generated UV to GX's.

## Blend/path already RULED OUT
The bm=1/4/2 blend factors are faithfully mapped (nvk gx_blend_factor). b30 is a faithful perform(0x8)
draw, not a forced over-draw. So the divergence is in the SAMPLING (mip) or the UV (texgen/texmatrix),
not the blend or the draw path.
