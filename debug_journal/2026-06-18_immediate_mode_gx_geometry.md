# Immediate-mode GX geometry in ngx — GXDrawCube (Mario occlusion probe)

2026-06-18, "keep porting." ngx renders from the J3D object model; the game's immediate-mode GX
primitives (GXDraw.c: GXDrawCube / GXDrawSphere) have NO J3D object, so the J3DShape capture path
misses them entirely. First member ported: **GXDrawCube** (0x803627fc), which fires ~1470×/run in
fastboot plaza from TMario::draw — the Mario occlusion probe (MarioMain.cpp ~195).

## What was built (committed)
- `runtime/ngx/ngx_imm_geom.h` — pure model-space geometry for GXDrawCube: the 24 GX_QUADS corners
  (each (±k,±k,±k), k=1/√3) per GXDrawCubeFace, + quad→triangle indices. render_test unit `imm_cube`
  asserts it against the GXDraw.c spec (9/9 units pass). This IS the shipping geometry (the override
  calls it).
- `runtime/overrides/imm_geom_native.cpp` — taps GXSetColorUpdate(0x80361ed4)/GXSetAlphaUpdate
  (0x80361f14)/GXSetDstAlpha(0x8036215c) for the live write-mask state, and overrides GXDrawCube
  (0x803627fc): hands the masks to ngx, runs the original (Dolphin GP consistency). Gated on
  SUNBRIGHT_NGX_PRESENT (under the Dolphin-GX baseline the real cube draws). First slice handles the
  alpha-only OCCLUSION PROBE (colour writes off); the visible silhouette (MarioMain ~219, a non-white
  matColor + dst-alpha blend) is left un-emitted (no regression — it was never drawn) for a follow-up.
- `ngx_emit_imm_cube` (ngx_j3d_shape.cpp) — builds the 24 corners, transforms by the current GX_PNMTX0
  (= boxDrawPrepare's viewMtx×scale×translate, captured synchronously in g_posmtx[0..2] at
  GXLoadPosMtxImm), projects via g_proj, frustum-clips, and emits ONE batch into the same snap/batch
  pipeline as J3D shapes, with a synthetic PASSCLR TEV state.
- NgxPEState gained `color_mask_off` / `alpha_mask_off` (default 0 = write RGBA → every J3D batch
  unchanged). vk_mesh.cpp AND ngx_present.cpp pipeline builders turn them into the Vulkan
  colorWriteMask. The const dst-alpha rides the PASSCLR vertex alpha (no shader-gen change needed).

## VERIFIED (deterministic, not eyeballed)
- `SUNBRIGHT_IMM_SHOW=1` forces the cube VISIBLE (opaque red) → it paints a ~13.5k-px red box on
  Mario near screen centre (geometry + current-matrix transform + projection + frustum clip + back-cull
  all correct). Without SHOW the colour-masked occlusion box is INVISIBLE (default red ≈ baseline ~300
  px → no regression). This is the falsifiable test for the port.
- render_test `imm_cube` green (spec-checked corners/winding).

## ⚠ THE SWAP-TABLE BUG (cost ~a dozen iterations — record so it never recurs)
A synthetic `NgxTevState st{}` leaves `swap_table[4] = {0,0,0,0}`. TEV swap id **0 decodes to "rrrr"**
(R broadcast), NOT identity. So the PASSCLR raster read `col0.rrrr` clobbered ALPHA with R — the const
dst-alpha never landed, and under IMM_SHOW the "red" rendered as WHITE (so it looked like nothing
rasterized; I chased depth/cull/pipeline/upload for hours). **Any hand-built NgxTevState MUST set
`swap_table[i] = 0x1B` (identity "rgba").** Default GX/J3D table id is 0x1B; only a real material that
sets it gets non-identity. (Fixed: ngx_emit_imm_cube sets all four to 0x1B.)

## OPEN — alpha 0x10 does NOT survive into the readback (the GXPeekARGB blocker, as the handoff predicted)
The occlusion box writes a constant framebuffer ALPHA 0x10 (GXSetDstAlpha(ENABLE,0x10)); TMario::
drawSyncCallback then GXPeekARGB's Mario's centre and tests `(argb&0xff000000)==0x10000000`. To serve
that, ngx's framebuffer alpha at the box pixels must read 0x10.
- The cube's PASSCLR FRAGMENT SHADER is **verified correct** (dumped the GLSL): `prev.a = rastemp.a =
  col0.a = round(0.0627*255) = 16`, `o.a = 16/255` → fragment alpha = 0x10. Geometry renders (the red
  box proves the fragment runs). colorWriteMask includes A; blend off.
- YET the readback (g_efb_color, captured from the present colour target inside PresentRenderer::render
  after the 3D pass) shows **alpha = 0xFF across the whole 3D region, 0x10:0 always** — even under
  IMM_SHOW where the cube's RED survives at ~13.5k px, the ALPHA at those exact pixels is 0xFF, and the
  0-alpha box's alpha 0 doesn't land either (=0:0). So the fragment writes 0x10 but the **alpha channel
  of the present target reads 0xFF in the 3D region regardless of what the fragment writes**.
- Ruled out: shader (verified), masks (verified), depth/cull (visible box at default cull/depth),
  vertex upload (in_range, nv=203k > vstart=140k), interp60 (off → unchanged), pollution (returns early
  in plaza, no fullscreen alpha clear), epoch filter (cube epoch 0 = display_epoch). draw_pollution's
  fullscreen "clear EFB alpha" pass does NOT run when pollution is inactive (plaza).
### SHARPENED DIAGNOSIS (session 2) — the 3D render pass writes NO alpha to the readback
Pinned it precisely with targeted probes (all gated on SUNBRIGHT_DBG_EFB, kept in-tree):
- `[efb] cube red px=N alpha[min/max/mean]` — the alpha at the cube's OWN pure-red pixels (IMM_SHOW).
  Result: **13k red px, alpha = 255 (min=max=mean) — every one.** So at the exact pixels the cube's
  RGB demonstrably landed, the alpha is the clear value, NOT the fragment's. Forcing the cube vertex
  alpha to 0.5 (0x80) → still 255. So the cube's alpha write is dropped at the target.
- The cube PIPELINE is built with **colorWriteMask = 0xf (RGBA), blendEnable = FALSE** (confirmed by a
  one-shot print), and alphaToOne is OFF (ms zero-init). So per Vulkan state the alpha MUST be written
  — yet it isn't.
- `/ngxprefix?n=0` (draw ZERO 3D batches) gives the **IDENTICAL** alpha histogram as the full render
  (=0xff:119464 =0:0 other:167256, byte-for-byte). ⇒ the 3D batches contribute NOTHING to the readback
  alpha. The readback alpha = clear + the J2D/HUD pass only. (RGB still differs — 3D RGB lands.)
- `SUNBRIGHT_NGX_CLEARA=0x55` (force the render-pass clear alpha to a sentinel) does NOT change the
  histogram either → the clear alpha doesn't "show through" — every pixel's alpha is set by *something*,
  just never the 3D fragments. The HUD is small corner quads (not fullscreen), so it's not a HUD clobber.

⇒ **The ngx 3D render target's ALPHA channel is not captured by the colour readback** even though RGB
is faithful and the pipeline state says write-RGBA. This is below the TEV/PE/mask layer — a
render-target / framebuffer / Dolphin-VKTexture-or-render-pass property. Likely culprits to check with
RenderDoc or by reading Dolphin's VKTexture/framebuffer creation: the RGBA8 render-target image's
actual format/usage, whether the colour view/attachment drops alpha, or whether the EFB→present copy
path resolves alpha to 1. The colour readback's RGB is correct (GXCopyTex writeback is byte-verified),
so this is specifically an ALPHA-channel-of-the-3D-target issue.

This is the **"own ngx EFB-alpha" frontier** (prior handoff flagged it). Until it's owned, GXPeekARGB
must NOT be wired to the colour readback (it'd force always-occluded) — it stays diagnostic. The const
dst-alpha is plumbed correctly (the cube's fragment outputs 0x10) for when the target alpha is owned.
Probes kept for the next session: `[efb] cube red px … alpha`, `[efb] clearcolor`, SUNBRIGHT_NGX_CLEARA,
SUNBRIGHT_NGX_NOHUD, `[imm] j2d quad` rect log.

### SESSION-2b NARROWED FURTHER — the colour-readback ALPHA does not reflect the rendered frame
More tests (gated SUNBRIGHT_DBG_EFB / SUNBRIGHT_NGX_NOHUD):
- The HUD/J2D quads are all SMALL (logged: counters at corners/top, biggest ~200×22; NO fullscreen
  quad) → not an alpha clobber. `SUNBRIGHT_NGX_NOHUD=1` (skip the HUD entirely) → cube alpha STILL
  0x10:0.
- **NOHUD + `/ngxprefix?n=0` (clear only, zero 3D) vs NOHUD + full 3D → byte-IDENTICAL alpha histogram**
  (=0xff:113251 other:173469). But a single VkClearValue cannot produce a 2-group spatial alpha
  pattern, and the 3D RGB clearly renders (scene visible) — so the readback's ALPHA is a FIXED pattern
  independent of both 3D content AND the clear. ⇒ the colour-readback's alpha CHANNEL is not reflecting
  the rendered attachment at all (RGB is; alpha isn't).
- ⇒ NEXT (5-min test to confirm): clear the render pass to a KNOWN alpha (SUNBRIGHT_NGX_CLEARA=0x77),
  draw nothing, read back — if g_efb_color alpha != 0x77, the readback's alpha is broken/static
  (format/copy issue), NOT a draw problem. Then inspect: the AbstractTexture RGBA8 render-target image
  (PresentRenderer::ensure_target, `g_gfx->CreateTexture(...RGBA8, RenderTarget, Texture_2DArray)`) —
  does Dolphin back it with a format that stores alpha? the framebuffer view? the vkCmdCopyImageToBuffer
  in the COLOR readback (line ~1157) — is it copying the right aspect/all 4 bytes? Suspect the render
  target's alpha plane isn't written/stored, so the readback's 4th byte is static.
- If this turns into a deep Dolphin-VKTexture plumbing fix for a MINOR effect (Mario silhouette through
  walls), it's reasonable to PARK GXPeekARGB and move to the next port (GXDrawSphere for sky scenes, or
  another engine subsystem). The immediate-mode GEOMETRY port (the milestone) is complete + committed.

## Tools / env added
- SUNBRIGHT_IMM_SHOW=1 — force the cube visible (red) for the geometry verify. (Kept; the falsifiable
  test.) SUNBRIGHT_DBG_EFB prints `[imm] cube …` (emit: onscreen corners, tris, NDC bbox, masks) and
  `[imm] present: cube batches seen/drawn`.
- GXDrawSphere (0x80362268) stays a diagnostic counter only (0 calls in plaza; sky is J3D here).
