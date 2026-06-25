// texture_basic_test — rung 2 of the parity-focused renderer TDD ladder: texture sampling.
//
// Same sensitivity principle (must NEVER pass on wrong output): a KNOWN 2x2 texture sampled with
// NEAREST filtering across a full-screen quad must place each texel in its exact final quadrant,
// asserted over the whole quadrant interior (not a sample point). Catches: texture upload (channel
// order / row order), UV→texel mapping, nearest filtering, and the geometry/orientation that rung 1
// pinned. Texture path is the SDL3 GPU renderer's (sb::gxsdl) real upload + sampler + draw.
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

void region(const char* what, const std::vector<uint8_t>& buf, int x0, int y0, int x1, int y1,
            int r, int g, int b, int tol) {
    int bad = 0, wx = -1, wy = -1, wd = -1;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const uint8_t* p = px(buf, x, y);
        int d = std::abs(p[0]-r)+std::abs(p[1]-g)+std::abs(p[2]-b);
        if (std::abs(p[0]-r)>tol || std::abs(p[1]-g)>tol || std::abs(p[2]-b)>tol) { if (d>wd){wd=d;wx=x;wy=y;} ++bad; }
    }
    int total=(x1-x0)*(y1-y0);
    if (bad) { const uint8_t* p=px(buf,wx,wy);
        std::fprintf(stderr,"FAIL %-24s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-24s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

NvkTevVertex V(float x, float y, float u, float v) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=.5f; t.w=1.f;
    t.rgba[0]=t.rgba[1]=t.rgba[2]=t.rgba[3]=1.f;   // white raster (modulate-neutral)
    t.uv[0][0]=u; t.uv[0][1]=v; return t;
}
const char* kTexFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = texture(tex[0], vUV[0]); }\n";
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // 2x2 RGBA8, row-major: texel(col,row) (0,0)=red (1,0)=green (0,1)=blue (1,1)=white.
    const uint8_t tex[2*2*4] = {
        255,0,0,255,   0,255,0,255,     // row 0: red,   green
        0,0,255,255,   255,255,255,255  // row 1: blue,  white
    };
    // Full-screen quad; UV(0,0) at NDC top-left (-1,-1), UV(1,1) at NDC bottom-right (1,1).
    std::vector<NvkTevVertex> v = {
        V(-1,-1, 0,0), V(1,-1, 1,0), V(1,1, 1,1),
        V(-1,-1, 0,0), V(1,1, 1,1),  V(-1,1, 0,1),
    };
    NvkTevBatch b{}; b.vstart=0; b.vcount=6; b.fragGlsl=kTexFrag; b.shaderKey=0x7001;
    b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
    b.tex[0].rgba=tex; b.tex[0].w=2; b.tex[0].h=2;
    b.tex[0].linear=0; b.tex[0].min_filter=0; b.tex[0].wrap_s=0; b.tex[0].wrap_t=0;  // NEAREST, clamp

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), &b, 1);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    // UV(0,0)=texel(0,0) sits at NDC(-1,-1) → final TOP-LEFT (rung-1 orientation: ndc y=-1 = top).
    // Each quadrant interior must be exactly one texel; skip a 4px guard band around the u/v=0.5 seam.
    const int lo0=0, hi0=28, lo1=36, hi1=64;
    region("TL = red",   buf, lo0,lo0, hi0,hi0, 255,0,0,   1);
    region("TR = green", buf, lo1,lo0, hi1,hi0, 0,255,0,   1);
    region("BL = blue",  buf, lo0,lo1, hi0,hi1, 0,0,255,   1);
    region("BR = white", buf, lo1,lo1, hi1,hi1, 255,255,255,1);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
