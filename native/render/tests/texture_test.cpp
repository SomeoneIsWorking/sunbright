// texture_test.cpp — GC texture decode + textured native render, no Dolphin.
//
// Decodes a real GameCube texture (RGB5A3, 4x4-tiled) with the REUSED sb_tex_decode,
// uploads it to the standalone Vulkan renderer, draws a full-screen textured quad, and
// verifies the sampled texels land in the right screen regions. Builds a 4x4 texture
// with distinct colored quadrants so UV->screen mapping is checkable. No Dolphin, no ROM.

#include "nvk.h"
#include "tex_decode.h"
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static void write_ppm(const char* path, const Nvk& n) {
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", n.width(), n.height());
    for (uint32_t y = 0; y < n.height(); ++y) for (uint32_t x = 0; x < n.width(); ++x) {
        const uint8_t* p = n.at(x, y); std::fputc(p[0], f); std::fputc(p[1], f); std::fputc(p[2], f);
    }
    std::fclose(f);
}

// Pack an RGB5A3 texel (opaque: bit15=1, 5-5-5 RGB).
static uint16_t rgb5a3_opaque(int r, int g, int b) {
    return (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}

int main() {
    std::printf("== GC texture decode + textured native render ==\n");

    // Build a 4x4 RGB5A3 texture (one tile) with 4 colored quadrants:
    //   top-left RED, top-right GREEN, bottom-left BLUE, bottom-right WHITE.
    // GC RGB5A3 is big-endian 16-bit, tiled in 4x4 blocks (exactly one tile here).
    const int W = 4, H = 4;
    uint16_t texel[16];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int r, g, b;
            bool right = x >= 2, bottom = y >= 2;
            if (!right && !bottom)      { r = 255; g = 0;   b = 0;   }  // TL red
            else if (right && !bottom)  { r = 0;   g = 255; b = 0;   }  // TR green
            else if (!right && bottom)  { r = 0;   g = 0;   b = 255; }  // BL blue
            else                        { r = 255; g = 255; b = 255; }  // BR white
            texel[y * W + x] = rgb5a3_opaque(r, g, b);
        }
    // Serialize big-endian (on-disc GC byte order) — one 4x4 tile, row-major within.
    uint8_t src[16 * 2];
    for (int i = 0; i < 16; ++i) { src[i*2] = texel[i] >> 8; src[i*2+1] = texel[i] & 0xFF; }

    // Decode with the REUSED GC decoder.
    uint32_t rgba[16];
    sb_tex_decode(rgba, src, W, H, SB_TF_RGB5A3, nullptr, 0);
    // sb_tex_decode packs RGBA8 per texel; verify the TL texel is red.
    uint8_t* t0 = (uint8_t*)&rgba[0];
    chk(t0[0] > 200 && t0[1] < 60 && t0[2] < 60, "decoded TL texel = red");

    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) { std::printf("  FAIL: no Vulkan\n"); return 1; }
    std::printf("  device: %s\n", nvk.deviceName());
    chk(nvk.setTexture((const uint8_t*)rgba, W, H), "upload texture");

    // Full-screen quad (two tris), UV (0,0) top-left .. (1,1) bottom-right.
    std::vector<NvkTexVertex> quad = {
        { -1, -1, 0, 0, 0 }, { 1, -1, 0, 1, 0 }, { 1, 1, 0, 1, 1 },
        { -1, -1, 0, 0, 0 }, { 1, 1, 0, 1, 1 }, { -1, 1, 0, 0, 1 },
    };
    chk(nvk.renderTexturedTriangles(quad, { 0, 0, 0, 1 }), "render textured quad");

    // Sample the four screen quadrants -> the four texture quadrants.
    const uint8_t* tl = nvk.at(32, 32);    // top-left -> red
    const uint8_t* tr = nvk.at(96, 32);    // top-right -> green
    const uint8_t* bl = nvk.at(32, 96);    // bottom-left -> blue
    const uint8_t* br = nvk.at(96, 96);    // bottom-right -> white
    chk(tl[0] > 200 && tl[1] < 60 && tl[2] < 60, "screen TL sampled red");
    chk(tr[1] > 200 && tr[0] < 60 && tr[2] < 60, "screen TR sampled green");
    chk(bl[2] > 200 && bl[0] < 60 && bl[1] < 60, "screen BL sampled blue");
    chk(br[0] > 200 && br[1] > 200 && br[2] > 200, "screen BR sampled white");
    write_ppm("nvk_texture.ppm", nvk);
    nvk.shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
