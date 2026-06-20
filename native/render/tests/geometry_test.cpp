// geometry_test.cpp — the GC-geometry -> native-frame vertical slice.
//
// Builds a GameCube GX display-list primitive (direct float XYZ positions + RGBA8
// colors, big-endian on-disc format), decodes it with the REUSED ngx mesh decoder
// (ngx_assemble_primitive — the shipping GC->native-mesh path), feeds the resulting
// native vertices to the standalone Vulkan renderer, reads pixels back, and verifies
// the triangle rendered with its decoded vertex colors. Proves the real asset->pixels
// path end to end, headless, no Dolphin, no ROM.

#include "nvk.h"
#include "ngx_mesh.h"          // ngx_assemble_primitive, NgxCP, NgxVertex
#include "ngx_decode.h"

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

// GameCube on-disc floats are big-endian.
static void put_bef32(unsigned char* p, float f) {
    uint32_t u; std::memcpy(&u, &f, 4);
    p[0] = u >> 24; p[1] = u >> 16; p[2] = u >> 8; p[3] = u;
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

int main() {
    std::printf("== GC geometry -> native frame (decode + render) ==\n");

    // CP descriptor: Position DIRECT float XYZ + Color0 DIRECT RGBA8888 (vstride 16).
    // (Same encoding the ngx mesh self-test uses.)
    NgxCP cp{};
    cp.vcd_lo = (1u << 9) | (1u << 13);
    cp.vat[0][0] = (1u << 0) | (4u << 1) | (5u << 14);

    // Three vertices of a GX_TRIANGLES primitive, NDC positions covering the center,
    // distinct per-vertex colors (red / green / blue) for a gradient triangle.
    struct V { float x, y, z; unsigned char r, g, b, a; } verts[3] = {
        { -0.5f, -0.5f, 0.f, 255,   0,   0, 255 },
        {  0.5f, -0.5f, 0.f,   0, 255,   0, 255 },
        {  0.0f,  0.5f, 0.f,   0,   0, 255, 255 },
    };
    unsigned char prim[3 * 16];
    for (int i = 0; i < 3; ++i) {
        unsigned char* v = prim + i * 16;
        put_bef32(v + 0, verts[i].x); put_bef32(v + 4, verts[i].y); put_bef32(v + 8, verts[i].z);
        v[12] = verts[i].r; v[13] = verts[i].g; v[14] = verts[i].b; v[15] = verts[i].a;
    }

    // Decode the GC primitive into native vertices + triangle indices.
    NgxArrays arr{};
    std::vector<NgxVertex> nv;
    std::vector<unsigned> idx;
    int tris = ngx_assemble_primitive(cp, 0x90 /*GX_TRIANGLES*/, prim, 3, arr, nv, idx);
    chk(tris == 1, "decoded 1 triangle");
    chk(nv.size() == 3, "decoded 3 vertices");
    // Verify the decode round-tripped position + color faithfully.
    chk(nv.size() == 3 &&
        nv[0].pos[0] == -0.5f && nv[1].pos[0] == 0.5f && nv[2].pos[1] == 0.5f,
        "positions decoded (BE float)");
    chk(nv.size() == 3 &&
        nv[0].clr[0][0] == 255 && nv[1].clr[0][1] == 255 && nv[2].clr[0][2] == 255,
        "colors decoded (RGBA8)");

    // Build the native renderer and convert decoded vertices -> NvkVertex (pos.xy as
    // NDC, color /255), expanded by the triangle index list.
    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) {
        std::printf("  FAIL: no Vulkan device\n");
        return 1;
    }
    std::printf("  device: %s\n", nvk.deviceName());
    std::vector<NvkVertex> tri;
    for (unsigned i : idx) {
        const NgxVertex& s = nv[i];
        tri.push_back({ s.pos[0], s.pos[1],
                        s.clr[0][0] / 255.f, s.clr[0][1] / 255.f,
                        s.clr[0][2] / 255.f, s.clr[0][3] / 255.f });
    }
    chk(nvk.renderTriangles(tri, { 0, 0, 0, 1 }), "render decoded triangle");  // black clear

    // Center pixel is inside the triangle -> a non-black gradient color; a corner stays
    // black (clear). This confirms the decoded GC geometry actually rasterized.
    const uint8_t* c = nvk.at(64, 64);
    chk(c[0] + c[1] + c[2] > 80, "center pixel lit by decoded triangle");
    const uint8_t* corner = nvk.at(2, 2);
    chk(corner[0] + corner[1] + corner[2] < 30, "corner is clear");
    write_ppm("nvk_geometry.ppm", nvk);

    nvk.shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
