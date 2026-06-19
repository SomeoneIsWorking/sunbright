# 2026-06-19 — Magenta "NPC" blob: PINNED to ti=64's 7-stage combiner (alpha→green via K2 konst)

Gap #3 secondary (handoff): the magenta blob. Prior journal CONTRADICTED itself (CLR0-array source /
by-pointer matColor). Both are **WRONG** — pinned here with hard oracle evidence.

## It is a REAL ngx shading bug (not faithful, not CLR0, not matColor, not visibility)
Localized the magenta in `freeroam_plaza.sav` to a single small cluster at **NDC (0.887, 0.308)**
(top-right, near the white building) — ~146 px. (The "NPC/Pianta" framing was a red herring; the
Pianta by the pillar is NOT magenta in this scene.)
- ab_oracle PPMs at that cluster: **Dolphin-GX = (86,91,99) grey-blue; ngx = (120,44,107) magenta.**
  So ngx is wrong (oracle has only 34 magenta-ish px total vs ngx 146).
- `/pixbatch` winner there = **draw=124 ti=64** (sh=815e9a40), a **7-stage** TEV material, vertex
  rgba WHITE (so magenta is NOT from vertex color), blend=1 atest=1, samples texmaps 0/1/2/3/5.
- `/gxstate?ti=64`: **matColor reads CORRECT** (ngx (255,255,255,252) ≈ xfmem (255,255,255,255);
  the "GX fn-tee (128,66,112)" is the known by-pointer-misread artifact, NOT what ngx uses). ⇒ the
  journal's matColor/CLR0 theories are FALSIFIED.
- `/ngxskipset?ti=64` → magenta DISAPPEARS, background behind it is WHITE sky (254). So ti=64 SHOULD
  draw there (over bright sky); GX tints it to grey, ngx tints it magenta ⇒ **shading bug, not a
  visibility/blend-order bug.**

## Mechanism (traced from the generated GLSL)
KONST: K0=(0,0,226) K1=(179,0,0) K2=(255,0,255 MAGENTA) K3=(255,130,255). C0=C1=(255,255,255),
C2=0, CPREV-init=(-90,0,-114,135). Stages (color):
- s0: prev = C0 + TEX1·K0   (unclamped)      s1: prev = 2·prev + 2·TEX2·K1   (unclamped)
- s2: prev = prev + TEX0                       (clamped 0..255)
- **s3: COLOR ADD(d=0, a=APREV, b=CPREV, c=K2=(255,0,255))** = lerp(prevA, prevRGB, K2):
  r = prevA+(prevR-prevA)·255/255 = prevR; **g = prevA+(prevG-prevA)·0 = prevA**; b = prevB.
  ⇒ **stage 3 maps GREEN ← accumulated ALPHA.** So a wrong accumulated alpha → low green → magenta.
- s4→C2, s5→C1, s6 combines. (Final visible is the s6 prev blended SRCALPHA over white sky.)

Alpha accumulation (the suspect): s0 a = C0a − TEX1a·K0a(88); s1 a −= TEX2a·K1a(182); s2 a += TEX0a.
GX makes prevA ≈ prevRGB (~90) → grey; ngx makes prevA ≈ 44 → magenta green. ⇒ ngx's accumulated TEV
**alpha** diverges. Likeliest roots (UNVERIFIED — needs a per-pixel TEV trace):
1. a **texture ALPHA decode** wrong for one of texmap1/2/0 (the alpha inputs), or
2. an **alpha-combiner clamp/scale** detail in s0–s2 (s0/s1 are unclamped in ngx — verify vs GX).

## NEXT STEP (tooling-first): build a per-pixel TEV-value trace
There is no tool to dump intermediate TEV stage outputs (prev/c0..c2, tevin_a..d, textemp.a per
stage) for ONE pixel of ONE material. Build that (a debug shader variant or a CPU re-eval of the
captured combiner over the captured vertex+sampled-texel inputs at a pixel) → compare ngx vs the GX
oracle stage-by-stage to nail whether it's texture-alpha or an alpha-combiner op. Then fix the named
defect. Do NOT guess-patch the combiner. (Also found while reading the GLSL: the shader hardcodes
`prev = ivec4(0,0,0,0)` and passes only C0/C1/C2 — it **ignores the CPREV/TEVPREV register init**
(tev_color[0]). Not ti=64's cause (s0 writes prev before any CPREV read), but a real latent bug for
any material that reads CPREV in stage 0; fix separately + count exercised materials first.)

Repro: load freeroam_plaza.sav, `/pixbatch?x=0.887&y=0.308`, `/gxstate?ti=64`, `/ngxskipset?ti=64`.
