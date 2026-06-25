// texture_filter_test — extends rung 2: GX sampler WRAP modes + bilinear MAG filtering faithfulness.
//
// A 2x1 texture (texel0=red, texel1=green) is sampled across four full-width strips with UV that runs
// past [0,1], exercising each address mode + the linear/nearest mag filter that gx_sdlgpu derives from
// the batch's GX tex state (wrap_s 0=CLAMP 1=REPEAT 2=MIRROR; linear = mag LINEAR). NEAREST strips are
// asserted to exact solid bands (tol 0); the bilinear strip asserts the endpoints clamp to pure red/
// green AND the centre is a genuine blend (catches a nearest sampler, which would give a hard edge).
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
        std::fprintf(stderr,"FAIL %-22s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-22s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

NvkTevVertex V(float x, float y, float u, float v) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=.5f; t.w=1.f;
    t.rgba[0]=t.rgba[1]=t.rgba[2]=t.rgba[3]=1.f;
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

// One full-width strip (rows [i*16,(i+1)*16)) with UV running uL..uR across it.
void strip(std::vector<NvkTevVertex>& v, int i, float uL, float uR) {
    float y0 = -1.f + i*0.5f, y1 = y0 + 0.5f;
    v.push_back(V(-1,y0,uL,0.5f)); v.push_back(V(1,y0,uR,0.5f)); v.push_back(V(1,y1,uR,0.5f));
    v.push_back(V(-1,y0,uL,0.5f)); v.push_back(V(1,y1,uR,0.5f)); v.push_back(V(-1,y1,uL,0.5f));
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    const uint8_t tex[2*4] = { 255,0,0,255,  0,255,0,255 };   // 2x1: red, green

    std::vector<NvkTevVertex> v;
    strip(v, 0, 0.f, 2.f);   // REPEAT
    strip(v, 1, 0.f, 2.f);   // MIRROR
    strip(v, 2, 0.f, 2.f);   // CLAMP
    strip(v, 3, 0.f, 1.f);   // bilinear (CLAMP, u 0..1)

    NvkTevBatch batches[4];
    for (int i = 0; i < 4; ++i) {
        NvkTevBatch& b = batches[i]; b = NvkTevBatch{};
        b.vstart = (uint32_t)(i*6); b.vcount = 6; b.fragGlsl = kTexFrag; b.shaderKey = 0x7100u + i;
        b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
        b.tex[0].rgba=tex; b.tex[0].w=2; b.tex[0].h=1; b.tex[0].min_filter=0;
    }
    batches[0].tex[0].wrap_s=1; batches[0].tex[0].wrap_t=1; batches[0].tex[0].linear=0;  // REPEAT, nearest
    batches[1].tex[0].wrap_s=2; batches[1].tex[0].wrap_t=2; batches[1].tex[0].linear=0;  // MIRROR
    batches[2].tex[0].wrap_s=0; batches[2].tex[0].wrap_t=0; batches[2].tex[0].linear=0;  // CLAMP
    batches[3].tex[0].wrap_s=0; batches[3].tex[0].wrap_t=0; batches[3].tex[0].linear=1;  // CLAMP, LINEAR mag

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), batches, 4);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    const int R=255, G=255;
    // Strip 0 REPEAT: u 0..2 → frac bands red/green/red/green at px [0,16)[16,32)[32,48)[48,64).
    region("REPEAT band0 red",   buf, 2,2,   14,14,  R,0,0, 0);
    region("REPEAT band1 green", buf, 18,2,  30,14,  0,G,0, 0);
    region("REPEAT band2 red",   buf, 34,2,  46,14,  R,0,0, 0);
    region("REPEAT band3 green", buf, 50,2,  62,14,  0,G,0, 0);
    // Strip 1 MIRROR: red/green/green/red.
    region("MIRROR band0 red",   buf, 2,18,  14,30,  R,0,0, 0);
    region("MIRROR band1 green", buf, 18,18, 30,30,  0,G,0, 0);
    region("MIRROR band2 green", buf, 34,18, 46,30,  0,G,0, 0);
    region("MIRROR band3 red",   buf, 50,18, 62,30,  R,0,0, 0);
    // Strip 2 CLAMP: red then green clamped for u>1.
    region("CLAMP red",          buf, 2,34,  14,46,  R,0,0, 0);
    region("CLAMP green(clamped)",buf, 18,34, 62,46,  0,G,0, 0);
    // Strip 3 bilinear: endpoints clamp to pure red/green; centre is a genuine blend (not a hard edge).
    region("BILINEAR left red",  buf, 2,50,  12,62,  R,0,0, 1);
    region("BILINEAR right green",buf, 52,50, 62,62,  0,G,0, 1);
    { // centre column band: must be blended — both R and G mid-range, B~0. A NEAREST sampler fails this.
        int bad=0; for (int y=52;y<60;++y) for (int x=30;x<34;++x) {
            const uint8_t* p=px(buf,x,y);
            if (!(p[0]>=60 && p[0]<=200 && p[1]>=60 && p[1]<=200 && p[2]<10)) ++bad; }
        if (bad) { std::fprintf(stderr,"FAIL BILINEAR centre blend  %d px not blended\n",bad); ++g_fails; }
        else std::fprintf(stderr,"ok   BILINEAR centre blend  red+green mixed (linear filter on)\n");
    }

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
