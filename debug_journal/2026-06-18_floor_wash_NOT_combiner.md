# Delfino floor "wash" — ROOT CAUSE CORRECTED: it is NOT the TEV combiner (2026-06-18, session 13)

Continuation of `2026-06-18_delfino_wash_investigation.md`. That session concluded the floor wash
was a **TEV combiner** divergence ("flat materials too bright"). **That conclusion is FALSE.**
This session disproves it with hard data and re-localizes the bug.

## Method (per the user directive: layer-divergence, not pixels-only)
Frame-exact oracle_ab @ emu 14s (idle Delfino): MEAN 35%, floor rows 3-4 ~120-137 delta, top ~40-63.
ngx floor ≈ (235) bright; Dolphin-GX oracle floor ≈ (99,106,112) ≈ 0.45×. Zoom-crop of both floors:
**identical tile texture, geometry, perspective — purely a uniform ~2× brightness difference.**

Localized the visible floor via `/pixbatch` (NDC→batch): at the near-center floor pixel the winner is
`draw=17 ti=10 tex0=80d0dec0`, shape **8112bfdc**, combiner `s0ce=08f8af`, cc=0701 (matSrc=VTX,
lighting OFF), PE blend=ONE/ZERO opaque, 1 stage.

## What was RULED OUT (each with reliable evidence)
1. **TEV combiner translation** — combiner read from the J3D material object (TevBlock 811342c4 raw
   bytes `c0 08 f8 af`) is `08f8af` = `lerp(ZERO,TEXC,RASC)` = **texture × rasterColor**. ngx's
   generated GLSL matches. (The `/gxstate` bpmem diff showing `gx=08428f` is the documented
   ASYNC-LAG trap — bpmem is GP-thread state; do not trust it. Verified ti=10 is one TEV state.)
2. **Lighting / ambient** — floor material is CLOF (LightOff block, vt=803e0d38), cc=0701 enable=0,
   no normals (nrm cls=0). Unlit. The earlier `/gxstate xfmem cc=070f (lit, mask=3)` was the
   xfmem async-lag trap (xfmem-not-cpu-oracle). The "ambient fix" (6bd521f) is irrelevant here.
3. **Texture decode** — `/tex` parity self-test 119/119 PASS (ngx decode == Dolphin decode). Floor
   texture 80d0dec0 (CMPR 64×64) mean = 234 (bright).
4. **Vertex-color decode** — parsed shape 8112bfdc's REAL primitive DL (J3DShapeDraw at sh+0x38 →
   draws[0]+8 = 80c1dd20, size 3488). VCD = POS idx16 + CLR0 idx16 + TEX0 idx16 (vstride 6, CLR0 at
   byte offset 2). **CLR0 index = 1 for ~all 525 verts** → array 80b9b600[1] = (255,255,255) WHITE.
   ngx reads this correctly (the diagnostic /ngxshapes clr0 mean (231,239,244) and the render
   snapshot agree). The static CLR0 array 80b9b600 is confirmed static across frames (BMD, not a
   per-frame computed buffer) and is the same base the DL itself bakes (`CP[a2]=00b9b600`).
5. **CLR0 buffer choice (unk114 vs BMD)** — decomp: `J3DShapePacket::draw` sets `j3dSys.unk114 =
   packet.unk2C` right before the shape draws; ngx reads unk114 at the same point. Both = 80b9b600.
6. **Mipmapping** — ngx mesh textures are mipLevels=1 (no mips). BUT the floor texture mean is 234
   (bright), so trilinear minification would BRIGHTEN GX toward 234, not darken to 105. Not the cause.

## The contradiction → the REAL localization
Floor = `texture(bright 234) × vtxColor(white, idx 1)` → ngx correctly renders it **bright (~210)**.
Both engines parse the SAME DL with the SAME indices into the SAME static array → they CANNOT differ
on shape 8112bfdc. Yet the GX oracle shows that pixel **dark (~105)**, and `/pixbatch` shows
**8112bfdc is the ONLY shape covering that near-center floor pixel** in ngx.

⇒ **GX draws a darkening element over the near floor that ngx is MISSING entirely.** It is not a
per-material shading bug, not depth-order among captured shapes (only one shape covers the pixel),
and not vertex/texture/combiner. ngx is dropping a draw.

Corroboration: the FAR floor renders dark in ngx correctly (e.g. draw=58 ti=10 vtxColor 0.28 → dark),
so the floor-shading machinery works where the data carries it. Only the near patch (white-vtx base
8112bfdc) lacks its darkening overlay in ngx. **This is almost certainly the SAME class as the
missing Mario ground shadow (marukage)** — a draw ngx's J3DShape capture does not see.

## NEXT (for the fix)
Find HOW SMS draws the near-floor darkening / the marukage shadow:
- Candidate: an **immediate-mode GX draw** (ngx only captures `J3DShape::draw`; memory note
  `fileselect-cloud-wash-drift-artifact` already flags "ngx misses immediate-mode GX draws").
- Candidate: a non-J3DShape path (J3DGrafContext direct, a shadow/projection system, particle).
- Candidate: a J3DShape ngx CULLS (FORCECULL / near-clip / framing_fail) — check the floor-overlay
  shape isn't being dropped at capture.
Method: compare GX draw/triangle counts vs ngx captured meshes for the scene; RE the marukage shadow
draw path in reference/sms; once the path is found, capture+composite it natively (own-it-natively).
Drive the fix with oracle_ab.sh (floor delta must drop) — but localize via the missing-draw layer,
not pixels.

## Tools used (all reliable, reusable)
- `tools/render/oracle_ab.sh 14` (SUNBRIGHT_BIN=build-freshtest/sunbright) — frame-exact A/B.
- `/pixbatch?x=&y=` (NDC→covering batches, front-to-back, with cc/combiner/tex/vtxColor).
- `/pixblend?x=&y=` (CPU per-layer TEV+blend replay → final ngx-predicted pixel).
- `/ngxshapes` (per-shape clr0[cls/fmt/base/mean], nrm, cc, tex0, ndc bbox).
- Python DL parser over `/r?a=&n=` (GX command stream: CP loads 0x08, XF 0x10, prim 0x80-0xBF;
  J3DShapeDraw DL at sh+0x38→draws[0]+8, size at +4).

## Shadow-path lead (the missing-draw mechanism)
`reference/sms/include/MarioUtil/ShadowUtil.hpp`: `TMBindShadowManager` (global `gpBindShadowManager`,
a `JDrama::TViewObj`) has `drawShadowGD(u32, TGraphics*)`, `drawShadowVolume(bool, TAlphaShadowQuad*)`,
`drawShadow(...)`, plus `TCircleShadowRequest` (circle shadows under actors). The `...GD` naming =
direct GD/immediate-mode GX emission, NOT `J3DShape::draw` → **ngx's J3DShape capture never sees it**.
`ShadowUtil.cpp` is NOT decompiled (empty stub), so the exact GX calls must be RE'd from the binary
(`sunbright-recomp --disasm`) or observed via a GX-call tee. This is the prime suspect for BOTH the
missing Mario marukage (finding #2) and the near-floor darkening (finding #1): if the near plaza floor
sits in a cast shadow that GX draws via this system, ngx misses it → near floor renders only its bright
base shape (8112bfdc, white CLR0). Next session: tee/RE the shadow draw, capture it natively, composite
it; confirm the floor delta drops via oracle_ab.sh. (Caveat not yet closed: confirm the near-floor
darkening is actually a cast shadow vs some other uncaptured overlay — the oracle floor looked fairly
uniform, so an EFB-copy routing leak of a bright pass is an alternative worth checking too.)
