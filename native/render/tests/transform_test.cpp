// transform_test.cpp — the FULL native geometry pipeline on MODEL-space data:
// decode GC geometry -> place with an MTX-seam model matrix -> project via GXState ->
// render -> verify pixels. Exercises MTX + GX + ngx-decode + nvk together, no Dolphin.

#include "nvk.h"
#include "nvk_transform.h"
#include "ngx_mesh.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include "gx_state.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static bool feq(float a, float b, float e = 1e-3f) { float d = a - b; return (d < 0 ? -d : d) <= e; }

static void put_bef32(unsigned char* p, float f) {
    uint32_t u; std::memcpy(&u, &f, 4);
    p[0] = u >> 24; p[1] = u >> 16; p[2] = u >> 8; p[3] = u;
}
static void write_ppm(const char* path, const Nvk& n) {
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", n.width(), n.height());
    for (uint32_t y = 0; y < n.height(); ++y) for (uint32_t x = 0; x < n.width(); ++x) {
        const uint8_t* p = n.at(x, y); std::fputc(p[0], f); std::fputc(p[1], f); std::fputc(p[2], f);
    }
    std::fclose(f);
}

static void test_project_unit() {
    // Ortho projection + 128x128 viewport. A model point translated to the origin by
    // the model-view should land at screen center -> NDC (0,0).
    f32 proj[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, 128, 128, 0.0f, 1.0f);
    f32 pm[7], vp[6]; GXGetProjectionv(pm); GXGetViewportv(vp);

    // model-view = translate (-5,-5,0): model point (5,5,-1) -> eye (0,0,-1) -> center.
    Mtx mv; PSMTXTrans(mv, -5, -5, 0);
    float modelPt[3] = { 5, 5, -1 };
    float ndc[2];
    nvk_project_ndc(modelPt, mv, pm, vp, ndc);
    chk(feq(ndc[0], 0.0f) && feq(ndc[1], 0.0f), "MTX model-view + GXProject -> center NDC");
}

int main() {
    std::printf("== model-space geometry -> transform -> native frame ==\n");
    test_project_unit();

    // Decode a GC triangle whose positions are MODEL-space (around model origin),
    // then place + project it with the engine's matrices.
    NgxCP cp{};
    cp.vcd_lo = (1u << 9) | (1u << 13);
    cp.vat[0][0] = (1u << 0) | (4u << 1) | (5u << 14);
    struct V { float x, y, z; unsigned char r, g, b, a; } verts[3] = {
        { -20.f, -20.f, -50.f, 255, 255, 0, 255 },   // model-space, yellow
        {  20.f, -20.f, -50.f, 255, 255, 0, 255 },
        {   0.f,  20.f, -50.f, 255, 255, 0, 255 },
    };
    unsigned char prim[3 * 16];
    for (int i = 0; i < 3; ++i) {
        unsigned char* v = prim + i * 16;
        put_bef32(v, verts[i].x); put_bef32(v + 4, verts[i].y); put_bef32(v + 8, verts[i].z);
        v[12] = verts[i].r; v[13] = verts[i].g; v[14] = verts[i].b; v[15] = verts[i].a;
    }
    NgxArrays arr{};
    std::vector<NgxVertex> nv; std::vector<unsigned> idx;
    ngx_assemble_primitive(cp, 0x90, prim, 3, arr, nv, idx);
    chk(nv.size() == 3 && idx.size() == 3, "decoded model-space triangle");

    // Perspective projection so the z=-50 triangle is in front of the camera, and a
    // model-view = identity (camera at origin looking down -Z).
    f32 proj[4][4]; C_MTXPerspective(proj, 60.0f, 1.0f, 1.0f, 1000.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);
    GXSetViewport(0, 0, 128, 128, 0.0f, 1.0f);
    f32 pm[7], vp[6]; GXGetProjectionv(pm); GXGetViewportv(vp);
    Mtx mv; PSMTXIdentity(mv);

    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) { std::printf("  FAIL: no Vulkan\n"); return 1; }
    std::printf("  device: %s\n", nvk.deviceName());
    std::vector<NvkVertex> tri = nvk_project_mesh(nv, idx, mv, pm, vp);
    chk(nvk.renderTriangles(tri, { 0, 0, 0.3f, 1 }), "render projected model triangle");

    const uint8_t* c = nvk.at(64, 64);
    chk(c[0] > 150 && c[1] > 150 && c[2] < 120, "center is the yellow model triangle");
    write_ppm("nvk_transform.ppm", nvk);
    nvk.shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
