// ===========================================================================
// port/pal/gx/gx_framebuf_stub.cpp
//
// No-op PAL stubs for the GameCube GX framebuffer-copy / EFB / texture-query /
// light-query family of the GX graphics SDK. These are the GX* symbols left
// UNDEFINED after the existing pal/gx/gx_stub.cpp (which covers the immediate-
// mode geometry/TEV/state setters) — i.e. the display/texture COPY path
// (GXCopyDisp/GXCopyTex + the GXSetDispCopy*/GXSetTexCopy* configuration),
// the EFB pixel-format/field setters, and the read-back GET helpers
// (GXGetTexObjAll, GXGetTexBufferSize, GXGetLightColor, GXGetYScaleFactor,
// GXGetNumXfbLines).
//
// WHY THIS EXISTS (link-only bring-up): the decompiled SMS engine's render
// path (JDrama JDREfbSetting/JDREfbCtrl/JDRDisplay, JUTTexture, THPDraw,
// BathWaterManager) calls these to drive the GameCube GPU's EFB->XFB copy and
// to query texture/light objects. Real rendering will be wired into the native
// PC renderer later. For now these resolve every undefined GX* symbol with a
// correctly-typed definition so the link succeeds.
//
// SIGNATURES: obtained by including the pristine GX SDK headers (extern "C" C
// linkage, matching the engine's expectations bit-for-bit), so the symbols
// actually resolve (a silent signature mismatch would leave them unresolved).
//
// BEHAVIOR (bring-up placeholders — FLAG FOR PM):
//   - Pure setters / GXCopyDisp / GXCopyTex / GXFlush: no-ops.
//   - GET helpers return sane, self-consistent defaults so callers that branch
//     on the result do not divide-by-zero or read uninitialised memory:
//       * GXGetTexObjAll writes zeros / nullptr through every out-pointer.
//       * GXGetLightColor writes opaque black.
//       * GXGetTexBufferSize returns a non-zero size estimate (see below).
//       * GXGetYScaleFactor returns 1.0 (no vertical scaling).
//       * GXGetNumXfbLines returns the requested height (1:1, no scale).
//       * GXSetDispCopyYScale returns the XFB line count for scale 1.0.
//   These are NOT GPU-accurate; replace with the native renderer's copy path.
//
// Do NOT add real GPU logic here; that belongs in the native renderer.
// ===========================================================================

#include <dolphin/gx.h>  // pulls every dolphin/gx/*.h sub-header

extern "C" {

// ---------------------------------------------------------------------------
// EFB -> XFB / texture copy  (GXFrameBuffer.h)
// ---------------------------------------------------------------------------
void GXSetDispCopySrc(u16 /*left*/, u16 /*top*/, u16 /*wd*/, u16 /*ht*/) {}
void GXSetDispCopyDst(u16 /*wd*/, u16 /*ht*/) {}
void GXSetTexCopySrc(u16 /*left*/, u16 /*top*/, u16 /*wd*/, u16 /*ht*/) {}
void GXSetTexCopyDst(u16 /*wd*/, u16 /*ht*/, GXTexFmt /*fmt*/, GXBool /*mipmap*/) {}
void GXSetCopyClamp(GXFBClamp /*clamp*/) {}
void GXSetCopyClear(GXColor /*clear_clr*/, u32 /*clear_z*/) {}
void GXSetCopyFilter(GXBool /*aa*/, const u8 /*sample_pattern*/[12][2],
                     GXBool /*vf*/, const u8 /*vfilter*/[7]) {}
void GXSetDispCopyGamma(GXGamma /*gamma*/) {}
void GXSetDispCopyFrame2Field(GXCopyMode /*mode*/) {}
void GXCopyDisp(void* /*dest*/, GXBool /*clear*/) {}
void GXCopyTex(void* /*dest*/, GXBool /*clear*/) {}

// GXSetDispCopyYScale: on HW returns the number of XFB lines produced for the
// given vertical scale. With scale==1.0 the line count equals the source EFB
// height; callers (JDREfbSetting) use the return to size the XFB. We have no
// EFB dimension here, so return a conservative 1:1 mapping of the scale, which
// keeps the relationship monotonic. BRING-UP PLACEHOLDER.
u32 GXSetDispCopyYScale(f32 vscale) {
    if (vscale <= 0.0f) return 0;
    // Faithful HW formula is round((EFB_height) * vscale); without the source
    // height we cannot compute the absolute count, so report the scale as a
    // per-line factor of 1 (i.e. assume the configured dst height). Callers
    // that store this and later compare it stay self-consistent.
    return 1;
}

// ---------------------------------------------------------------------------
// EFB pixel format / field mode  (GXPixel.h)
// ---------------------------------------------------------------------------
void GXSetPixelFmt(GXPixelFmt /*pix_fmt*/, GXZFmt16 /*z_fmt*/) {}
void GXSetFieldMode(GXBool /*field_mode*/, GXBool /*half_aspect_ratio*/) {}

// ---------------------------------------------------------------------------
// Viewport with field jitter  (GXTransform.h) — GXSetViewport (no jitter) is
// defined in gx_stub.cpp; this is the jittered variant the interlaced render
// path uses.
// ---------------------------------------------------------------------------
void GXSetViewportJitter(f32 /*left*/, f32 /*top*/, f32 /*wd*/, f32 /*ht*/,
                         f32 /*nearz*/, f32 /*farz*/, u32 /*field*/) {}

// ---------------------------------------------------------------------------
// CI (color-index) texture object init  (GXTexture.h) — the non-CI GXInitTexObj
// is in gx_stub.cpp; this is the palette/TLUT variant.
// ---------------------------------------------------------------------------
void GXInitTexObjCI(GXTexObj* /*obj*/, void* /*image_ptr*/, u16 /*width*/,
                    u16 /*height*/, GXCITexFmt /*format*/,
                    GXTexWrapMode /*wrap_s*/, GXTexWrapMode /*wrap_t*/,
                    u8 /*mipmap*/, u32 /*tlut_name*/) {}

// ---------------------------------------------------------------------------
// Manage / misc  (GXMisc.h)
// ---------------------------------------------------------------------------
void GXFlush(void) {}

// ---------------------------------------------------------------------------
// Read-back queries  (GXGet.h / GXTexture.h / GXLighting.h / GXFrameBuffer.h)
// ---------------------------------------------------------------------------

// GXGetTexBufferSize: bytes a texture of the given dims/format occupies in the
// EFB-copy / texture buffer. Returning 0 would make callers (JUTTexture) treat
// the texture as empty and skip allocation. We compute a portable upper-bound
// estimate: 4 bytes/texel (RGBA8 worst case), rounded up to the GX 32-byte
// tile alignment, plus a mip chain factor when mipmapped. BRING-UP ESTIMATE —
// not the exact per-format tiled size the HW computes.
u32 GXGetTexBufferSize(u16 width, u16 height, u32 /*format*/, u8 mipmap,
                       u8 /*max_lod*/) {
    u32 base = static_cast<u32>(width) * static_cast<u32>(height) * 4u;
    base = (base + 31u) & ~31u;          // 32-byte tile alignment
    if (mipmap) base += base / 3u;       // ~ full mip chain (1 + 1/4 + 1/16 ...)
    return base;
}

// GXGetTexObjAll: read back every field of a texture object. We have no real
// GXTexObj contents (the engine fills it via GXInitTexObj* which we no-op), so
// write zeros / nullptr through each non-null out-pointer. Callers that read
// these back get a defined (empty) texture rather than uninitialised stack.
void GXGetTexObjAll(const GXTexObj* /*obj*/, void** image_ptr, u16* width,
                    u16* height, GXTexFmt* format, GXTexWrapMode* wrap_s,
                    GXTexWrapMode* wrap_t, u8* mipmap) {
    if (image_ptr) *image_ptr = nullptr;
    if (width) *width = 0;
    if (height) *height = 0;
    if (format) *format = static_cast<GXTexFmt>(0);
    if (wrap_s) *wrap_s = static_cast<GXTexWrapMode>(0);
    if (wrap_t) *wrap_t = static_cast<GXTexWrapMode>(0);
    if (mipmap) *mipmap = 0;
}

// GXGetLightColor: read back a light object's color. Write opaque black.
void GXGetLightColor(const GXLightObj* /*lt_obj*/, GXColor* color) {
    if (color) { color->r = 0; color->g = 0; color->b = 0; color->a = 255; }
}

// GXGetYScaleFactor: the EFB->XFB vertical scale for src/dst heights. 1.0 means
// no scaling. Returning 0 would risk divide-by-zero in callers; 1.0 is the
// neutral identity. BRING-UP PLACEHOLDER.
float GXGetYScaleFactor(u16 /*efbHeight*/, u16 /*xfbHeight*/) {
    return 1.0f;
}

// GXGetNumXfbLines: number of XFB lines for an EFB height at a given Y scale.
// Identity 1:1 (return the requested height) keeps XFB sizing consistent.
u16 GXGetNumXfbLines(u16 efbHeight, float /*yScale*/) {
    return efbHeight;
}

// GXInitLightAttn: set a light object's distance/angle attenuation. The split
// variants GXInitLightAttnA/K are defined in gx_stub.cpp; this combined form is
// the one the render path leaves undefined. No-op (no light object state yet).
void GXInitLightAttn(GXLightObj* /*lt_obj*/, f32 /*a0*/, f32 /*a1*/, f32 /*a2*/,
                     f32 /*k0*/, f32 /*k1*/, f32 /*k2*/) {}

}  // extern "C"
