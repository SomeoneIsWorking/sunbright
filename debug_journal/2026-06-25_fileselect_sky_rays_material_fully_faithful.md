# File-select sky rays (b3): the ENTIRE material pipeline is faithful — divergence is geometry/over-entry/BTK-phase (2026-06-25)

## Summary
The bright white vertical "light rays" in the file-select sky (batch **b3**, shaderKey
`7bc0841d43634f53`) are rendered **faithfully** from every captured material input. This EXTENDS
the prior proof (a260b16, which ruled out fog/combiner/matColor/I4-decode) by also ruling out the
**color-channel control (chanctrl) and per-vertex color source**. The remaining cause is NOT the
material pipeline at all — it is geometry / draw-order / over-entry / BTK texture-animation phase.
Do NOT re-chase the material/blend/TEV/chanctrl for these rays.

## What b3 is (all values measured, value-first)
- TEV (2 stages, key 7bc0841d): `out = tex0 * col0 * tex1 * 2`, additive blend bm=1/4/1
  (src=SRCALPHA dst=ONE). No konst, no TEV register inputs.
- col0 = color channel 0. Per-packet material (`mp->getMaterial()->getColorBlock()`, the REAL drawn
  material — we already read per-packet, not the modelData base):
  - `getColorChanNum()=1`, `mColorChan[0..3].mChanCtrl = 0700,0701,0202,0400`
  - COLOR0 (cc[0]=0700): matSrc bit0=0 → **GX_SRC_REG**, lighting enable bit1=0 → **off**. → col0.rgb = matColor.
  - ALPHA0 (cc[1]=0701): matSrc bit0=1 → **GX_SRC_VTX** → col0.a = vertex alpha.
  - `matColor[0] = 255,255,255,255` (white).
  - vertex CLR0 = **(0,0,0,255)** (rgb BLACK, alpha 255) — see `[cov] shape#3`.
  - So faithfully: col0 = (white rgb from REG, alpha 255 from VTX). rastemp = (1,1,1,1).
- Both textures t0,t1 are the SAME 8x8 intensity texture (blocky 0/255, mean 123) — dumped to
  `scratch/frames/btex_03_t{0,1}_{rgb,a}.ppm`. Output = tex0·tex1 → bright where both white.
- Two texgens (`SB_TEXGEN_DBG`): tg0 src=4 scale 2.0 off 1.931; tg1 src=5 scale 0.5 off (2.681,0.250).
  Different scales (4×) → moire of the half-white texture → still hits overlapping white → bright.
- Sky MActor animations bound (`SB_SKY_ANM_PROBE`): **only a BTK** (texture-coord). NO BPK, NO BRK.
  So there is NO material-color / TEV-register animation that could darken col0 at runtime.
  (Note: `TSky::load` skips `startAllAnim` for `mMap==15`, but the BTK IS bound here → the
  file-select sky's internal `mMap` is NOT 15; SB_STAGE=15 maps to a different mMap.)

## The decisive experiment (and why it is NOT the fix)
Forcing b3's COLOR0 to source from the (black) vertex (`SB_B3_VTX`, since reverted) made the rays
vanish and the sky match the oracle EXACTLY. This proves the rays *should* be dark — but it is a
BANDAID, not the root cause: the per-packet chanctrl genuinely says COLOR0=REG=white, with no
animation to change it. We render col0=white faithfully. So forcing VTX just paints over a
non-material divergence. Removed; do not commit a key-based color force.

## Conclusion — where the divergence actually lives (for the next session)
Every captured MATERIAL input is faithful (combiner, chanctrl color+alpha source, matColor, vertex
color, both textures, both texgens, blend). The bright rays are the CORRECT output of this material.
The oracle shows them absent. Therefore the cause is one of:
1. **Over-entry / draw set**: the real perform list may not draw these specific ray SHAPES in this
   scene. (sky.bmd as a whole IS drawn — the gradient b4 renders — so it would be a per-shape skip,
   not the whole model. Check whether the ray shapes are in a sub-model/material the real draw-buffer
   sort drops, or behind the opaque dome b0 via z.) Draw order in present: dome b0 (opaque, z=0.99993,
   z-write) first, then gradient b4 (z 0.99982, nearer, shows) and rays b3 (z 0.99985–0.99997, zw=0).
   z alone does not cleanly explain full invisibility (half of b3 is nearer than the dome).
2. **BTK texture-animation phase**: output = tex0·tex1 of the SAME texture at tg0(×2)/tg1(×0.5). If
   the real BTK scrolls the two texgens so their white cells never overlap (product≈0), the rays
   vanish. Our calcAnm advances the BTK, but our anim-frame COUNT likely differs from the real game's
   (we tick from drive_sky start; the game ticks from map load) → different phase. BUT the oracle has
   NO rays at ANY brightness, which argues a phase artifact is unlikely (a pulsing god-ray would be
   visible at some phase). Verify by reading the oracle sky region for faint structure before assuming.

## New diagnostics added this session (committed, env-gated, OFF by default)
- `[mat]` line now prints `msv1`, `cc0`, `cc1` (COLOR0/ALPHA0 chanctrl) alongside `msv0`/`matc0`.
- `[cov]` line now prints the source vertex CLR0 (`vclr0=r,g,b,a`) + the shaderKey — THE way the
  black ray vertex was found.
- `[rawcc]` (SB_J3D_DBG): one-shot raw colorblock dump for the ray material (key 7bc0841d) —
  nchan + all 4 mChanCtrl + matColor, read fresh from the per-packet material.
- `[sky-anm]` (SB_SKY_ANM_PROBE=1, scene_drive.cpp): dumps which anims (bck/bpk/btp/btk/brk) the sky
  MActor has bound. Proved only BTK is bound.
