# 2026-06-16 — File-select (and general) ngx render wash

## Symptom
The native renderer (`SUNBRIGHT_NGX_PRESENT`) renders the **file-select** screen badly
washed out / overexposed: the 3D beach background (sky, sea, sand, distant island) is
near-white, low contrast. Vanilla / Dolphin-GX (the oracle) renders it correct: vibrant
blue sky gradient, blue sea, tan sand, green island.

Quantified on the static file-select (same screen, both reached by pressing Start to skip
the FMV — NOT fastboot):
- ngx full render: median luma **236**, mean RGB ≈ (207,213,219) — bright, neutral.
- oracle full render: median **158**, mean RGB ≈ (110,162,198) — darker, **blue-dominant**.

## Tooling built this session (all committed; see `/probe` skill + `tools/gp`)
- **gp oracle is now LIVE pure-Dolphin** (NGX_SHAPE OFF). ⚠ MAJOR GOTCHA discovered:
  enabling the J3D capture (`SUNBRIGHT_NGX_SHAPE`) **BYPASSES Dolphin's GX render and
  freezes its XFB** on an early frame. Every "oracle" comparison before this fix (this
  session and likely prior) compared against a **frozen** reference → wrong conclusions.
- `/abshot2` (+ `gp abshot2`): same-present dual capture (Dolphin XFB + ngx texture) → PPM,
  zero drift. NOTE: only valid with a live Dolphin GX, which conflicts with NGX_SHAPE — use
  the two-process harness for live A/B; `/abshot2` is for the present-substitution path.
- `/ngxdbg?m=normal|tex|ras|cat|bid|tex1|uv0|uv1` (+ `gp dbg`): flip the renderer's debug
  output on a LIVE scene (clears the pipeline cache, regenerates shaders). No relaunch.
- `/ngxdbg?nolight=N`: toggle native lighting live.
- `/xfdump` (+ `gp xf`): dump Dolphin's live xfmem (ambient/material/channel). Truth in the
  live oracle; LAGS in an ngx-capture process. Caveat: shows the LAST-drawn material only.
- `SUNBRIGHT_NO_SHADER_CACHE`, `SUNBRIGHT_NGX_NOBLEND`, `SUNBRIGHT_NGX_AMBGD` env A/B knobs.

## Ruled OUT (with evidence)
- **Lighting**: ngx-NOLIGHT (med 235) ≈ ngx-normal (med 237). Lighting is not the wash.
- **Blend overdraw**: `SUNBRIGHT_NGX_NOBLEND=1` (force all opaque) — wash unchanged.
- **Fog**: fsel=0 (disabled) for file-select.
- **EFB copy filter**: coefficients sum to 64 (unity).
- **Palette / TLUT / CI**: sky texture is fmt=5 (RGBA8, not CI). The palette path is already
  fully PC-native anyway (`tex_decode.cpp` + `ov_gxloadtlut` parse guest RAM, no Dolphin).
- **Texgen**: `uv1` debug shows texcoord1 varies smoothly across the scene — UVs are fine.
- **Texture decode/binding**: textures decode (cards crisp in tex mode); all 8 texmaps bound.

## Root cause (pinned)
1. The sky/sea **textures are white/black masks** (tex0 AND tex1 debug both show white sky /
   black sea — no blue texture exists). Their colour comes from the **raster** (vertex/material
   colour channel), modulated by the mask texture.
2. ngx's **raster for the sky is white**; the oracle's raster (DBG_RASCOLOR) is a **blue
   gradient**. That is the entire difference.
3. The sky material: `cc=0686` → matsrc=**REG**, lit, ambsrc=**REG**, diff=SIGN, attn=SPOT,
   light0. matColor read from guest RAM (block `+0x04`) = **0xffffffff (white)**. Lights are
   white. So `colors_0 = matColor(white) * clamp(ambient + lights)`. For the oracle's blue
   raster, the **ambient must be blue**.
4. ngx cannot find that blue ambient:
   - The block is **CLOF** (`J3DColorBlockLightOff`, vtable 0x803e0d38 — ALL 155850 blocks in
     this scene). CLOF has **no ambient field** and `setAmbColor` is a no-op
     (verified in `reference/sms/.../J3DColorBlocks.hpp` + `J3DMaterial.cpp`). CLOF::load()
     does NOT program ambient — the material inherits the global XF ambient register.
   - `J3DColorBlockLightOn::load()` programs ambient via **`J3DGDSetChanAmbColor`** (0x802f33a8),
     a GD/XF-direct write — NOT `GXSetChanAmbColor`. We now tee BOTH:
     - `GXSetChanAmbColor` tee: fires ~27k×/scene but reads a **wrong ~purple (128,66,99)**.
     - `J3DGDSetChanAmbColor` tee: fires **0×** during file-select.
   - So the blue XF ambient the GPU actually uses is programmed by **neither** captured path.

## The architectural gap (the actual open problem)
The XF **ambient register** state the GPU uses is NOT in the J3D material objects ngx reads
(CLOF has no ambient field), and is NOT captured by either ambient function tee. It is set
somewhere we haven't found — candidates to investigate next:
- A scene/JDrama-level one-time ambient setup (search file-select / shine-select scene code
  and the JDrama light/`J3DGDSetChanAmbColor` callers; the early call may precede capture).
- Inlined GD-buffer XF writes (a material/model's prebuilt display list replayed by the GPU)
  that don't go through the named function — would require interpreting the GD display list.
- Reading XF ambient with correct synchronization (xfmem lags in the ngx-capture process).

Default kept at **ambient=0** (studied-safe; gameplay renders correctly with it). The purple
global register and the (0×) GD capture are both behind opt-in envs (`NGX_AMBGLOBAL` removed;
`NGX_AMBGD=1`) so no default regression. **No hack applied** — the real fix needs the ambient
source found/captured.

## Next step
Find where the file-select (and per-scene) XF ambient is programmed and capture it natively
(own the channel state the same way `capture_colorchan` owns matColor/channel-control). Verify
each candidate against the LIVE oracle (`gp launch both` + `gp dbg ras` vs oracle DBG_RASCOLOR)
on the static file-select — drift-free because the screen is static.
