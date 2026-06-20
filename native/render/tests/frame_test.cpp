// frame_test.cpp — proves the native renderer produces a real frame with NO Dolphin.
//
// Creates a standalone headless Vulkan device, renders (1) a direct-NDC triangle and
// (2) a triangle whose screen positions come from the engine's own GXProject (tying
// the native GX transform to actual pixels), reads the pixels back, and verifies them.
// Writes PPMs so the frames can be eyeballed too. lavapipe makes this work with no GPU.

#include "nvk.h"
#include <dolphin/gx.h>
#include "gx_state.h"

#include <cstdio>
#include <cstdint>
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

static Nvk g_nvk;

static void test_direct_triangle() {
    // Triangle covering screen center, red; clear blue.
    std::vector<NvkVertex> tri = {
        { -0.5f, -0.5f, 0, 1, 0, 0, 1 },
        {  0.5f, -0.5f, 0, 1, 0, 0, 1 },
        {  0.0f,  0.5f, 0, 1, 0, 0, 1 },
    };
    chk(g_nvk.renderTriangles(tri, { 0, 0, 1, 1 }), "render direct triangle");
    std::printf("  device: %s  %ux%u\n", g_nvk.deviceName(), g_nvk.width(), g_nvk.height());

    const uint8_t* center = g_nvk.at(g_nvk.width() / 2, g_nvk.height() / 2);
    chk(center[0] > 200 && center[2] < 60, "center pixel is the red triangle");
    const uint8_t* corner = g_nvk.at(1, 1);
    chk(corner[2] > 200 && corner[0] < 60, "top-left corner is the blue clear");
    write_ppm("nvk_direct.ppm", g_nvk);
}

static void test_gxproject_triangle() {
    // Drive the triangle through the ENGINE's transform: set an ortho projection +
    // a viewport matching the render target, GXProject 3 world points -> screen px,
    // convert px -> Vulkan NDC, render. The green vertex lands where GXProject says.
    const uint32_t W = g_nvk.width(), H = g_nvk.height();
    f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, (f32)W, (f32)H, 0.0f, 1.0f);
    f32 pm[7], vp[6];
    GXGetProjectionv(pm); GXGetViewportv(vp);
    f32 mv[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};  // identity model-view

    // three world points (ortho: x,y map straight through; z=-1)
    struct P { f32 x, y; } world[3] = { {-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f} };
    std::vector<NvkVertex> tri;
    f32 cx = 0, cy = 0;
    for (auto& w : world) {
        f32 sx, sy, sz;
        GXProject(w.x, w.y, -1.0f, mv, pm, vp, &sx, &sy, &sz);
        // screen pixels -> Vulkan NDC ([-1,1], y down both ways)
        f32 nx = (sx / (f32)W) * 2.0f - 1.0f;
        f32 ny = (sy / (f32)H) * 2.0f - 1.0f;
        tri.push_back({ nx, ny, 0, 0, 1, 0, 1 });  // green
        cx += sx / 3.0f; cy += sy / 3.0f;
    }
    chk(g_nvk.renderTriangles(tri, { 0, 0, 0, 1 }), "render GXProject triangle");
    // The triangle's centroid in screen px should be green in the frame.
    const uint8_t* c = g_nvk.at((uint32_t)cx, (uint32_t)cy);
    chk(c[1] > 200 && c[0] < 60 && c[2] < 60, "GXProject centroid pixel is green");
    write_ppm("nvk_gxproject.ppm", g_nvk);
}

int main() {
    std::printf("== native frame (standalone Vulkan, no Dolphin) ==\n");
    if (!g_nvk.init(128, 128)) {
        std::printf("  init failed on default device; retrying software (lavapipe)\n");
        if (!g_nvk.init(128, 128, /*preferCpu=*/true)) {
            std::printf("  FAIL: no Vulkan device available\n");
            return 1;
        }
    }
    test_direct_triangle();
    test_gxproject_triangle();
    g_nvk.shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
