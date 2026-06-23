// gx_imm_test.cpp — SLICE 2 unit test for the GX immediate-mode 2D capture transform.
//
// The fader (ScrnFader.cpp) draws a full-screen GX_QUADS in pixel coordinates under the
// gameLoop's ortho projection (C_MTXOrtho(0,fbW, 0,efbH, -1,1)). This asserts the PURE
// transform (gx_imm_xform.h imm_project) maps those pixel corners to the EXPECTED Vulkan
// NDC (hand-derived), and that triangulation of a quad yields the right 2 triangles.
// Then it renders a captured red quad through nvk and checks the pixels — the same path
// sms_boot_present.cpp uses. Verify-first: this is the number a fader-render must move.

#include "gx_imm_xform.h"
#include "nvk.h"
#include <dolphin/mtx.h>
#include <dolphin/gx.h>
#include "gx_state.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static bool feq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

static void write_ppm(const char* path, const Nvk& n) {
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", n.width(), n.height());
    for (uint32_t y = 0; y < n.height(); ++y) for (uint32_t x = 0; x < n.width(); ++x) {
        const uint8_t* p = n.at(x, y); std::fputc(p[0], f); std::fputc(p[1], f); std::fputc(p[2], f);
    }
    std::fclose(f);
}

int main() {
    // --- a standard 640x480 pixel-space ortho + full-frame viewport (the math the
    // faithful GXProject path must satisfy; the game's quirky arg order is handled by
    // the same path at runtime, verified live via SB_GX_IMM_DBG). C_MTXOrtho(t,b,l,r):
    // X = (l,r) = (0,640), Y = (t,b) = (0,480). ---
    f32 ortho[4][4];
    C_MTXOrtho(ortho, 0.0f, 480.0f, 0.0f, 640.0f, -1.0f, 1.0f);
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, 640, 480, 0.0f, 1.0f);
    float pm6[6], vp[6];
    {   // GXGetProjectionv emits [type, projMtx0..5]; gx_imm uses projMtx[0..5].
        f32 pv[7]; GXGetProjectionv(pv);
        for (int i = 0; i < 6; ++i) pm6[i] = pv[i + 1];
        chk(pv[0] == (f32)GX_ORTHOGRAPHIC, "captured projection is orthographic");
    }
    GXGetViewportv(vp);
    // The fader's PNMTX0 = MTXTrans(0,0,0) = identity translation.
    float posI[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};

    // pixel corner (0,0) -> top-left -> Vulkan NDC (-1,-1); (640,480) -> (1,1).
    SbImmVtx tl = imm_project({0,   0,   0, 1,0,0,1}, GX_ORTHOGRAPHIC, pm6, posI, vp);
    SbImmVtx br = imm_project({640, 480, 0, 1,0,0,1}, GX_ORTHOGRAPHIC, pm6, posI, vp);
    SbImmVtx ce = imm_project({320, 240, 0, 1,0,0,1}, GX_ORTHOGRAPHIC, pm6, posI, vp);
    chk(feq(tl.x, -1) && feq(tl.y, -1), "pixel (0,0) -> NDC (-1,-1) top-left");
    chk(feq(br.x,  1) && feq(br.y,  1), "pixel (640,480) -> NDC (1,1) bottom-right");
    chk(feq(ce.x,  0) && feq(ce.y,  0), "pixel (320,240) -> NDC (0,0) center");
    chk(feq(tl.r, 1) && feq(tl.g, 0) && feq(tl.b, 0), "colour passthrough");

    // --- texcoord dequant (imm_texcoord_scale): the bound TEX0 vtx-attr fmt drives it. ---
    // J2D default is (GX_U16, frac 15): GXTexCoord1s16(0x8000) -> 1.0, 0x0000 -> 0.0 — the
    // corner UVs J2DPicture/J2DWindow emit (handoff RE). (type 2 = GX_U16, 3 = GX_S16.)
    chk(feq(imm_texcoord_scale(0x8000, 2, 15, 16), 1.0f), "U16 frac15 0x8000 -> 1.0");
    chk(feq(imm_texcoord_scale(0x0000, 2, 15, 16), 0.0f), "U16 frac15 0x0000 -> 0.0");
    chk(feq(imm_texcoord_scale(0x4000, 2, 15, 16), 0.5f), "U16 frac15 0x4000 -> 0.5");
    // S16 sign-extends: 0x8000 = -32768 -> -1.0 (not 1.0) — proves type, not writer, decides.
    chk(feq(imm_texcoord_scale(0x8000, 3, 15, 16), -1.0f), "S16 frac15 0x8000 -> -1.0");
    chk(feq(imm_texcoord_scale(0x0100, 0, 8, 16), 1.0f), "U8 frac8 0x100 -> 1.0");

    // --- triangulation: one GX_QUADS quad -> 2 tris (0,1,2)(0,2,3) ---
    SbImmVtx quad[4] = {
        {-1,-1,0, 1,0,0,1}, {1,-1,0, 1,0,0,1}, {1,1,0, 1,0,0,1}, {-1,1,0, 1,0,0,1}
    };
    std::vector<SbImmVtx> tris;
    imm_triangulate(SB_GX_QUADS, quad, 4, tris);
    chk(tris.size() == 6, "quad triangulates to 6 verts (2 tris)");
    if (tris.size() == 6) {
        chk(tris[0].x == quad[0].x && tris[2].x == quad[2].x, "tri0 = (0,1,2)");
        chk(tris[3].x == quad[0].x && tris[5].x == quad[3].x, "tri1 = (0,2,3)");
    }

    // --- end-to-end: render a captured full-screen red quad through nvk ---
    Nvk nvk;
    if (!nvk.init(64, 64) && !nvk.init(64, 64, true)) { std::printf("  FAIL: no Vulkan\n"); return 1; }
    std::printf("  device: %s\n", nvk.deviceName());
    std::vector<NvkVertex> nv(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        nv[i] = NvkVertex{ tris[i].x, tris[i].y, tris[i].z,
                           tris[i].r, tris[i].g, tris[i].b, tris[i].a };
    chk(nvk.renderTriangles(nv, NvkClear{0, 0, 0, 1}), "render captured quad");
    const uint8_t* c = nvk.at(32, 32);
    chk(c[0] > 200 && c[1] < 60 && c[2] < 60, "center pixel is the red quad");
    write_ppm("gx_imm.ppm", nvk);

    // --- alpha-over blend (the fader's GX_BM_BLEND): a BLACK quad at a=0.5 over a RED
    // clear must composite to ~half-red, proving partial-alpha overlays blend. ---
    for (auto& v : nv) { v.r = v.g = v.b = 0.0f; v.a = 0.5f; }
    chk(nvk.renderTriangles(nv, NvkClear{1, 0, 0, 1}), "render half-alpha black over red");
    const uint8_t* cb2 = nvk.at(32, 32);
    chk(cb2[0] > 100 && cb2[0] < 160 && cb2[1] < 30 && cb2[2] < 30,
        "alpha 0.5 black over red -> ~half red (blended)");
    nvk.shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
