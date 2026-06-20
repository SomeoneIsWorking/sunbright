// gx_test.cpp — TDD harness for the GX seam SLICE 1 (transform: gx_impl.cpp).
//
// Verifies state capture/readback (GXSetProjection/Viewport/Scissor -> GXGetProjectionv/
// GXGetViewportv) and the full eye->screen transform (GXProject) against hand-computed
// spec values. The captured state is exactly what GXProject + the renderer consume.

#include <dolphin/gx.h>
#include "gx_state.h"
#include "ngx_light.h"   // decode_chanctl — the renderer's decoder; cross-check the packing
#include "gx_tev_bridge.h"  // GXState.tev -> NgxTevState (the GX-tee -> renderer path)
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

// SLICE 3: lighting capture (GXSetChanCtrl/Mat/AmbColor + light objects). The
// packed chan-ctrl MUST decode back through the renderer's decode_chanctl(), so the
// seam and the shader's lighting decoder agree by construction.
static void test_lighting_state() {
    auto& g = sb::platform::gx::state();

    // Channel control: enabled, REG sources, light0 only, CLAMP diffuse, SPOT attn.
    GXSetChanCtrl(GX_COLOR0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, /*mask*/0x01,
                  GX_DF_CLAMP, GX_AF_SPOT);
    ngx::ChanCtl c = ngx::decode_chanctl(g.chan[0].ctrl);
    chki(c.enable, 1, "chanctl enable decodes");
    chki(c.matVtx, 0, "chanctl matSrc=REG decodes");
    chki(c.ambVtx, 0, "chanctl ambSrc=REG decodes");
    chki(c.diffFn, 2, "chanctl diffFn=CLAMP decodes");
    chki(c.attnSel, 3, "chanctl attnFn=SPOT decodes");
    chki((long)c.mask, 0x01, "chanctl lightMask decodes");

    // A high light bit (light5) must survive the split lo/hi mask packing.
    GXSetChanCtrl(GX_COLOR1, GX_TRUE, GX_SRC_VTX, GX_SRC_VTX, 0x20 /*light5*/,
                  GX_DF_SIGN, GX_AF_SPEC);
    ngx::ChanCtl c1 = ngx::decode_chanctl(g.chan[1].ctrl);
    chki((long)c1.mask, 0x20, "chanctl high-bit lightMask decodes");
    chki(c1.matVtx, 1, "chanctl matSrc=VTX decodes");
    chki(c1.diffFn, 1, "chanctl diffFn=SIGN decodes");
    chki(c1.attnSel, 1, "chanctl attnFn=SPEC decodes");

    // Material/ambient colours.
    GXColor mc = { 100, 150, 200, 255 };
    GXColor ac = { 20, 30, 40, 255 };
    GXSetChanMatColor(GX_COLOR0, mc);
    GXSetChanAmbColor(GX_COLOR0, ac);
    chki(g.chan[0].matColor.r, 100, "matColor r captured");
    chki(g.chan[0].matColor.b, 200, "matColor b captured");
    chki(g.chan[0].ambColor.g, 30, "ambColor g captured");

    // COLOR0A0 writes the colour AND alpha slot (idx 0 and 2).
    GXColor pair = { 9, 9, 9, 200 };
    GXSetChanMatColor(GX_COLOR0A0, pair);
    chki(g.chan[0].matColor.r, 9, "COLOR0A0 sets colour slot");
    chki(g.chan[2].matColor.a, 200, "COLOR0A0 sets alpha slot");

    // Light object: build, load into slot 2, verify the native float state.
    GXLightObj lt;
    GXColor lcol = { 255, 128, 0, 255 };
    GXInitLightColor(&lt, lcol);
    GXInitLightPos(&lt, 10.f, 20.f, 30.f);
    GXInitLightDir(&lt, 0.f, 0.f, 1.f);          // stored negated
    GXInitLightAttn(&lt, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
    GXColor back; GXGetLightColor(&lt, &back);
    chki(back.r, 255, "GXGetLightColor r round-trips");
    chki(back.g, 128, "GXGetLightColor g round-trips");

    GXLoadLightObjImm(&lt, GX_LIGHT2);
    chki(g.light[2].valid ? 1 : 0, 1, "light2 loaded valid");
    chkf(g.light[2].color[0], 1.0f, "light2 color.r = 1.0");
    chkf(g.light[2].color[1], 128.f/255.f, "light2 color.g");
    chkf(g.light[2].pos[1], 20.f, "light2 pos.y");
    chkf(g.light[2].dir[2], -1.f, "light2 dir.z negated (GXInitLightDir)");
    chkf(g.light[2].cosAtt[0], 1.f, "light2 cosAtt0");
}

// SLICE 4: TEV combiner setters. Driving GXSetTevOp(GX_MODULATE) + GXSetTevOrder
// through the seam must produce EXACTLY the NgxTevState that the renderer's tev_test
// already proved renders the spec pixel (101,50,25) — closing the GX-tee -> renderer loop.
static void test_tev_state() {
    auto& g = sb::platform::gx::state();
    g.tev = sb::platform::gx::GXState::TevBlock{};   // reset (swapTable defaults to identity)

    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

    NgxTevState st = sb::platform::gx::ngx_tevstate_from_gx(g);
    chki(st.num_stages, 1, "tev num_stages");
    // GX_MODULATE color: a=ZERO(15) b=TEXC(8) c=RASC(10) d=ZERO(15), op ADD, clamp, dest PREV.
    chki((long)st.stage[0].color_env, (long)((15u<<12)|(8u<<8)|(10u<<4)|15u|(1u<<19)),
         "MODULATE color_env packs to the rendering encoding");
    chki((long)st.stage[0].alpha_env, (long)((7u<<13)|(4u<<10)|(5u<<7)|(7u<<4)|(1u<<19)),
         "MODULATE alpha_env packs to the rendering encoding");
    chki(st.stage[0].texmap, 0, "tev order texmap0");
    chki(st.stage[0].texcoord, 0, "tev order texcoord0");
    chki(st.stage[0].color_chan, GX_COLOR0A0, "tev order raster channel");
    chki(st.swap_table[0], 0x1B, "swap table 0 defaults to identity (no rrrr)");

    // GXSetTevSwapModeTable round-trips into the NgxTevState swizzle byte.
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    NgxTevState st2 = sb::platform::gx::ngx_tevstate_from_gx(g);
    chki(st2.swap_table[1], 0x57, "swap table 1 = ggga (0x57)");

    // Konst + TEV register colours capture.
    GXColor k = { 10, 20, 30, 40 };
    GXSetTevKColor(GX_KCOLOR0, k);
    GXColor tc = { 1, 2, 3, 4 };
    GXSetTevColor(GX_TEVREG1, tc);   // GX_TEVREG1 == register index 2 (CPREV=0,REG0=1,REG1=2)
    NgxTevState st3 = sb::platform::gx::ngx_tevstate_from_gx(g);
    chki(st3.kcolor[0][0], 10, "kcolor0 r");
    chki(st3.kcolor[0][3], 40, "kcolor0 a");
    chki(st3.tev_color[2][1], 2, "tevreg1 (=C1, index 2) g");
}

int main() {
    std::printf("== GX seam (transform + pipeline slices) unit tests ==\n");
    test_state_roundtrip();
    test_project_ortho();
    test_pipeline_state();
    test_lighting_state();
    test_tev_state();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
