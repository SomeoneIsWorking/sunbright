// port/pal/gd/gd_stub.cpp
//
// No-op stub layer for the GameCube GD (display-list builder) SDK.
//
// PURPOSE (throwaway, link-only): the decompiled SMS engine (heavily, the J3D
// material/render path) calls the GD API, which records GX register/command
// bytes into a display-list buffer (a GDLObj). Real display-list recording will
// be wired into the native PC renderer later. For now these are empty no-ops so
// `libsmsport_core.a` (and anything that links it) RESOLVES every GD*/__GD*
// symbol with a correctly-typed definition.
//
// Same pattern as port/pal/gx/gx_stub.cpp: we obtain the EXACT signatures by
// including the pristine GD SDK headers (extern "C" linkage, matching the
// engine's expectations bit-for-bit) and then DEFINE each declared function as
// a no-op. Including the headers — rather than re-typing prototypes —
// guarantees parameter types/linkage match, so the symbols actually resolve at
// link time (a silent signature mismatch would leave them unresolved).
//
// NOTE: __GDCurrentDL is a GLOBAL pointer variable (the "current display list"),
// declared `extern GDLObj* __GDCurrentDL;` in <dolphin/gd/GDBase.h> and read by
// the header's inline GDWrite_* helpers (~113 refs across the J3D path). It must
// be DEFINED here, not stubbed as a function. GDOverflowed is a FUNCTION (the
// overflow handler), not a global flag.
//
// Behavior is intentionally nil. Do NOT add real display-list logic here; that
// lives in the native renderer.

#include <dolphin/gd.h>  // pulls every dolphin/gd/*.h sub-header

extern "C" {

// ---------------------------------------------------------------------------
// Global state  (GDBase.h)
//   The "current display list" pointer the GDWrite_* inline helpers append to.
//   A real GD layer points this at an active GDLObj; for link-now it is null.
// ---------------------------------------------------------------------------
GDLObj* __GDCurrentDL = nullptr;

// ---------------------------------------------------------------------------
// Base: display-list object lifecycle / flush / overflow  (GDBase.h)
// ---------------------------------------------------------------------------
void GDInitGDLObj(GDLObj*, void*, u32) {}
void GDFlushCurrToMem(void) {}
void GDPadCurr32(void) {}
void GDOverflowed(void) {}
void GDSetOverflowCallback(GDOverflowCb) {}

// ---------------------------------------------------------------------------
// Geometry / vertex description  (GDGeometry.h)
// ---------------------------------------------------------------------------
void GDSetVtxDescv(const GXVtxDescList*) {}
void GDSetArray(GXAttr, void*, u8) {}
void GDSetArrayRaw(GXAttr, u32, u8) {}
void GDSetGenMode2(u8, u8, u8, u8, GXCullMode) {}

// ---------------------------------------------------------------------------
// Pixel / blend / Z  (GDPixel.h)
// ---------------------------------------------------------------------------
void GDSetBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) {}
void GDSetZMode(unsigned char, GXCompare, unsigned char) {}

// ---------------------------------------------------------------------------
// TEV (texture environment)  (GDTev.h)
// ---------------------------------------------------------------------------
void GDSetAlphaCompare(GXCompare, u8, GXAlphaOp, GXCompare, u8) {}

}  // extern "C"
