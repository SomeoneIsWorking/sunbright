// blend_depth_test — rung 4 of the parity-focused renderer TDD ladder: blend modes + depth test.
//
// Rungs 1-3 proved raster, texture sampling, and the TEV combiner math. This rung proves the
// PER-BATCH PIPELINE STATE (GX blend + depth) that gx_sdlgpu derives from each NvkTevBatch is
// faithful. A trivial passthrough shader (o = vColor) isolates the pipeline state — the colour is
// the vertex colour, so what's under test is purely blend factors / depth test+func+write, not the
// combiner. Four quadrants, each two overlapping draws in draw order:
//   TL additive blend   (src=ONE,  dst=ONE)              → exact integer add (tol 0).
//   TR alpha-over blend  (src=SRC_ALPHA, dst=1-SRC_ALPHA) → src*a + dst*(1-a) (GPU float, tol 1).
//   BL depth rejects far  (near drawn FIRST, far LEQUAL)  → near survives — catches "depth ignored".
//   BR depth accepts near (far drawn FIRST, near LEQUAL)  → near wins — order-independent sort.
#include "gx_sdlgpu.h"
#include "gx_geom.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        std::fprintf(stderr,"FAIL %-22s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-22s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

// A vertex with solid colour+alpha (0..255) at the given NDC xy and clip depth z (w=1 → NDC z = z).
NvkTevVertex V(float x, float y, float z, int rr, int gg, int bb, int aa) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=z; t.w=1.f;
    t.rgba[0]=rr/255.f; t.rgba[1]=gg/255.f; t.rgba[2]=bb/255.f; t.rgba[3]=aa/255.f;
    return t;
}

// Passthrough fragment shader (o = vColor). Declares tex[8]+push_constant so remap_for_sdlgpu's
// fixed binding rewrite + the shader's hardcoded 8-sampler/1-uniform layout both match (resources
// unused). o.a = vColor.a feeds the blend.
const char* kPassFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = vColor; }\n";

// Append a quad (2 tris) covering NDC [x0,x1]x[y0,y1] at depth z with solid colour+alpha.
void quad(std::vector<NvkTevVertex>& v, float x0, float y0, float x1, float y1, float z,
          int r, int g, int b, int a) {
    v.push_back(V(x0,y0,z,r,g,b,a)); v.push_back(V(x1,y0,z,r,g,b,a)); v.push_back(V(x1,y1,z,r,g,b,a));
    v.push_back(V(x0,y0,z,r,g,b,a)); v.push_back(V(x1,y1,z,r,g,b,a)); v.push_back(V(x0,y1,z,r,g,b,a));
}

NvkTevBatch base(uint32_t vstart, uint64_t key) {
    NvkTevBatch b{}; b.vstart=vstart; b.vcount=6; b.fragGlsl=kPassFrag; b.shaderKey=key;
    b.z_test=0; b.z_func=3; b.z_write=0; b.blend_mode=0; b.src_factor=1; b.dst_factor=0;
    return b;
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // Quadrant NDC rects (y-down clip: NDC y=-1 = top).
    const float L=-1, M=0, R=1, T=-1, MidY=0, B=1;
    std::vector<NvkTevVertex> v;
    std::vector<NvkTevBatch> batches;

    // ── TL: additive blend. bg(40,60,80) opaque, then fg(30,50,70) src=ONE dst=ONE → (70,110,150).
    { uint32_t s=(uint32_t)v.size(); quad(v, L,T, M,MidY, 0.5f, 40,60,80,255);
      NvkTevBatch b=base(s,0x4000); batches.push_back(b); }
    { uint32_t s=(uint32_t)v.size(); quad(v, L,T, M,MidY, 0.5f, 30,50,70,255);
      NvkTevBatch b=base(s,0x4001); b.blend_mode=1; b.src_factor=1; b.dst_factor=1; batches.push_back(b); }

    // ── TR: alpha-over. bg(0,0,200) opaque, fg(200,0,0,a=128) src=SRC_ALPHA dst=1-SRC_ALPHA.
    //   R=200*128/255≈100, B=200*127/255≈100 → ~(100,0,100) (GPU float blend → tol 1).
    { uint32_t s=(uint32_t)v.size(); quad(v, M,T, R,MidY, 0.5f, 0,0,200,255);
      NvkTevBatch b=base(s,0x4002); batches.push_back(b); }
    { uint32_t s=(uint32_t)v.size(); quad(v, M,T, R,MidY, 0.5f, 200,0,0,128);
      NvkTevBatch b=base(s,0x4003); b.blend_mode=1; b.src_factor=4; b.dst_factor=5; batches.push_back(b); }

    // ── BL: depth rejects far. near red(220,20,20) z=0.3 FIRST (test+write), far green z=0.8 LEQUAL
    //   → 0.8<=0.3 false → rejected → RED survives. A depth-ignoring renderer shows green = FAIL.
    { uint32_t s=(uint32_t)v.size(); quad(v, L,MidY, M,B, 0.3f, 220,20,20,255);
      NvkTevBatch b=base(s,0x4004); b.z_test=1; b.z_func=3; b.z_write=1; batches.push_back(b); }
    { uint32_t s=(uint32_t)v.size(); quad(v, L,MidY, M,B, 0.8f, 20,220,20,255);
      NvkTevBatch b=base(s,0x4005); b.z_test=1; b.z_func=3; b.z_write=1; batches.push_back(b); }

    // ── BR: depth accepts near. far green z=0.8 FIRST, near red z=0.3 LEQUAL → 0.3<=0.8 true → RED
    //   overwrites. Together with BL this proves the sort is order-INDEPENDENT (true depth, not painter).
    { uint32_t s=(uint32_t)v.size(); quad(v, M,MidY, R,B, 0.8f, 20,220,20,255);
      NvkTevBatch b=base(s,0x4006); b.z_test=1; b.z_func=3; b.z_write=1; batches.push_back(b); }
    { uint32_t s=(uint32_t)v.size(); quad(v, M,MidY, R,B, 0.3f, 220,20,20,255);
      NvkTevBatch b=base(s,0x4007); b.z_test=1; b.z_func=3; b.z_write=1; batches.push_back(b); }

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), batches.data(), (int)batches.size());
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    const int lo0=0, hi0=28, lo1=36, hi1=64;
    region("TL additive",   buf, lo0,lo0, hi0,hi0, 70,110,150, 0);
    region("TR alpha-over", buf, lo1,lo0, hi1,hi0, 100,0,100,  1);
    region("BL depth-rej",  buf, lo0,lo1, hi0,hi1, 220,20,20,  0);
    region("BR depth-acc",  buf, lo1,lo1, hi1,hi1, 220,20,20,  0);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
