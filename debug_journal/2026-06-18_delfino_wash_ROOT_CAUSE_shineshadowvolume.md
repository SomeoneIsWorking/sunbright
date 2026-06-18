# Delfino "wash" ROOT CAUSE FOUND + PROVEN: TModelWaterManager::drawShineShadowVolume (plaza pollution)

2026-06-18 session 15c. This RESOLVES the multi-session Delfino "wash". Supersedes the leads in
`2026-06-18_delfino_wash_CORRECTED_ground_not_skydome.md` — they were right that it's a darkening pass
external to the floor material; this names it, RE's it, and proves it.

## THE ANSWER
The Delfino plaza ground renders ~2.4× too bright in ngx because ngx never draws
**`TModelWaterManager::drawShineShadowVolume` (0x8027c67c)** — the **plaza POLLUTION darkening**. It
runs ONLY on map 1 (Delfino Plaza) and recedes as the plaza is cleaned (flag 0x40000 → `unk5E0C` grows,
and at full-clean the effect disables: `unk5D60 &= ~0x100`). At file-1 start the plaza is polluted →
this draws a dark-blue volumetric darkening over the ground that ngx omits.

## HOW IT WAS FOUND (the tool chain — all committed, all reusable)
1. `/shapeat?x=&y=` — per-pixel shape → captured vs FRESH guest colorBlock + baked-DL walk. PROVED the
   floor scene-draw is genuinely unlit bright `tex×vcol` in GX too (baked DL `[100e=00000701]` en=0,
   `bpc0=08f8af` tex×ras) — identical to ngx. So the darkening is NOT per-material.
2. `/gxblend` — GXSetBlendMode histogram + caller LR + copy gamma. Found copyGamma=1.0 (copy ruled
   out) and TWO darkening blends with named callers:
     - `SUBTRACT` cnt 1635, caller `TModelWaterManager::drawShineShadowVolume`  ← the wash
     - `BLEND dst=SRCCLR` cnt 326, caller `TMapObjWave::initDraw`  ← smaller residual
3. `SUNBRIGHT_KILL_SHINESHADOW=1` (shadow_kill_diag.cpp) — no-op drawShineShadowVolume in the ORACLE:
   floor 101→193 (left), 72→139 (center). Closes ~80% of the 101→250 gap. PROVEN primary cause.
   (Residual 193 vs 250 = TMapObjWave multiply + minor lighting — port next.)

## THE EFFECT (RE'd from reference/sms/src/Player/ModelWaterManager.cpp:1310 drawShineShadowVolume)
Map-1-only. Params (TModelWaterManager init @175 + loadAfter @192; gpModelWaterManager guest obj):
  - `unk5E0C` (sphere base radius) = 6000 default; loadAfter: `flag=getFlag(0x40000)` clamp 60,
    `fVar1=flag/60`; if fVar1<1 `unk5E0C = fVar1*24000 + 8000` (so 8000 at flag 0 = fully polluted);
    if fVar1>=1 effect DISABLED. (stage 2 forces fVar1=1.)
  - `unk5E40`=100 (radius span), `unk5E44`=5 (sphere count r31), `unk5E45`=132 (clear alpha r27),
    `unk5E47/48/49` = (0,20,40) = **dark-blue tint** (matches the oracle's blue-grey ground).
  - sphere center (model, pre-viewMtx): translation (0, 3600, -7458); radius = i*25 + unk5E0C.
Passes (param_1 = j3dSys/director mViewMtx):
  1. CLEAR ALPHA: fullscreen quad (-1000..1000,z-200), ZMode(F,ALWAYS,F), ColorUpdate OFF, AlphaUpdate
     ON, blend NONE, TEV alpha = C0.a = 132 → EFB alpha := 132 everywhere.
  2. VOLUME (5 spheres, GXCallDisplayList sphere_glist_p, RGBA4 indexed, ZMode(T,GREATER,T)):
     per sphere: blend ONE/ONE + CULL_FRONT (back faces) ADD alpha(local_28.a≈25); then blend
     SUBTRACT + CULL_BACK (front faces) SUBTRACT — standard volume count vs scene depth (z GREATER).
  3. FINAL: fullscreen quad, TEV color = C0 = (0,20,40), blend INVDSTALPHA/DSTALPHA, DstAlpha(T,0),
     ColorUpdate ON → blends dark-blue over the scene masked by the accumulated EFB alpha.
Net: a soft dark-blue volumetric darkening of the polluted plaza ground; near-uniform over the visible
area (sphere radius ~8000 ≫ plaza), explaining the measured uniform ~0.45× linear darkening.

## PORT PLAN (own-it-natively, into ngx — NOT a flat multiply)
ngx_present renders the 3D scene into an R8G8B8A8 color + D32 depth target (PresentRenderer). Insert a
"pollution" pass AFTER the 3D scene (depth valid), BEFORE HUD, reproducing passes 1-3 with the color
target's ALPHA channel as the EFB-alpha scratch:
  - need: a unit sphere mesh; the view matrix (j3dSys mViewMtx) × sphere model (center (0,3600,-7458),
    radius from params); read params from gpModelWaterManager (guest RAM); 3 pipelines (alpha-clear
    quad [colorWriteMask=A], sphere volume [A only, z-test GREATER no-... match z-write, cull front/back,
    VK add + REVERSE_SUBTRACT], final masked blend [colorWriteMask=RGB, INVDSTALPHA/DSTALPHA]).
  - gate on map==1 + effect enabled; verify with oracle_ab.sh 14 (ground rows 0.43→~1.0) and the floor
    pixel matching the oracle (~101 not 250). Alternative: analytical sphere-fog in one fullscreen pass
    (reconstruct world pos from depth, ray-sphere chord) — but the literal passes are the faithful port.
Also TODO (smaller): TMapObjWave::initDraw multiply (326) for the residual.

## PORTED ✅ (session 15c, ngx_present.cpp + pollution shaders)
Implemented the native sphere-volume pass (capture tee on 0x8027c67c → live params + view matrix;
3 passes into the present colour-alpha+depth target: clear EFB-alpha, sphere volume front/back z-test
GREATER+write add/reverse-subtract saturating alpha inside the clean dome, final INVDSTALPHA/DSTALPHA
dark-blue blend). RESULT oracle_ab 14: mean delta **89.4 → 45.6**; floor rows ~120-137 → ~30-70;
floor-left 250 → 129 (oracle 101). Floor now dark blue-grey with a sunlit centre, matching the oracle.
Gotchas learned: sphere mesh winding made the cull inverted (sky net-subtracted → pure tint); FIX was
depthWrite TRUE on the volume passes (game uses ZMode GREATER,WRITE) so inside-dome saturates to
A=1 (bright) — sky-top went 0,20,40 → 59,57,55 (preserved). A/B envs: SUNBRIGHT_NGX_NOPOLLUTION=1
(disable), SUNBRIGHT_NGX_POLL_NOVOL=1 (clear+final only, no sphere).
RESIDUAL (delta 45, not 0): (1) dome-edge softness — 1 sphere vs the game's 5 concentric (radius span
only 100 so minor); (2) the SKY region (separate sky-wash issue + framing); (3) TMapObjWave::initDraw
multiply (326, the other /gxblend darkening caller) — port next for the floor 129→~101 residual.

## Tools (committed)
/shapeat?x=&y= · /gxblend (+ darkening caller LRs) · SUNBRIGHT_KILL_SHINESHADOW=1 · oracle_ab.sh 14.
