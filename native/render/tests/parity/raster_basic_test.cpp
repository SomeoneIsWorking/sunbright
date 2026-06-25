// raster_basic_test — the first rung of the parity-focused renderer TDD ladder.
//
// Renders KNOWN inputs through the SDL3 GPU renderer (sb::gxsdl, the GX seam's only renderer) and
// asserts the readback pixels against SPEC-computed ground truth — not against another renderer.
// Smallest scope first: (1) a render-pass CLEAR reproduces the GX copy-clear colour exactly; (2) a
// single solid triangle covers the expected region with the expected (interpolated) raster colour,
// and leaves the rest at the clear. Larger scope (textured quad, per-TEV-stage combiner math, blend
// modes, depth) are sibling *_test.cpp files added up the ladder. See docs/gx_sdlgpu_switch.md.
#include "gx_sdlgpu.h"
#include "gx_geom.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>

using sb::render::NvkTevVertex;
using sb::render::NvkTevBatch;

namespace {
constexpr int W = 64, H = 64;
int g_fails = 0;

const uint8_t* px(const std::vector<uint8_t>& buf, int x, int y) { return &buf[((size_t)y * W + x) * 4]; }

// A spec-passthrough TEV fragment shader: o = raster colour0 (vColor). Declares the same resource
// interface the generated TEV shaders use (set=0 tex[8] + push_constant) so it exercises the real
// remap path in gx_sdlgpu; the body just outputs the interpolated vertex colour.
const char* kPassFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = vColor; }\n";

void check_rgb(const char* what, const uint8_t* p, int r, int g, int b, int tol) {
    int dr = std::abs(p[0] - r), dg = std::abs(p[1] - g), db = std::abs(p[2] - b);
    if (dr > tol || dg > tol || db > tol) {
        std::fprintf(stderr, "FAIL %s: got (%d,%d,%d) expected (%d,%d,%d) tol=%d\n",
                     what, p[0], p[1], p[2], r, g, b, tol);
        ++g_fails;
    } else {
        std::fprintf(stderr, "ok   %s: (%d,%d,%d)\n", what, p[0], p[1], p[2]);
    }
}

NvkTevVertex vtx(float x, float y, float z, float r, float g, float b, float a) {
    NvkTevVertex v{};
    v.x = x; v.y = y; v.z = z; v.w = 1.0f;
    v.rgba[0] = r; v.rgba[1] = g; v.rgba[2] = b; v.rgba[3] = a;
    return v;
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) {
        std::fprintf(stderr, "FAIL: sb::gxsdl::init(%d,%d) failed (need SDL_VIDEODRIVER=offscreen)\n", W, H);
        return 1;
    }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // ── Scope 1: CLEAR reproduces the GX copy-clear colour (0.2,0.4,0.6) → (51,102,153). ──
    sb::gxsdl::frame_begin(0.2f, 0.4f, 0.6f, 1.0f);
    sb::gxsdl::draw_tev(nullptr, 0, nullptr, 0);
    sb::gxsdl::frame_end();
    if (!sb::gxsdl::readback(buf.data(), W, H)) { std::fprintf(stderr, "FAIL: readback\n"); return 1; }
    check_rgb("clear center", px(buf, W / 2, H / 2), 51, 102, 153, 2);
    check_rgb("clear corner", px(buf, 1, 1), 51, 102, 153, 2);

    // ── Scope 2: one solid RED triangle covering the centre, over a BLACK clear. The triangle
    // spans NDC (-0.8,+0.8)-(+0.8,+0.8)-(0,-0.8) (SDL3 GPU Y-down clip space) → after the readback
    // top-left flip it fills the middle; the centre pixel is red, the top corner stays clear. ──
    std::vector<NvkTevVertex> verts = {
        vtx(-0.8f,  0.8f, 0.5f, 1, 0, 0, 1),
        vtx( 0.8f,  0.8f, 0.5f, 1, 0, 0, 1),
        vtx( 0.0f, -0.8f, 0.5f, 1, 0, 0, 1),
    };
    NvkTevBatch b{};
    b.vstart = 0; b.vcount = 3; b.fragGlsl = kPassFrag; b.shaderKey = 0x9001;
    b.z_test = 1; b.z_func = 3; b.z_write = 1; b.blend_mode = 0;
    std::vector<NvkTevBatch> batches = { b };

    sb::gxsdl::frame_begin(0, 0, 0, 1);
    sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), batches.data(), (int)batches.size());
    sb::gxsdl::frame_end();
    if (!sb::gxsdl::readback(buf.data(), W, H)) { std::fprintf(stderr, "FAIL: readback\n"); return 1; }
    check_rgb("tri centre = red", px(buf, W / 2, H / 2), 255, 0, 0, 2);
    check_rgb("tri top corner = clear", px(buf, W / 2, 2), 0, 0, 0, 2);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
