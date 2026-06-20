// lighting_test.cpp — GX per-vertex lighting -> lit raster colour -> native frame.
//
// Drives the SHIPPING lighting unit (ngx_light.h light_color0 — the same function the
// ngx override calls) to compute a vertex's lit COLOR0, feeds it as the raster colour
// into a PASSCLR TEV material (out = rasterColor), renders through nvk, and checks the
// SPEC-COMPUTED pixel. A directional white light + white material: a normal facing the
// light is full-bright, a normal at 60deg is half-bright, a normal facing away is dark.
// This proves lit-vs-unlit brightness actually reaches the framebuffer.

#include "nvk.h"
#include "tev_shader.h"
#include "ngx_render_data.h"
#include "ngx_light.h"

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

// GX_PASSCLR TEV state: out = rasterColor (no texture). cc: d=RASC(10), a=b=c=ZERO.
static NgxTevState make_passclr() {
    NgxTevState st{};
    st.num_stages = 1;
    st.stage[0].color_env = (15u << 12) | (15u << 8) | (15u << 4) | 10u | (1u << 19);
    st.stage[0].alpha_env = (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4) | (1u << 19);
    st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 4;
    for (auto& s : st.swap_table) s = 0x1B;
    return st;
}

// Compute the lit COLOR0 (0..1 rgb) for a surface normal, via the shipping unit.
static void lit_color(const float normal[3], bool enable, float out[3]) {
    ngx::ChanCtl C{};
    C.matVtx = false; C.ambVtx = false; C.enable = enable;
    C.diffFn = 2 /*CLAMP*/; C.attnSel = 0 /*NONE = directional, attn=1*/;
    C.mask = 0x01;                                   // light0 only
    const float matColor[3] = { 1, 1, 1 };           // white material
    const float ambColor[3] = { 0, 0, 0 };           // no ambient: brightness from the light
    ngx::LightSrc lights[8];
    lights[0].valid = true;
    lights[0].color[0] = lights[0].color[1] = lights[0].color[2] = 1.f;  // white light
    lights[0].pos[0] = 0; lights[0].pos[1] = 0; lights[0].pos[2] = 100;  // far +Z => l=(0,0,1)
    const float eye[3] = { 0, 0, 0 };
    const float vcol0[3] = { 1, 1, 1 };
    ngx::light_color0(C, matColor, ambColor, lights, eye, normal, vcol0, out);
}

static int render_normal(Nvk& nvk, const NvkTevPush& push, const float normal[3], bool enable) {
    float lit[3]; lit_color(normal, enable, lit);
    auto V = [&](float x, float y) {
        NvkTevVertex v{};
        v.x = x; v.y = y; v.z = 0.f;
        v.rgba[0]=lit[0]; v.rgba[1]=lit[1]; v.rgba[2]=lit[2]; v.rgba[3]=1.f;
        v.rgba1[0]=lit[0]; v.rgba1[1]=lit[1]; v.rgba1[2]=lit[2]; v.rgba1[3]=1.f;
        return v;
    };
    std::vector<NvkTevVertex> tri = { V(-0.8f,-0.8f), V(0.8f,-0.8f), V(0.f,0.8f) };
    nvk.renderTevTriangles(tri, push, { 0, 0, 0, 1 });
    return nvk.at(64, 64)[0];   // center R (grey, so R==G==B)
}

int main() {
    std::printf("== GX lighting -> lit raster -> native frame ==\n");

    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) {
        std::printf("  FAIL: no Vulkan device\n");
        return 1;
    }
    std::printf("  device: %s\n", nvk.deviceName());
    chk(nvk.setTevFragment(sb_tev_gen_fragment(make_passclr())), "compiled PASSCLR TEV");

    NvkTevPush push{};
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 4; ++c) push.kcolor[i][c] = 255;

    const float toward[3]  = { 0.f, 0.f, 1.f };                       // n.l = 1   -> full bright
    const float at60[3]    = { std::sin(1.0471975f), 0.f, std::cos(1.0471975f) }; // 60deg, n.l=0.5
    const float away[3]    = { 0.f, 0.f, -1.f };                      // n.l = -1  -> dark

    int lit   = render_normal(nvk, push, toward, true);
    int mid   = render_normal(nvk, push, at60,   true);
    int dark  = render_normal(nvk, push, away,   true);
    int unlit = render_normal(nvk, push, away,   false);  // lighting disabled => matColor

    std::printf("  toward=%d  at60=%d  away=%d  (lighting off, away)=%d\n", lit, mid, dark, unlit);

    chk(lit >= 250, "normal toward light -> full bright (~255)");
    chk(std::abs(mid - 128) <= 3, "normal at 60deg -> half bright (~128)");
    chk(dark <= 6, "normal away from light -> dark (~0)");
    chk(lit - dark >= 200, "lit vs unlit brightness reaches the framebuffer");
    chk(unlit >= 250, "lighting disabled -> material colour (white), independent of normal");

    nvk.shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
