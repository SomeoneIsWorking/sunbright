// raster_basic_test — rung 1 of the parity-focused renderer TDD ladder.
//
// PRINCIPLE (user directive): a parity test must NEVER pass if the rendering is wrong. So these
// checks are SENSITIVE — they assert EVERY pixel of whole regions against spec-computed truth (not
// a few sample points), with a tight tolerance, so any per-pixel colour error, geometry/position
// error, or vertical-orientation (Y-flip) error fails the test. Smallest scope first; sibling
// *_test.cpp files extend up the ladder (textured quad, TEV combiner, blend, depth, scenes).
//
// Renders KNOWN inputs through sb::gxsdl (the SDL3 GPU renderer — the GX seam's only renderer) and
// reads pixels back. The fragment shader is a spec passthrough (o = raster colour0) so this rung
// isolates the clear + vertex/raster + readback path; the TEV combiner is exercised by later rungs.
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

const uint8_t* px(const std::vector<uint8_t>& b, int x, int y) { return &b[((size_t)y * W + x) * 4]; }

// Assert EVERY pixel in [x0,x1)x[y0,y1) equals (r,g,b) within tol. Reports the first/worst miss and
// the count — one wrong pixel fails the test.
void region(const char* what, const std::vector<uint8_t>& buf, int x0, int y0, int x1, int y1,
            int r, int g, int b, int tol) {
    int bad = 0, wx = -1, wy = -1, wd = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const uint8_t* p = px(buf, x, y);
            int d = std::abs(p[0]-r) + std::abs(p[1]-g) + std::abs(p[2]-b);
            if (std::abs(p[0]-r) > tol || std::abs(p[1]-g) > tol || std::abs(p[2]-b) > tol) {
                if (d > wd) { wd = d; wx = x; wy = y; } ++bad;
            }
        }
    int total = (x1-x0)*(y1-y0);
    if (bad) {
        const uint8_t* p = px(buf, wx, wy);
        std::fprintf(stderr, "FAIL %-28s %d/%d px wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what, bad, total, wx, wy, p[0], p[1], p[2], r, g, b);
        ++g_fails;
    } else {
        std::fprintf(stderr, "ok   %-28s all %d px = (%d,%d,%d)+-%d\n", what, total, r, g, b, tol);
    }
}

NvkTevVertex V(float x, float y, float z, float r, float g, float b) {
    NvkTevVertex v{}; v.x=x; v.y=y; v.z=z; v.w=1.0f;
    v.rgba[0]=r; v.rgba[1]=g; v.rgba[2]=b; v.rgba[3]=1.0f; return v;
}
// Two triangles covering NDC rect [x0,x1]x[y0,y1] (clip y-down), solid colour.
std::vector<NvkTevVertex> quad(float x0, float y0, float x1, float y1, float r, float g, float b) {
    return { V(x0,y0,.5f,r,g,b), V(x1,y0,.5f,r,g,b), V(x1,y1,.5f,r,g,b),
             V(x0,y0,.5f,r,g,b), V(x1,y1,.5f,r,g,b), V(x0,y1,.5f,r,g,b) };
}
const char* kPassFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = vColor; }\n";

NvkTevBatch batch(uint32_t vcount, uint64_t key) {
    NvkTevBatch b{}; b.vstart=0; b.vcount=vcount; b.fragGlsl=kPassFrag; b.shaderKey=key;
    b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0; return b;
}
void draw(const std::vector<NvkTevVertex>& v, uint64_t key, std::vector<uint8_t>& out,
          float cr, float cg, float cb) {
    NvkTevBatch b = batch((uint32_t)v.size(), key);
    sb::gxsdl::frame_begin(cr, cg, cb, 1.0f);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), &b, 1);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(out.data(), W, H);
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) {
        std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1;
    }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // Scope 1 — CLEAR: every pixel == the GX copy-clear (0.2,0.4,0.6) → (51,102,153). Sensitive to
    // clear colour + format/channel order across the WHOLE frame.
    sb::gxsdl::frame_begin(0.2f, 0.4f, 0.6f, 1.0f);
    sb::gxsdl::draw_tev(nullptr, 0, nullptr, 0);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);
    region("clear=all (51,102,153)", buf, 0, 0, W, H, 51, 102, 153, 1);

    // Scope 2 — FULL-SCREEN quad green over black: EVERY pixel exactly green. Catches any per-pixel
    // colour/coverage error anywhere in the frame.
    draw(quad(-1,-1, 1,1, 0,1,0), 0x2001, buf, 0,0,0);
    region("fullscreen=all green", buf, 0, 0, W, H, 0, 255, 0, 1);

    // Scope 3 — LEFT-HALF quad (NDC x in [-1,0]) → final columns 0..31 green, 32..63 clear. Sharp
    // vertical boundary at x=32. Sensitive to the X clip→pixel mapping (a shifted/scaled triangle
    // moves the boundary → fail). Skip a +-2px guard band around the exact edge.
    draw(quad(-1,-1, 0,1, 0,1,0), 0x3001, buf, 0,0,0);
    region("lefthalf: cols 0..29 green",  buf, 0,  0, 30, H, 0, 255, 0, 1);
    region("lefthalf: cols 34..63 clear", buf, 34, 0, W,  H, 0, 0,   0, 1);

    // Scope 4 — ORIENTATION (falsifiable Y check): a quad on NDC y in [-1,0]. The capture contract
    // is Y-DOWN clip space (gx_geom.h), so y=-1 is the TOP of the screen → this quad fills the TOP
    // half of the final image (rows 0..31), and the readback delivers it top-left origin. If the
    // renderer's vertical orientation were wrong (a missing/extra flip) the green would land in the
    // BOTTOM half and BOTH asserts fail. (Independently confirmed correct: the full scene renders
    // Mario UPRIGHT at 0.07 parity. This rung caught a backwards expectation during authoring —
    // exactly the sensitivity intended: it must never pass on wrong output.)
    draw(quad(-1,-1, 1,0, 0,1,0), 0x4001, buf, 0,0,0);
    region("orient: rows 0..29 green",  buf, 0, 0,  W, 30,   0, 255, 0, 1);
    region("orient: rows 34..63 clear", buf, 0, 34, W, H,    0, 0,   0, 1);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
