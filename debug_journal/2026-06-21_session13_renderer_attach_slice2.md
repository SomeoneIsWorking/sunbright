# Session 13 — renderer-attach SLICE 2: immediate-mode 2D capture + render in sms-boot

Continues session 12 (renderer-attach SLICE 1 = present pipeline + clear color). User
direction stands: attach the native renderer to sms-boot so the movie/wipe/scenes become
visually verifiable.

## DONE (committed + pushed: parent main @45dea30, submodule sunbright @59079ae)
SLICE 2: the GameCube **immediate-mode** draw API (the fader / GC-logo / J2D HUD) is now
captured at the GX seam and composited over the clear, so the boot's 2D output is on-screen.

### How it works
The immediate writers (`GXBegin`/`GXPosition*`/`GXColor*`/`GXEnd`) are INLINE in
`reference/sms/include/dolphin/gx/GXVert.h` and wrote the (nonexistent) write-gather FIFO
sink. Under `SMS_NATIVE_PLATFORM` they now call `sb_gx_imm_*` capture hooks:
- `native/render/gx_imm_xform.h` (PURE, unit-tested): model → eye (current pos matrix) →
  SCREEN via the **EXACT decomp GXProject** (projection AND viewport) → Vulkan NDC, plus
  GX_QUADS/TRIANGLES/STRIP/FAN triangulation. Going through the real GXProject reproduces
  SMS's quirky `C_MTXOrtho` arg order (it passes `fbWidth` into the t/b slot and
  `efbHeight` into l/r — verified verbatim against `mtx44.c`) with NO special-casing.
- `native/platform/gx_imm_impl.cpp`: the capture hooks. Snapshots proj/viewport/pos-matrix
  at `GXBegin`, builds+transforms+triangulates verts, hands the frame's Vulkan-NDC triangle
  list to the present via `sb_gx_imm_take`. Single-threaded (present fires at frame end on
  the draw thread — `VIWaitForRetrace` → present, synchronous). Lazy clear on the first
  `GXBegin` AFTER a present consumed, so multi-retrace frames (waitForRetrace loops) render
  the same accumulated content. `SB_GX_IMM_DBG=1` dumps the first captured verts.
- `gx_impl.cpp` GXBegin opens the primitive; `GXGeometry.h` GXEnd closes it (native branch).
- `sms_boot_present.cpp`: clear to GXSetCopyClear, then draw the captured imm tris.
- `nvk.cpp`: enabled standard alpha-over blend (GX_BM_BLEND SRCALPHA/INVSRCALPHA) on the
  triangle pipeline — a NO-OP for opaque (a=1) geometry (all 3D/opaque tests unaffected),
  lets partial-alpha overlays composite.

### ROOT-CAUSE FIX (the big one): TColor endianness — `reference/sms/include/JSystem/JUtility/JUTColor.hpp`
`TColor::toUInt32()` was `return *(u32*)&r;` — an endian-dependent type-pun. GXColor is
`{u8 r,g,b,a}`; reading it as u32 gives **0xRRGGBBAA on the big-endian GC** (what
`GXColor1u32` hardware + source literals expect) but **0xAABBGGRR on a little-endian PC**,
so the alpha byte landed in the R channel and **every colour was scrambled** (the fader's
black-with-animating-alpha rendered as animating RED). Rewrote `toUInt32`/`set(u32)`
byte-explicit (0xRRGGBBAA) — portable, matches the GC result on both ends. This is a
latent bug for the WHOLE native port, not just the fader. (Only 2 call sites: the operator
and its inverse; self-consistent and now hardware-correct.)

### Verification (verify-first)
- `sms-gx_imm_test` (render_test): pixel→NDC ortho corners, quad triangulation, end-to-end
  render + alpha-over blend → **12/12**. `ctest -E platform_test` → **22/22**.
- Live: `SUNBRIGHT_DISC=scratch/disc/sms.iso SB_FRAME_DUMP=1 SB_FRAME_DUMP_MAX=20 ./build-native/sms-boot`
  → 20 PPMs in `scratch/frames/`. The boot's 2D box renders **blue (0,38,137)** at the
  captured centre location (133,170)-(509,274). Correct sensible colour = the endianness
  fix confirmed (was scrambled red pre-fix). PNG: `scratch/frames/boot_0019.png`.
- The full-screen fader quad captures correctly but is BLACK-over-black at this boot state
  (movie stuck STATE_FADE_IN, GXSetCopyClear=black) so it's invisible by design — expected.

## STATE / NEXT
Boot still runs the gameLoop, stuck at the Hx_ wipe stub (STATE_FADE_IN). Build:
`cmake -S native -B build-native -DSMS_BUILD_BOOT=ON -DCMAKE_BUILD_TYPE=Release` (note `-S
native`), `cmake --build build-native --target sms-boot -j`.

NEXT (handoff order): **port the Hx_ wipe library** (fully RE'd in the session-12 journal:
state struct G=0x803f43c0; Hx_StartWipe/UpdateWipe/GetWipeType/TimerCountDown; type-12
m-mark 9-phase machine; point list @0x803c1320; Hx_MovieStartSyncEx gates THP play on
phase>=6). Port its STATE/timer/phase logic faithfully (timer/data-driven, verifiable by
SB_MOVIE_DBG advancing), no-op the GPU draws — now SEEABLE via the renderer. Then THP video
(reference/sms/src/THPPlayer/). SLICE 3 (J3D scene capture) needs gameplay/TMarDirector,
which requires the wipe/movie to advance first.
