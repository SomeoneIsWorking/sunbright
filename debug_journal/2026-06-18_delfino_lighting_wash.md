# 2026-06-18 — Delfino gameplay 2.2× brightness WASH (ngx native renderer)

## Symptom (RELIABLE — not drift)
FASTBOOT into Delfino Plaza, `SUNBRIGHT_NGX_PRESENT=1`, time-averaged /abshot2 A/B (static
camera — GX self-delta ~3/frame, so NOT the camera-drift artifact that plagued file-select):
**ngx is ~2.2× too BRIGHT across the whole frame** (centre-band mean GX≈(95,98,100) vs
ngx≈(213,215,210); full-frame |delta| ≈ 100). The plaza/buildings are overexposed/washed.
(File-select, by contrast, is faithful — delta ~36 — see the other journal.)

## What it is NOT
- **NOT lighting being ADDED.** `/ngxdbg?nolight=1` (lighting OFF) gives the SAME brightness as ON
  (both ≈213). So ngx's lighting is not over-brightening by adding light — it's failing to DARKEN.
- **NOT camera drift / animation.** GX + ngx are both static in FASTBOOT (self-delta ≤3).

## CONFIRMED partial cause: wrong AMBIENT for lit CLOF materials (DL-baked async capture)
`/ngxshape` lighting dump + XFMEM ground truth:
- ngx uses `amb_reg[0]=(128,128,128)` (grey 0x808080) for the lit world materials. The material's
  channel control is cc=0500 (CLOF, ambSrc=REG). XFMEM (Dolphin GPU-side ground truth) for the
  SAME material says **amb=00000000** (black). So ngx's ambient is wrong → illum = 0.5(amb)+0.835
  (diffuse) = 1.34 → clamps to 1.0 (full bright) where GX's illum ≈ 0.45.
- The game sets the XF ambient register to MANY values per draw-group (`XF_AMB` hist:
  `0×6488  808080×2916  282828×1249  ffffffa0×416  00277700×207`). ngx tracks the GLOBAL latest
  (grey) and uses it for ALL materials; each material's correct ambient is the one current at ITS
  GPU draw. The world materials' correct ambient (0) is set via J3D's DL/XF-direct path, which
  ngx's CPU-side function tee (GXSetChanAmbColor) does NOT see in order — the SAME DL-baked async
  trap as the cloud texmtx. (`J3DGDSetChanAmbColor` @0x802f33a8 fires 0× — not that path.)

### A/B evidence (FASTBOOT Delfino, ratio = ngx/GX centre-band brightness)
| mode | ratio | note |
|---|---|---|
| default (ngx amb=grey 128) | **2.26** | the wash |
| `SUNBRIGHT_NGX_DOLAMB=1` (xfmem per-material ambient) | **1.42** | best — xfmem ambient is per-material-correct |
| `SUNBRIGHT_NGX_AMB0=1` (force ambient 0) | **2.17** | ~no help — correct ambient is NOT uniformly 0 |
| `SUNBRIGHT_NGX_AMBGD=1` (J3DGDSetChanAmbColor capture) | n/a | GD path fires 0× → no data |

So: the per-material ambient (which xfmem has right) is a CONFIRMED ~halving factor. The fix must
capture the per-draw-group ambient FAITHFULLY (in GPU draw order), NOT the global latest and NOT a
flat 0. This is the DL-baked async capture problem (same class as the cloud texmtx).

## RESIDUAL (beyond ambient) — flat/non-lit materials also too bright
Even with the correct ambient (DOLAMB), ngx is still 1.42× bright. `/ngxshape`: there are huge
`flat_verts` (322M, mean_lum 0.629) alongside `lit_verts` (195M, mean_lum 0.449). The plaza floor
band stays ~213 even with AMB0, suggesting the dominant plaza material is FLAT (vtx/tex colour, no
lighting) and renders its texture×vtxcolour at full brightness while GX shows it darker. Open: why
are GX's flat surfaces darker — a TEV/konst/matColor difference, an EFB-copy/gamma stage, or a
missing darkening pass. Needs the same per-shape A/B isolation, NOT eyeballing.

## Status / next
This is the **N6 lighting/material FIDELITY gap** the renderer plan flagged OPEN. Root cause is
PARTLY pinned (per-draw-group ambient capture = ~half) and partly open (flat-material brightness).
The faithful fix is NOT shipped (DOLAMB reads xfmem = against the ngx ground rules + only partial;
AMB0 doesn't work). Proper fix = capture the per-draw-group XF ambient register in GPU draw order
(RE J3D's ambient programming path — it's neither GXSetChanAmbColor-fn nor J3DGDSetChanAmbColor;
likely a direct XF-write baked in the per-draw-group setup). Then re-attack the flat-material
residual with per-shape A/B. Tools: FASTBOOT + time-averaged /abshot2 (static camera, reliable),
`/ngxshape` lighting dump (amb_reg vs XFMEM ground truth + XF_AMB histogram), `/ngxdbg?nolight`,
SUNBRIGHT_NGX_{DOLAMB,AMB0,AMBGD}. Data: scratch/screenshots/{ambdef,amb0,da,ag}_*.{gx,ngx}.ppm.
