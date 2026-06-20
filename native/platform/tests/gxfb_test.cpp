// gxfb_test.cpp — TDD harness for the GX seam SLICE 6 (framebuffer/management half:
// gx_fb_impl.cpp). Asserts the pure-math helpers (texture buffer size, XFB-line / Y-scale
// formulas — ported verbatim from the decomp) against HAND-COMPUTED ground truth, and the
// state-capture setters (fog/pixfmt/dstAlpha/dispcopy/tlut/drawsync/vtxattrfmtv + the
// texobj CI getters) by round-trip. Verify-first: every number here is spec-derived.

#include <dolphin/gx.h>
#include "gx_state.h"
#include <cstdio>
#include <cmath>

using sb::platform::gx::state;

static int g_fail = 0, g_checks = 0;
static void chku(u32 got, u32 want, const char* what) {
    ++g_checks;
    if (got != want) { ++g_fail; std::printf("  FAIL: %s got=%u want=%u\n", what, got, want); }
}
static void chkf(f32 got, f32 want, const char* what, f32 eps = 1e-4f) {
    ++g_checks;
    if (std::fabs(got - want) > eps) {
        ++g_fail; std::printf("  FAIL: %s got=%.5f want=%.5f\n", what, got, want);
    }
}

// --- pure math: GXGetTexBufferSize -----------------------------------------
// Hand-derived: nx=ceil(w/tileW), ny=ceil(h/tileH), size=nx*ny*tileBytes.
//  RGBA8 (tile 4x4, 64B): 32x32 -> 8*8*64 = 4096
//  I4    (tile 8x8, 32B): 64x64 -> 8*8*32 = 2048
//  RGB565(tile 4x4, 32B): 16x16 -> 4*4*32 = 512
//  CMPR  (tile 8x8, 32B): 64x64 -> 8*8*32 = 2048
//  I8    (tile 8x4, 32B): 40x20 -> ceil(40/8)=5, ceil(20/4)=5 -> 5*5*32 = 800
static void test_tex_buffer_size() {
    chku(GXGetTexBufferSize(32, 32, GX_TF_RGBA8, 0, 0), 4096, "texbufsize RGBA8 32x32");
    chku(GXGetTexBufferSize(64, 64, GX_TF_I4,    0, 0), 2048, "texbufsize I4 64x64");
    chku(GXGetTexBufferSize(16, 16, GX_TF_RGB565,0, 0), 512,  "texbufsize RGB565 16x16");
    chku(GXGetTexBufferSize(64, 64, GX_TF_CMPR,  0, 0), 2048, "texbufsize CMPR 64x64");
    chku(GXGetTexBufferSize(40, 20, GX_TF_I8,    0, 0), 800,  "texbufsize I8 40x20");
}

// --- pure math: XFB lines / Y-scale ----------------------------------------
// scale=(256/yScale)&0x1FF; numLines=(h-1)*256; actualHeight=numLines/scale+1.
//  h=448, yScale=1 -> scale=256, (447*256)/256+1 = 447+1 = 448
//  h=480, yScale=1 -> 480
static void test_xfb_lines() {
    chku(GXGetNumXfbLines(448, 1.0f), 448, "xfb lines 448@1.0");
    chku(GXGetNumXfbLines(480, 1.0f), 480, "xfb lines 480@1.0");
    // GXSetDispCopyYScale reads the recorded EFB source height (GXSetDispCopySrc).
    GXSetDispCopySrc(0, 0, 640, 448);
    u32 lines = GXSetDispCopyYScale(1.0f);
    chku(lines, 448, "dispcopy yscale lines");
    chkf(state().dispCopy.yScale, 1.0f, "dispcopy yscale recorded");
    chku(state().dispCopy.yScaleLines, 448, "dispcopy yscale lines recorded");
    // Identity fit: efb==xfb -> factor 1.0.
    chkf(GXGetYScaleFactor(448, 448), 1.0f, "yscale factor 448->448");
    chkf(GXGetYScaleFactor(480, 480), 1.0f, "yscale factor 480->480");
}

// --- GXSetVtxAttrFmtv: vector form fans out to the per-attr table -----------
static void test_vtx_attr_fmtv() {
    static const GXVtxAttrFmtList list[] = {
        { GX_VA_POS, GX_POS_XYZ, GX_F32, 0 },
        { GX_VA_TEX0, GX_TEX_ST, GX_S16, 8 },
        { GX_VA_NULL, GX_POS_XYZ, GX_F32, 0 },   // terminator
    };
    GXSetVtxAttrFmtv(GX_VTXFMT0, list);
    auto& pos = state().vtxAttrFmt[GX_VTXFMT0][GX_VA_POS];
    chku(pos.cnt, GX_POS_XYZ, "vtxattrfmtv pos cnt");
    chku(pos.type, GX_F32, "vtxattrfmtv pos type");
    auto& tex = state().vtxAttrFmt[GX_VTXFMT0][GX_VA_TEX0];
    chku(tex.cnt, GX_TEX_ST, "vtxattrfmtv tex0 cnt");
    chku(tex.frac, 8, "vtxattrfmtv tex0 frac");
}

// --- texobj CI + getters ----------------------------------------------------
static void test_texobj_ci() {
    GXTexObj obj{};
    int dummy_image = 0;
    GXInitTexObjCI(&obj, &dummy_image, 64, 32, GX_TF_C8, GX_REPEAT, GX_CLAMP, GX_FALSE, 3);
    chku(GXGetTexObjWidth(&obj), 64, "texobj CI width");
    chku(GXGetTexObjHeight(&obj), 32, "texobj CI height");
    void* img = nullptr; u16 w = 0, h = 0; GXTexFmt fmt{};
    GXTexWrapMode ws{}, wt{}; u8 mip = 9;
    GXGetTexObjAll(&obj, &img, &w, &h, &fmt, &ws, &wt, &mip);
    chku((u32)(img == &dummy_image), 1, "texobj CI image ptr");
    chku(w, 64, "texobj CI all width");
    chku((u32)fmt, GX_TF_C8, "texobj CI format");
    chku((u32)ws, GX_REPEAT, "texobj CI wrap_s");
}

// --- TLUT init + load -------------------------------------------------------
static void test_tlut() {
    GXTlutObj tobj{};
    int palette[16] = {0};
    GXInitTlutObj(&tobj, palette, GX_TL_RGB565, 16);
    GXLoadTlut(&tobj, 5);
    auto& b = state().tlut[5];
    chku((u32)b.valid, 1, "tlut bound valid");
    chku(b.nEntries, 16, "tlut nEntries");
    chku((u32)(b.lut == palette), 1, "tlut lut ptr");
    chku(b.fmt, GX_TL_RGB565, "tlut fmt");
}

// --- semantic state setters round-trip -------------------------------------
static void dummy_drawsync_cb(u16) {}
static void test_state_setters() {
    GXColor fc = { 10, 20, 30, 40 };
    GXSetFog(GX_FOG_LIN, 100.0f, 5000.0f, 1.0f, 10000.0f, fc);
    auto& f = state().fog;
    chku((u32)f.type, GX_FOG_LIN, "fog type");
    chkf(f.startz, 100.0f, "fog startz");
    chkf(f.endz, 5000.0f, "fog endz");
    chku(f.color.g, 20, "fog color g");

    GXSetDstAlpha(GX_TRUE, 0x80);
    chku((u32)state().dstAlphaEnable, GX_TRUE, "dstAlpha enable");
    chku(state().dstAlpha, 0x80, "dstAlpha value");

    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    chku((u32)state().pixFmt, GX_PF_RGBA6_Z24, "pixel fmt");
    chku((u32)state().zFmt, GX_ZC_LINEAR, "z fmt");

    GXSetAlphaUpdate(GX_TRUE);
    chku((u32)state().alphaUpdate, GX_TRUE, "alpha update");

    GXSetZTexture(GX_ZT_ADD, GX_TF_Z16, 0x1234);
    chku(state().zTex.bias, 0x1234, "ztexture bias");

    // GXSetDrawSyncCallback returns the PREVIOUSLY registered callback (swap semantics).
    GXSetDrawSyncCallback(nullptr);                              // reset
    GXDrawSyncCallback prev1 = GXSetDrawSyncCallback(dummy_drawsync_cb);
    chku((u32)(prev1 == nullptr), 1, "drawsync cb prev was null");
    GXDrawSyncCallback prev2 = GXSetDrawSyncCallback(nullptr);
    chku((u32)(prev2 == dummy_drawsync_cb), 1, "drawsync cb prev was ours");

    GXSetDrawSync(0xABCD);
    chku(state().drawSyncToken, 0xABCD, "drawsync token");
}

// --- NTSC 480i render-mode data symbol --------------------------------------
static void test_render_mode_data() {
    chku(GXNtsc480Int.fbWidth, 640, "ntsc480 fbWidth");
    chku(GXNtsc480Int.efbHeight, 480, "ntsc480 efbHeight");
    chku(GXNtsc480Int.xfbHeight, 480, "ntsc480 xfbHeight");
    chku(GXNtsc480Int.viWidth, 640, "ntsc480 viWidth");
}

int main() {
    test_tex_buffer_size();
    test_xfb_lines();
    test_vtx_attr_fmtv();
    test_texobj_ci();
    test_tlut();
    test_state_setters();
    test_render_mode_data();
    std::printf("gxfb_test: %d/%d checks passed%s\n", g_checks - g_fail, g_checks,
                g_fail ? "  *** FAILURES ***" : "");
    return g_fail ? 1 : 0;
}
