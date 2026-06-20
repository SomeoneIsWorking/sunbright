// gx_test.cpp — TDD harness for the GX seam SLICE 1 (transform: gx_impl.cpp).
//
// Verifies state capture/readback (GXSetProjection/Viewport/Scissor -> GXGetProjectionv/
// GXGetViewportv) and the full eye->screen transform (GXProject) against hand-computed
// spec values. The captured state is exactly what GXProject + the renderer consume.

#include <dolphin/gx.h>
#include "gx_state.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0, g_checks = 0;
static void chkf(f32 got, f32 want, const char* what, f32 eps = 1e-3f) {
    ++g_checks;
    if (std::fabs(got - want) > eps) {
        ++g_fail;
        std::printf("  FAIL: %s got=%.4f want=%.4f\n", what, got, want);
    }
}

static void test_state_roundtrip() {
    // Orthographic projection matrix (4x4) — only the entries the SDK reads matter.
    f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    f32 pm[7];
    GXGetProjectionv(pm);
    chkf(pm[0], (f32)GX_ORTHOGRAPHIC, "projType readback");
    chkf(pm[1], 1.0f, "projMtx0 = m00");
    chkf(pm[2], 0.0f, "projMtx1 = m03 (ortho)");
    chkf(pm[5], 1.0f, "projMtx4 = m22");

    GXSetViewport(0, 0, 640, 480, 0.0f, 1.0f);
    f32 vp[6];
    GXGetViewportv(vp);
    chkf(vp[2], 640.0f, "viewport width readback");
    chkf(vp[3], 480.0f, "viewport height readback");
    chkf(vp[5], 1.0f, "viewport farz readback");

    GXSetScissor(10, 20, 300, 200);
    chkf((f32)sb::platform::gx::state().scWd, 300.0f, "scissor wd captured");
    chkf((f32)sb::platform::gx::state().scLeft, 10.0f, "scissor left captured");
}

static void test_project_ortho() {
    // Ortho identity projection, 640x480 viewport [0..1] depth, identity model-view.
    f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, 640, 480, 0.0f, 1.0f);
    f32 pm[7], vp[6];
    GXGetProjectionv(pm);
    GXGetViewportv(vp);
    f32 mv[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};  // identity model-view

    f32 sx, sy, sz;
    // center point -> viewport center (320,240), z=-1 -> screen depth 0.
    GXProject(0, 0, -1, mv, pm, vp, &sx, &sy, &sz);
    chkf(sx, 320.0f, "center sx");
    chkf(sy, 240.0f, "center sy");
    chkf(sz, 0.0f, "center sz (z=-1, ortho)");

    // off-center (0.5, 0.25) -> (480, 180): +x right, +y up (screen y flips).
    GXProject(0.5f, 0.25f, -1, mv, pm, vp, &sx, &sy, &sz);
    chkf(sx, 480.0f, "offcenter sx");
    chkf(sy, 180.0f, "offcenter sy");
}

static void chki(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) { ++g_fail; std::printf("  FAIL: %s got=%ld want=%ld\n", what, got, want); }
}

static void test_pipeline_state() {
    auto& g = sb::platform::gx::state();
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    chki(g.blendType, GX_BM_BLEND, "blend type");
    chki(g.blendSrc, GX_BL_SRCALPHA, "blend src");
    chki(g.blendDst, GX_BL_INVSRCALPHA, "blend dst");

    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    chki(g.zCompare, GX_TRUE, "z compare");
    chki(g.zFunc, GX_LEQUAL, "z func");
    chki(g.zUpdate, GX_TRUE, "z update");

    GXSetCullMode(GX_CULL_BACK);
    chki(g.cullMode, GX_CULL_BACK, "cull mode");
    GXSetColorUpdate(GX_FALSE);
    chki(g.colorUpdate, GX_FALSE, "color update");
    GXSetZCompLoc(GX_TRUE);
    chki(g.zCompLocBeforeTex, GX_TRUE, "zcomploc");

    GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
    chki(g.alphaComp0, GX_GREATER, "alpha comp0");
    chki(g.alphaRef0, 128, "alpha ref0");
    chki(g.alphaOp, GX_AOP_AND, "alpha op");

    GXColor clr = { 10, 20, 30, 255 };
    GXSetCopyClear(clr, 0xFFFFFF);
    chki(g.copyClearColor.r, 10, "clear r");
    chki(g.copyClearColor.g, 20, "clear g");
    chki(g.copyClearColor.b, 30, "clear b");
    chki((long)g.copyClearZ, 0xFFFFFF, "clear z");

    GXSetNumChans(2); GXSetNumTexGens(3); GXSetNumTevStages(4);
    chki(g.numChans, 2, "num chans");
    chki(g.numTexGens, 3, "num texgens");
    chki(g.numTevStages, 4, "num tev stages");
}

int main() {
    std::printf("== GX seam (transform + pipeline slices) unit tests ==\n");
    test_state_roundtrip();
    test_project_ortho();
    test_pipeline_state();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
