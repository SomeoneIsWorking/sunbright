# Delfino floor "wash" — ROOT CAUSE (definitive): ngx does NOT load authored MIPMAPS (2026-06-18, session 14)

Supersedes BOTH prior conclusions, each now FALSIFIED with hard data:
- `2026-06-18_floor_wash_NOT_combiner.md` (session 13): "floor wash = a MISSING DRAW (cast shadow /
  immediate-mode geometry)." **FALSE.**
- the earlier `delfino-lighting-wash` thread: "wrong per-draw-group AMBIENT / lit-shading too bright."
  **FALSE** (the ~0.5 blue factor only *looked* like ambient).

## THE ROOT CAUSE (airtight, hard data)
The Delfino floor renders ~2.2–2.5× too BRIGHT in ngx (oracle floor ≈ (101,106,112) muted blue-grey;
ngx floor ≈ (250,250,250) near-white). The floor is built from `J3DShape`s, ti=10, material UNLIT
(cc=0x0701, enable bit1=0 — confirmed against the decomp `J3DColorChan` bit layout), matSrc=VTX,
combiner `08f8af` = `TEXC*RASC` (texture × rasterColor). Vertex color CLR0 index = 1 = WHITE
(re-parsed the real `J3DShapeDraw` DL: 138/159 verts index 1, vert-weighted mean (255,255,255)).
So `frag = texture × white = texture`. ngx samples the texture ≈ white; GX samples it ≈ (101,106,112).

**The floor texture (CMPR 64×64 @ guest 0x80d32940, minF=5 = GX_LIN_MIP_LIN) ships an AUTHORED mip
chain whose HIGH (minified) levels are deliberately DARK BLUE** (decoded the CMPR endpoints myself):
- L0 64×64, L1 32×32, L2 16×16, L3 8×8 → mean RGB ≈ (235,227,218) bright cream.
- **L4 4×4 ≈ (64,60,175), L5 2×2 ≈ (26,62,134), L6 1×1 ≈ (57,48,170) → DARK BLUE.**
These dark mips are WITHIN this texture's own mip region (CMPR total = 2048+512+128+32 + 3×32(8×8-
tile-padded) = 2816 B), consistently dark-blue across all 3 small levels (authored, not a box filter,
not adjacent garbage). The plaza floor tiles ~100× (UV range ±50) → heavily MINIFIED → GX (trilinear)
samples the dark mips → blue-grey floor. **ngx loads only mip level 0** (`vk_mesh.cpp`: every mesh
texture is created `mipLevels=1`, sampler `MIPMAP_MODE_NEAREST`) → samples bright L0 → white floor =
the "wash." `texture(bright cream 235) × white = (101,106,112)` requires a ×(0.43,0.47,0.51) blue
factor — that factor IS the dark authored mip, NOT ambient/lighting.

Why session 13's dismissal was wrong: it checked only the texture MEAN (= L0's 234, bright) and
assumed "mips = box-average = bright, so minification can only brighten." But GC textures ship
*authored* mips that need not equal a box filter; here they were painted dark to fake distance
shading. (Session 13's "missing draw" deduction came from: floor base shape identical in both engines,
unlit, white vtx, bright L0 → ngx "correctly" bright → so GX must add darkening. The darkening is the
authored dark mip, sampled by GX's trilinear minification, not a separate draw.)

## What was RULED OUT this session (each with hard evidence)
- **Cast shadow / TMBindShadowManager (session 13's lead)** — added `SUNBRIGHT_KILL_SHADOW` (no-ops
  drawShadowGD 0x8022fa40 + drawShadow 0x8022f014; `runtime/overrides/shadow_kill_diag.cpp`).
  GX-with-shadow vs GX-without = mean 0.98, localized to a TINY blob under Mario (4×4 grid rows[2]
  cols[1,2] = 3.4 delta). The cast shadow IS a real, separate missing draw (worth porting later) but
  contributes ~3 of the ~120 floor delta — it is NOT the wash.
- **Lighting / ambient** — floor material is genuinely UNLIT (cc=0x0701 en=0, decomp bit layout). An
  unlit channel ignores ambient. The `SUNBRIGHT_NGX_AMBMUL` probe "worked" (235→128) only because ANY
  ~0.5 multiply lands near 105 — it did NOT prove ambient (removed; it tested a falsified mechanism).
- **xfmem cc=070f (lit)** — UNSTABLE across curls (en=0 / en=1 / en=1) = the documented async-lag trap.
- **TEV combiner** — GX bpmem combiner is async noise; when it settles it = ngx's 08f8af (PASS).
- **Vertex color / texture decode / fog** — vtx white (re-parsed DL); texture bright cream (CMPR
  decoded by hand); GXSetFog sync tee = GX_FOG_NONE.

## THE FIX (own-it-natively, IN PROGRESS / next)
ngx must LOAD THE AUTHORED MIP CHAIN for mesh textures and sample trilinear. In `vk_mesh.cpp`:
1. `NgxTexBind`: add `mip_count` (read ResTIMG `mipmapCount` @ TIMG+0x18 in `capture_textures`,
   `ngx_j3d_shape.cpp`; also `max_lod`@0x17).
2. `decode_bind`: decode levels 0..mip_count-1 into staging; advance the GUEST source by the
   **tile-PADDED** mip size, NOT `sb_tex_size_bytes` (which is unpadded — it returns 8 for a 4×4 CMPR
   level but the guest stores a full 8×8 tile = 32 B). Add `sb_tex_mip_stride_bytes(w,h,fmt)` that pads
   w→ceil(w/tw)*tw, h→ceil(h/th)*th per format tile (CMPR/I4/C4=8×8; I8/IA4/C8=8×4; 16-bit/C14X2/RGBA8
   =4×4). The decode already reads full tiles, so decode each level at its padded dims into an RGBA
   buffer of the LOGICAL w×h (decode uses width as dst row stride; padded source is consumed by the
   tile loop). Record per-level staging offset + logical w/h.
3. `make_dev_image` + the per-texture image: `mipLevels = mip_count`; image view `levelCount`.
4. Upload: one `VkBufferImageCopy` per level (mipLevel=l, imageExtent = level dims). The pre/post
   barriers must cover `levelCount = mip_count` (the `barrier` lambda hardcodes 1).
5. Sampler: `mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR`, `maxLod = VK_LOD_CLAMP_NONE`, min/mag from
   the TIMG GXTexFilter.
VERIFY: `SUNBRIGHT_BIN=build-freshtest/sunbright tools/render/oracle_ab.sh 14` — floor rows 3-4 delta
(~120 now) MUST drop; floor should go blue-grey. Don't eyeball — the number must move.

## Tools (reliable, reusable)
- `tools/render/oracle_ab.sh 14` (SUNBRIGHT_BIN=build-freshtest/sunbright) — frame-exact GX vs ngx A/B.
- `runtime/overrides/shadow_kill_diag.cpp` (`SUNBRIGHT_KILL_SHADOW=1`) — no-op the cast shadow.
- Hand CMPR-mip decoder pattern (python over `/r?a=&n=`): RGB565 endpoints per 8-byte block; mip L
  guest offset = Σ tile-padded sizes of levels < L.
- Floor pixel sampling: oracle ≈ (101,106,112), ngx ≈ (250,250,250) at NDC (0,-0.7..-0.85),
  shape 8112bfdc, DL @ sh+0x38→draws[0]+8 = 80c1dd20 (size 0xda0), CLR0 array @80b9b600 (idx1=white).
