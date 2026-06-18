# Delfino "wash" — session 15 CORRECTION: it's the GROUND that's ~0.45× too BRIGHT, not "unlit floor + ambient"

Supersedes the mechanism in `2026-06-18_floor_wash_EFB_per_fragment.md` and the handoff
`scratch/handoff_2026-06-18_floor_wash_per_fragment.md`. The EFB-peek "per-fragment" fact still holds;
the *explanation* in that handoff (unlit matVTX white floor that GX darkens via inherited ambient) is
**WRONG** and is corrected here with frame-exact pixel data.

## METHOD (the one that finally produced numbers)
`tools/render/oracle_ab.sh 14` (SUNBRIGHT_BIN=build-freshtest/sunbright) → frame-exact GX-oracle
(`ab_oracle.ppm`) vs ngx (`ab_ngx.ppm`). Then `PIL.getpixel` at known points + a 3×3 region
ratio map (oracle/ngx). This is the ONLY trustworthy oracle (xfmem is async-lagged — re-confirmed:
xfmem lights read all-zero in the NGX_PRESENT=1 run; xfmem cc flips en=1 from neighbours).

## THE HARD NUMBERS (frame-exact, emu_secs=14, idle Delfino plaza)
3×3 region ratio = oracle / ngx (so <1 ⇒ ngx too bright):
```
1.16  1.23  1.29     ← SKY (top): ngx too DARK (separate, known sky issue)
0.47  0.45  0.47     ← horizon/mid
0.43  0.43  0.44     ← floor (bottom)
```
Point samples: floor-left oracle(101,106,113) ngx(250,242,240); floor-center oracle(72,75,91)
ngx(181,170,192); tree-trunk oracle(11,42,27) ngx(43,131,46). The floor ratio is a CONSTANT ~0.40
regardless of near/far/position. Model fit over 38k floor pixels: **linear oracle=0.43×ngx (err 0.058)
beats gamma oracle=ngx^g (best g≈4, err 0.37)** → it's a LINEAR darkening, not gamma.

## WHAT IS RULED OUT (hard data this session — do NOT re-open)
- **NOT per-material lighting/ambient** (the handoff's lead). The ~0.45× hits BOTH lit trees (0.36×,
  reg/lit) AND the nominally-unlit floor (0.43×, vtx/flat) *uniformly*. Lighting would only touch lit.
- **NOT vertex-color decode.** The floor's CLR0 array `80b9b600` is genuinely BRIGHT (raw entries
  4d5b88, ffffff, d8d5fd, 505a94, 8692bc, e2eaff… mean ≈200). ngx reads it correctly. GX reads the
  SAME array (same base/stride/RGBA8 idx16). decode_color fmt5 = R,G,B,A bytes (unambiguous).
- **NOT the texture.** Floor texture `80d32940` CMPR 64×64 = mean R=235 cream, A=255 (`/texat`). The
  ti=11 overlay tex `80d22940` = mean 223 cream. Both bright; ngx decode parity OK.
- **NOT the TEV combiner.** ngx s0 = TEXC×RASC, scale ×1, bias 0 (verified GLSL). (bpmem 08fffa is the
  async-lag caveat; object-model read is texture×rasColor.)
- **NOT a multiply-blend darkening J3D pass.** ALL captured blend layers are additive/normal/screen:
  ti=8 SRCALPHA/ONE, ti=78 SRCALPHA/INVSRCALPHA, ti=9 ONE/INVSRCCLR (sky), ti=80 SRCALPHA/ONE. NONE is
  a multiply (dst=SRCCLR). Skipping ti 8,9,77,78,80,81 did NOT change the floor pixels.
- **NOT EFB copy gamma/scale.** A whole-frame copy darkening would darken the SKY too; the sky is the
  OPPOSITE (oracle brighter, 1.2×). So the darkening is GROUND-ONLY, in-EFB (EFB peek = dark).
- **NOT the ti=11 overlay or compositing of overlays.** `/ngxskip ti=11` and skipping all blend ti
  did nothing. `/ngxskip ti=10` → floor goes BLACK ⇒ the visible floor IS ti=10.
- **NOT a single global multiply** (sky region is 1.2×, ground 0.45× — region-dependent).

## THE PARADOX (the genuine open core)
The visible floor is **ti=10, cc=0x0701 = en=0 (UNLIT), matSrc=VTX** (pixbatch winner at floor-center
draw=33; `/ngxshape` "BIGGEST map shape cc=0701 en=0"). For an unlit matVTX channel, output = vertex
colour, and BOTH engines read the same bright array → they MUST agree. Yet GX renders the floor at
0.43× of ngx. Under "identical unlit inputs" this is logically impossible, so ONE premise is false:
  (A) **ngx mis-reads the floor's channel-enable** — the floor is actually en=1 (LIT) in GX and ngx
      captures en=0 (wrong block, or the live `j3dSys.mMatPacket`@+0x3C / material@+0x38 / colorBlock
      @+0x20 read races the draw, grabbing a neighbouring decal's CLOF/en=0 block). NOTE: ALL colour
      blocks in the scene are CLOF (vtable 803e0d38) — but CLOF blocks CARRY en in their chanctrl and
      MANY are en=1 (e.g. 0x068e, 0x070f). "LightOff" only means the block stores no lights/ambient;
      lighting still runs off the GLOBAL ambient/light registers. So a "CLOF floor" can be lit.
  (B) **a ground-only darkening pass ngx doesn't capture** — an immediate-mode GXDraw / J2D / EFB
      effect drawn over the ground (not sky), ~0.45× linear, into the EFB. (No such J3D multiply pass
      exists, so if it's a draw it's outside the J3DShape capture.)
  (C) ngx over-LIGHTS lit surfaces (trees 0.36×: ngx illum too high — light colours/attenuation) AND
      mis-classifies the floor as unlit (A). Two bugs that happen to both read ~0.45×.

(A)+(C) is currently the most likely: ngx's reg/lit verts already render 0.22 vs GX trees 0.10 (ngx
2.2× too bright on KNOWN-lit geometry → ngx illum/light-colour IS too high), and the floor being
secretly lit would fold it into the same over-lighting.

## DECISIVE NEXT STEP (build this, don't eyeball)
Add a probe that, given a J3DShape ADDRESS (not the shared tev_index), reads its material→colorBlock
chanctrl directly, for the EXACT shape covering floor-center. That settles en=0 vs en=1 for the floor:
 - en=1 ⇒ ngx's per-shape material/chanctrl capture is wrong (fix the matpacket→material association
   / capture timing in ov_j3dshape_draw); then port GX lighting (light colours, attenuation, the
   global ambient) faithfully so lit ground hits 0.10–0.29, not 0.22–0.64.
 - en=0 ⇒ hunt the ground darkening pass (immediate-mode / J2D / EFB effect) outside J3DShape capture.
Verify the fix with `oracle_ab.sh 14`: the bottom-two region rows (now 0.43–0.47) must move to ~1.0,
AND tree-trunk must darken 0.29→0.10. The sky (1.2×) is a SEPARATE issue — don't conflate.

## Tools used (all live, build-freshtest/sunbright, ALWAYS HEADLESS)
`oracle_ab.sh N`; `/gxstate?ti=N` (obj-block vs xfmem vs fn-tee, raw block bytes, PE/blend, GLSL);
`/pixbatch?x=&y=` (NDC → covering fragments front-to-back, per-frag cc/blend/tex/rgba); `/ngxshapes`
(per-shape cc/clr0 base+mean/nrm/ndc); `/ngxshape` (col0 lum BY CATEGORY reg|vtx × flat|lit — the key
aggregate; colour-block vtable histogram; biggest-shape); `/ngxverts`; `/ngxskip?ti=` `/ngxskipset?ti=`
`/ngxdbg?nolight=`; `/texat?a=&fmt=&w=&h=`; `/r?a=&n=`. Reach Delfino: SUNBRIGHT_FASTBOOT=1, wait
emu_secs≥14 (`/metrics`), `curl /abshot2` to present, THEN probe. `pkill -9 -x sunbright` between runs.
