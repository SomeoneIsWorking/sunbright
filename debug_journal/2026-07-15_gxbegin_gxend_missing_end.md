# 2026-07-15 — Delfino boot abort: decomp dropped GXEnd() in 3 draw funcs

After the TMapObjTree::initMapObj port cleared its OSPanic, the default-fastboot (Delfino)
boot advanced into rendering and then aborted:

    [aurora FATAL aurora::gx] GXBegin: called without matching GXEnd

## Localized with a new instrument (never debug a black box)

Added `SB_GXBEGIN_TRACE` to aurora `lib/dolphin/gx/GXVert.cpp`: when the `sInBegin` guard
trips, it prints the OPEN primitive's params + the backtrace of the GXBegin that was never
closed (per-begin `backtrace()` capture, env-gated so normal runs pay nothing; sms-boot
already links -rdynamic so names resolve). The traced boot named it exactly:

    unbalanced GXBegin: prim=0x80 (GX_QUADS) vtxFmt=0x00 (4 verts), never GXEnd'd
    JUTResFont::drawChar_scale -> J2DPrint::parse -> J2DTextBox::drawSelf ->
    J2DScreen::draw -> TConsoleStr::perform  (the debug console text overlay)

## Root cause: GXEnd() is a NO-OP MACRO on GC, so the decomp dropped it

On GameCube `GXEnd()` compiles to nothing (the vertex count in GXBegin self-terminates the
primitive), so an omitted `GXEnd();` is invisible in the DOL. Aurora, however, models
GXBegin/GXEnd as a real balanced pair (fifo state + GX_AUTO size patch + vertex-count
validation) and OSPanics on the next GXBegin if one is left open.

Sibling immediate-mode draws all pair 1:1 (beam 2/2, ScrnFader 2/2, J2DWindow 3/3,
J2DScreen 1/1, ...). A per-file count found the anomalies — GXBegin without a matching
GXEnd — in exactly three BUILT files (a 4th, dolphin/gx/GXGeometry.c, is excluded from the
native build; aurora owns GX):

- `JSystem/JUtility/JUTResFont.cpp` — drawChar_scale (the boot blocker; debug console text)
- `JSystem/JUtility/JUTRomFont.cpp` — the ROM-font glyph quad (same shape)
- `Map/PollutionLayer.cpp` — TPollutionLayerWave::draw, the per-z triangle strip (Delfino
  has pollution, so this would panic next)

## Fix (universally correct, no guard)

Restored the canonical `GXEnd();` in each — after the last vertex (fonts) / at the end of
each per-z strip (pollution). This is a no-op on GC and matches every other paired draw
site, so it's a plain decomp-correctness fix, not a native-only shim. This is the same
"decomp dropped a GC-invisible call" bug class as the fused-immediate mBlack constant.

If another `GXBegin without matching GXEnd` ever appears, re-run with `SB_GXBEGIN_TRACE=1`
and grep the per-file GXBegin/GXEnd counts for a new 1/0 file.
