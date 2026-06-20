// cube_test.cpp — a depth-tested 3D cube from INDEXED GC geometry, fully native.
//
// The real J3D vertex format: positions live in a separate array, the display list
// references them by index (POS_INDEX16). This builds a cube as 8 indexed positions +
// a 6-quad display list (per-face colors), decodes it with ngx_build_mesh (the shipping
// DL walker + indexed-array resolver), places it with an MTX-seam rotation, projects
// with a perspective matrix, and renders DEPTH-TESTED. Verifies a solid shape rendered
// (center lit, corners clear) — the cube's correctness is confirmed visually (PPM).
// Decode (indexed) -> MTX -> GX projection -> depth-buffered Vulkan, no Dolphin.

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

// 8 cube corners (model space).
static const float CORNER[8][3] = {
    {-15,-15,-15}, {15,-15,-15}, {15,15,-15}, {-15,15,-15},  // back  (z-)
    {-15,-15, 15}, {15,-15, 15}, {15,15, 15}, {-15,15, 15},  // front (z+)
};
// 6 faces (CCW), each 4 corner indices + an RGB face color.
struct Face { int idx[4]; unsigned char r, g, b; };
static const Face FACE[6] = {
    {{4,5,6,7}, 255,  64,  64},   // +Z front  red
    {{1,0,3,2},  64, 255,  64},   // -Z back   green
    {{5,1,2,6},  64,  64, 255},   // +X right  blue
    {{0,4,7,3}, 255, 255,  64},   // -X left   yellow
    {{7,6,2,3}, 255,  64, 255},   // +Y top    magenta
    {{0,1,5,4},  64, 255, 255},   // -Y bottom cyan
};

int main() {
    std::printf("== depth-tested 3D cube from indexed GC geometry ==\n");

    // Position array: 8 corners, stride 12, big-endian float XYZ.
    static unsigned char posbuf[8 * 12];
    for (int i = 0; i < 8; ++i) {
        put_bef32(posbuf + i * 12 + 0, CORNER[i][0]);
        put_bef32(posbuf + i * 12 + 4, CORNER[i][1]);
        put_bef32(posbuf + i * 12 + 8, CORNER[i][2]);
    }
    // CP: Position Index16 (array 0) + Color0 Direct RGBA8888.
    NgxCP cp{};
    cp.vcd_lo = (3u << 9) | (1u << 13);
    cp.vat[0][0] = (1u << 0) | (4u << 1) | (5u << 14);
    cp.array_base[0] = 0x1000; cp.array_stride[0] = 12;

    // Display list: 6 Quads prims (one per face). vstride = 2 (be16 index) + 4 (RGBA8).
    std::vector<unsigned char> dl;
    for (const Face& f : FACE) {
        dl.push_back(0x80); dl.push_back(0); dl.push_back(4);   // Quads, count 4
        for (int k = 0; k < 4; ++k) {
            int ix = f.idx[k];
            dl.push_back((unsigned char)(ix >> 8)); dl.push_back((unsigned char)ix);  // be16 index
            dl.push_back(f.r); dl.push_back(f.g); dl.push_back(f.b); dl.push_back(255);
        }
    }
    auto resolve = [](unsigned addr, void*) -> const unsigned char* {
        if (addr >= 0x1000 && addr < 0x1000 + sizeof(posbuf)) return posbuf + (addr - 0x1000);
        return nullptr;
    };
    std::vector<NgxVertex> nv; std::vector<unsigned> idx;
    int tris = ngx_build_mesh(cp, dl.data(), dl.size(), resolve, nullptr, nv, idx);
    chk(tris == 12, "cube decoded to 12 triangles (6 quads)");
    chk(nv.size() == 24, "24 verts (4 per face)");

    // Place + project: rotate the cube (MTX seam), translate to z=-80, perspective.
    Mtx ry, rx, rot, mv;
    PSMTXRotRad(ry, 'y', 0.6f);
    PSMTXRotRad(rx, 'x', 0.45f);
    PSMTXConcat(rx, ry, rot);
    PSMTXCopy(rot, mv);
    mv[0][3] = 0; mv[1][3] = 0; mv[2][3] = -80;   // 80 units in front of the camera

    f32 proj[4][4]; C_MTXPerspective(proj, 50.0f, 1.0f, 1.0f, 1000.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);
    GXSetViewport(0, 0, 256, 256, 0.0f, 1.0f);
    f32 pm[7], vp[6]; GXGetProjectionv(pm); GXGetViewportv(vp);

    Nvk nvk;
    if (!nvk.init(256, 256) && !nvk.init(256, 256, true)) { std::printf("  FAIL: no Vulkan\n"); return 1; }
    std::printf("  device: %s\n", nvk.deviceName());
    std::vector<NvkVertex> tri = nvk_project_mesh(nv, idx, mv, pm, vp);
    chk(nvk.renderTriangles(tri, { 0.1f, 0.1f, 0.15f, 1 }), "render depth-tested cube");

    // Center is on the cube (lit, not the dark clear); a corner is clear. Count lit
    // pixels to confirm the cube has real coverage.
    const uint8_t* c = nvk.at(128, 128);
    chk(c[0] + c[1] + c[2] > 200, "center pixel on the cube");
    const uint8_t* corner = nvk.at(3, 3);
    chk(corner[0] < 60 && corner[1] < 60, "corner is the clear color");
    int lit = 0;
    for (uint32_t y = 0; y < nvk.height(); ++y)
        for (uint32_t x = 0; x < nvk.width(); ++x) {
            const uint8_t* p = nvk.at(x, y);
            if (p[0] + p[1] + p[2] > 250) ++lit;   // brighter than the dark clear
        }
    chk(lit > 256 * 256 / 8, "cube covers a substantial area");
    std::printf("  lit pixels: %d / %d\n", lit, 256 * 256);
    write_ppm("nvk_cube.ppm", nvk);
    nvk.shutdown();

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
