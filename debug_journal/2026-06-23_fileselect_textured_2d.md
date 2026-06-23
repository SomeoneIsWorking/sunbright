# 2026-06-23 — File-select port (boot order), milestone 3: TEXTURED 2D renders

Continues `2026-06-23_fileselect_menu_windows.md` (milestone 2: menu windows render as
SOLID WHITE — the immediate-mode 2D path captured position+colour only). This milestone
adds textured 2D capture + decode + render to the native sms-boot path, so the J2D panes
(stage-name banner, score/coin/shine HUD, episode text, window borders) render with their
textures instead of white. USER directive: port in BOOT ORDER (`port-in-boot-order-not-delfino`).

## Verified result
`scratch/frames/menu_textured.png` (frame 402, SB_FILESELECT=1 SB_STAGE=4): renders
"BIANCO HILLS" (the stage-name J2DPicture), "EPISODE 1" text, and the "SCORE 0 x100 ☀ ???
☀ ☀" HUD row with coin + shine icons — all decoded from their GX textures and modulated by
vertex colour. Was solid-white boxes before. ctest -E platform_test = 28/28.

## What landed
Two halves.

### 1. Textured immediate-mode 2D capture (native/platform/gx_imm_impl.cpp + GXVert.h)
The gx_imm seam now captures texcoords and the bound texmap-0, and groups the frame's
triangles into per-texture BATCHES (`SbImmBatch` in `native/render/gx_imm_xform.h`):
- `GXTexCoord*` writers (GXVert.h, SMS_NATIVE_PLATFORM) route to `sb_gx_imm_texcoord_*`.
  Float forms pass through (0..1); integer forms (1s16/2u16/...) push raw bits the seam
  dequants via `imm_texcoord_scale(bits, type, frac, width)` using the bound TEX0 vtx-attr
  fmt — J2D's default is (GX_U16, frac 15) (J2DGrafContext), so GXTexCoord1s16(0x8000) →
  1.0, 0x0000 → 0.0 (the corner UVs J2DPicture/J2DWindow emit). 1-component forms pair
  S then T per vertex (reset on each GXPosition). Pure + unit-tested (gx_imm_test, +5 checks).
- `GXBegin` now passes the vtxfmt to `sb_gx_imm_begin(prim, vtxfmt)` so the seam can read
  the TEX0 attr fmt. The bound texmap-0 (gx_state `boundTex[0]`, extended with magFilt +
  tlutName) is snapshotted at GXBegin; a prim is TEXTURED iff it emitted texcoords AND a
  texture was bound (so a stale binding can't texture-ize the texcoord-less gradient, which
  sets TEX0=GX_NONE). `push_batch` coalesces consecutive same-texture prims.
- `J2DWindow::Texture::draw` writes each vertex colour with `GXParam1s32(-1)` (= 0xFFFFFFFF
  white) not GXColor1u32 — same 32-bit FIFO word on HW. GXVert.h now routes GXParam1s32 to
  `sb_gx_imm_param_color_s32` which treats it as the DIRECT-CLR0 colour inside a GXBegin
  (else it's a raw FIFO word → ignored, the native renderer has no FIFO).
- `sms_boot_present.cpp` consumes `sb_gx_imm_take_batches`: each batch → an NvkTevBatch;
  textured ones decode the bound GX texture once (cached by image ptr; sb_tex_decode at
  block-padded dims, copy logical w×h to avoid the padding-column UV leak), bind it, and use
  a GX_MODULATE TEV shader (tex0 × rasterColor); untextured ones keep the colour-only
  passthrough. All 2D draws: depth off, SRCALPHA/INVSRCALPHA blend, on top of the 3D scene.

### 2. ROOT CAUSE: J2D ResTIMG headers were big-endian (the SEGV)
First decode crashed in sb_tex_decode: img=0x80000395a7c0, fmt=I4 2048×2048. 2048 = 0x0800
= byteswap of 8 (0x0008) — the 'TIMG' resources a .blo references are RAW BIG-ENDIAN on disc
and were never swapped to host. JUTTexture::initTexObj reads width/height/imageDataOffset
straight from the ResTIMG → garbage dims + a wild texel pointer. BMD-embedded TIMGs get this
via bmd_swap (TEX1); the standalone J2D ones did not — and the textured-2D path was the
FIRST consumer to read them, so it bit. Fix (faithful swap-at-load, the established pattern):
`native/assets/timg_swap.{h,cpp}` (`smsport::assets::restimg_swap_to_host`) swaps the ResTIMG
header in place ONCE per pointer; wired into `JUTResReference::getResource` for the 'TIMG'
tag (the single archive chokepoint — never touches the host-built ResTIMG from the
JUTTexture(w,h,fmt) ctor, which never goes through getResource). Field offsets/scope match
bmd_swap.cpp's swap_TEX1 ResTIMG case (width@2 height@4 numColors@A paletteOffset@C
lodBias@1A imageDataOffset@1C). After the fix dims are sane (8×8, RGB5A3 80×20, IA4 36×36,
banner 454×50) and pointers host-side (0x7fffe3...).

## RESIDUAL / NEXT (the next unit)
The render is textured but the menu is static-at-.blo-defaults (milestone 2 deferred the
per-file save-data population + open animation). Visible residuals: "EPISODE 1 EPISODE 1"
appears doubled/overlapping (default panes, no per-file data), a bright green vertical strip
at the right edge, and the blue window-border 9-slice isn't prominent. These are the
perform-calc work, not the texture path:
- TSelectMenu::perform calc (the 10-state window-open animation, cases 0-9 + input nav) —
  not yet ported; per-file panes (sc_number/coin_number/sc_mark visibility from
  TFlagManager) populated in setup's deferred ~3.5KB block.
- TSelectShineManager (ct 0x80178eb4) + JPA particles, changeOrder draw-order masks, then
  file-pick → APP_STATE_GAMEPLAY.
Paletted (CI) J2D textures: restimg_swap swaps numColors/paletteOffset, but the TLUT
(ResTLUT) header itself isn't swapped yet and the present gate falls back to colour-only for
a paletted texmap with no resolved TLUT — none observed in file-select (all I4/IA4/RGB5A3).

## Verify loop
    cmake --build build-native --target sms-boot -j$(nproc)
    ctest --test-dir build-native -E platform_test          # 28/28
    pkill -9 -x sms-boot; (timeout 110 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso \
      SB_THP_FAST=1 SB_WATCHDOG_SECS=0 SB_HOST_ALLOC_CAP_MB=3072 SB_FILESELECT=1 SB_STAGE=4 \
      SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=400 SB_FRAME_DUMP_MAX=3 ./build-native/sms-boot \
      > scratch/frames/fs.log 2>&1 &); sleep 95; pkill -9 -x sms-boot
    # boot_0402.ppm → PIL preview. SB_J3D_DBG=1 traces [imm-tex] decode descriptors.
