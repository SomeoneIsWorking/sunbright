// tev_test.cpp — the GX TEV combiner -> generated GLSL -> native frame slice.
//
// Builds a known TEV material (GX_MODULATE: out = texColor * rasterColor), generates
// its fragment shader with the SHIPPING TEV generator (sb_tev_gen_fragment), compiles
// it at runtime (sb_compile_fragment_glsl, glslang), and renders a quad sampling a
// distinct-per-channel texture with a known raster colour. The output pixel is
// SPEC-COMPUTED from the GC integer combiner — no eyeballing.
//
// GX_MODULATE per stage: prev = lerp(ZERO, tex, ras) in 0..255 integer space, i.e.
//   prev_c = ((tex_c * (ras_c + (ras_c>>7))) + 128) >> 8
// For tex=(200,100,50), ras=128: ras+ras>>7 = 129, so
//   R=(200*129+128)>>8=101, G=(100*129+128)>>8=50, B=(50*129+128)>>8=25.

#include "nvk.h"
#include "tev_shader.h"            // sb_tev_gen_fragment, NgxTevState
#include "ngx_render_data.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static void write_ppm(const char* path, const Nvk& nvk) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", nvk.width(), nvk.height());
    for (uint32_t y = 0; y < nvk.height(); ++y)
        for (uint32_t x = 0; x < nvk.width(); ++x) {
            const uint8_t* p = nvk.at(x, y);
            std::fputc(p[0], f); std::fputc(p[1], f); std::fputc(p[2], f);
        }
    std::fclose(f);
}

// GX_MODULATE TEV state (mirrors tev_shader.cpp's make_modulate, but with the swap
// tables set to identity 0x1B — a synthetic state left at swap_table=0 swizzles to
// "rrrr" and clobbers per-channel results, the known swap-table trap).
static NgxTevState make_modulate() {
    NgxTevState st{};
    st.num_stages = 1;
    // cc: a=ZERO(15) b=TEXC(8) c=RASC(10) d=ZERO(15), clamp(bit19), dest=prev.
    st.stage[0].color_env = (15u << 12) | (8u << 8) | (10u << 4) | 15u | (1u << 19);
    // ac: a=ZERO(7) b=TEXA(4) c=RASA(5) d=ZERO(7), clamp(bit19), dest=prev.
    st.stage[0].alpha_env = (7u << 13) | (4u << 10) | (5u << 7) | (7u << 4) | (1u << 19);
    st.stage[0].texmap = 0;
    st.stage[0].texcoord = 0;
    st.stage[0].color_chan = 4;             // GX_COLOR0A0 -> raster from vColor (col0)
    for (auto& k : st.kcolor) { k[0] = k[1] = k[2] = k[3] = 255; }
    for (auto& s : st.swap_table) s = 0x1B; // identity swizzle (rgba)
    return st;
}

int main() {
    std::printf("== GX TEV combiner -> generated GLSL -> native frame ==\n");

    NgxTevState st = make_modulate();
    std::string glsl = sb_tev_gen_fragment(st);
    chk(glsl.find("#version 450") != std::string::npos, "generator emitted a GLSL shader");

    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) {
        std::printf("  FAIL: no Vulkan device\n");
        return 1;
    }
    std::printf("  device: %s\n", nvk.deviceName());

    chk(nvk.setTevFragment(glsl), "compiled + built the TEV pipeline");

    // texmap 0 = a solid 2x2 with distinct channels (200,100,50,255).
    uint8_t tex[2 * 2 * 4];
    for (int i = 0; i < 4; ++i) { tex[i*4+0]=200; tex[i*4+1]=100; tex[i*4+2]=50; tex[i*4+3]=255; }
    chk(nvk.setTevTexture(0, tex, 2, 2), "uploaded texmap 0");

    // A center-covering triangle: raster color0 = 128/255 on every vertex, uv = 0.5.
    const float ras = 128.0f / 255.0f;
    auto V = [&](float x, float y) {
        NvkTevVertex v{};
        v.x = x; v.y = y; v.z = 0.0f;
        v.rgba[0] = v.rgba[1] = v.rgba[2] = ras; v.rgba[3] = 1.0f;
        v.rgba1[0] = v.rgba1[1] = v.rgba1[2] = ras; v.rgba1[3] = 1.0f;
        for (int t = 0; t < 8; ++t) { v.uv[t][0] = 0.5f; v.uv[t][1] = 0.5f; }
        return v;
    };
    std::vector<NvkTevVertex> tri = { V(-0.8f, -0.8f), V(0.8f, -0.8f), V(0.0f, 0.8f) };

    // push constants: CPREV/C0..C2 = 0 (prev init), KONST = 255.
    NvkTevPush push{};
    for (int i = 0; i < 4; ++i) for (int c = 0; c < 4; ++c) push.kcolor[i][c] = 255;

    chk(nvk.renderTevTriangles(tri, push, { 0, 0, 0, 1 }), "rendered TEV triangle");

    const uint8_t* p = nvk.at(64, 64);
    std::printf("  center pixel = (%d,%d,%d,%d), expected ~(101,50,25,255)\n", p[0], p[1], p[2], p[3]);
    // Spec-computed GX_MODULATE result, allow +-2 for rounding/sampler.
    chk(std::abs((int)p[0] - 101) <= 2, "R = tex.r * ras (modulate)");
    chk(std::abs((int)p[1] -  50) <= 2, "G = tex.g * ras (modulate)");
    chk(std::abs((int)p[2] -  25) <= 2, "B = tex.b * ras (modulate)");
    chk((int)p[3] >= 253, "A opaque (modulate alpha=255)");

    // A corner outside the triangle stays at the black clear.
    const uint8_t* corner = nvk.at(2, 2);
    chk(corner[0] + corner[1] + corner[2] < 16, "corner is clear");

    write_ppm("nvk_tev.ppm", nvk);
    nvk.shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
