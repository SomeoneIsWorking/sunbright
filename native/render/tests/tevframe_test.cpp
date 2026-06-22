// tevframe_test.cpp — the multi-material TEV frame (Nvk::renderTevFrame): clear ONCE, draw
// MANY material batches (each its own generated shader + texture + push + state) into one
// target, read back ONCE. This is the scene-render path sms_boot_present.cpp drives.
//
// Two batches into one frame:
//   • LEFT half  — a PASSCLR material (out = rasterColor), red vertex colour, no texture.
//   • RIGHT half — a GX_MODULATE material (out = tex * ras), white raster, a (200,100,50) tex.
// Spec-computed pixels: left = (255,0,0); right = tex (ras=255 → modulate is identity). This
// proves the per-batch shader, per-batch texture binding, vstart offsetting and single-clear
// accumulation all work — no eyeballing.
#include "nvk.h"
#include "tev_shader.h"
#include "ngx_render_data.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static NgxTevState make_passclr() {
    NgxTevState st{};
    st.num_stages = 1;
    st.stage[0].color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);   // out = RASC
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(5u<<4) | (1u<<19);  // out = RASA
    st.stage[0].texmap = 0xff; st.stage[0].texcoord = 0xff; st.stage[0].color_chan = 4;
    for (auto& s : st.swap_table) s = 0x1B;
    return st;
}
static NgxTevState make_modulate() {
    NgxTevState st{};
    st.num_stages = 1;
    st.stage[0].color_env = (15u<<12)|(8u<<8)|(10u<<4)|15u | (1u<<19);    // out = TEXC*RASC
    st.stage[0].alpha_env = (7u<<13)|(4u<<10)|(5u<<7)|(7u<<4) | (1u<<19);
    st.stage[0].texmap = 0; st.stage[0].texcoord = 0; st.stage[0].color_chan = 4;
    for (auto& s : st.swap_table) s = 0x1B;
    return st;
}

int main() {
    std::printf("== multi-material TEV frame (renderTevFrame) ==\n");
    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) {
        std::printf("  FAIL: no Vulkan device\n"); return 1;
    }
    std::printf("  device: %s\n", nvk.deviceName());

    std::string passFrag = sb_tev_gen_fragment(make_passclr());
    std::string modFrag   = sb_tev_gen_fragment(make_modulate());

    // texture for batch B (a solid 2x2 with distinct channels).
    uint8_t tex[2*2*4];
    for (int i = 0; i < 4; ++i) { tex[i*4]=200; tex[i*4+1]=100; tex[i*4+2]=50; tex[i*4+3]=255; }

    auto quad = [&](float x0, float x1, float r, float g, float b) {
        auto V = [&](float x, float y) {
            NvkTevVertex v{}; v.x = x; v.y = y; v.z = 0.0f;
            v.rgba[0]=r; v.rgba[1]=g; v.rgba[2]=b; v.rgba[3]=1.0f;
            v.rgba1[0]=r; v.rgba1[1]=g; v.rgba1[2]=b; v.rgba1[3]=1.0f;
            for (int t=0;t<8;++t){ v.uv[t][0]=0.5f; v.uv[t][1]=0.5f; }
            return v;
        };
        return std::vector<NvkTevVertex>{ V(x0,-0.9f), V(x1,-0.9f), V(x1,0.9f),
                                          V(x0,-0.9f), V(x1,0.9f),  V(x0,0.9f) };
    };
    std::vector<NvkTevVertex> verts;
    auto L = quad(-0.95f, -0.05f, 1.0f, 0.0f, 0.0f);   // left: red
    auto R = quad( 0.05f,  0.95f, 1.0f, 1.0f, 1.0f);   // right: white raster (modulate w/ tex)
    verts.insert(verts.end(), L.begin(), L.end());
    verts.insert(verts.end(), R.begin(), R.end());

    std::vector<Nvk::NvkTevBatch> batches(2);
    batches[0].vstart = 0; batches[0].vcount = 6;
    batches[0].fragGlsl = passFrag.c_str(); batches[0].shaderKey = 1;
    for (int c=0;c<4;++c) for(int k=0;k<4;++k) batches[0].push.kcolor[c][k]=255;

    batches[1].vstart = 6; batches[1].vcount = 6;
    batches[1].fragGlsl = modFrag.c_str(); batches[1].shaderKey = 2;
    batches[1].tex[0] = { tex, 2, 2, 0 };
    for (int c=0;c<4;++c) for(int k=0;k<4;++k) batches[1].push.kcolor[c][k]=255;

    chk(nvk.renderTevFrame(verts, batches, {0,0,0,1}), "rendered 2-batch TEV frame");

    const uint8_t* lp = nvk.at(32, 64);
    std::printf("  left  pixel = (%d,%d,%d) expected ~(255,0,0)\n", lp[0], lp[1], lp[2]);
    chk(lp[0] >= 250 && lp[1] <= 5 && lp[2] <= 5, "left half = passclr red");

    const uint8_t* rp = nvk.at(96, 64);
    std::printf("  right pixel = (%d,%d,%d) expected ~(200,100,50)\n", rp[0], rp[1], rp[2]);
    chk(std::abs((int)rp[0]-200)<=3 && std::abs((int)rp[1]-100)<=3 && std::abs((int)rp[2]-50)<=3,
        "right half = modulate texture");

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
