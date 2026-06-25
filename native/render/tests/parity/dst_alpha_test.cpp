// dst_alpha_test — parity rung for the DESTINATION-ALPHA plane (GXSetColorUpdate /
// GXSetAlphaUpdate / GXSetDstAlpha + the DST_ALPHA / INV_DST_ALPHA blend factors).
//
// This is the mechanism the TModelWaterManager water-volume / silhouette composites use:
//   (1) clear the framebuffer alpha mask to 0   (SMS_FillScreenAlpha: colour OFF, force dst-alpha 0)
//   (2) write the mask to 1 over a region        (colour OFF, alpha ON)
//   (3) composite a colour ONLY where the mask=1 (colour ON, blend DST_ALPHA / INV_DST_ALPHA)
// Without honoring colour/alpha write masks + dst-alpha, step 1/2 wash the scene (visible white
// fill) and step 3 reads a garbage mask (the Delfino "black left"). This asserts, pixel-exact,
// that the colour appears ONLY in the masked half — so a renderer that ignores any of the three
// PE controls FAILS. Spec-truth, no eyeballing.
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
        std::fprintf(stderr,"FAIL %-26s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-26s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

NvkTevVertex V(float x, float y, int rr, int gg, int bb, int aa) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=0.5f; t.w=1.f;
    t.rgba[0]=rr/255.f; t.rgba[1]=gg/255.f; t.rgba[2]=bb/255.f; t.rgba[3]=aa/255.f;
    return t;
}
void quad(std::vector<NvkTevVertex>& v, float x0, float y0, float x1, float y1, int r, int g, int b, int a) {
    v.push_back(V(x0,y0,r,g,b,a)); v.push_back(V(x1,y0,r,g,b,a)); v.push_back(V(x1,y1,r,g,b,a));
    v.push_back(V(x0,y0,r,g,b,a)); v.push_back(V(x1,y1,r,g,b,a)); v.push_back(V(x0,y1,r,g,b,a));
}

// Passthrough fragment (o = vColor); o.a = vColor.a feeds the alpha-plane write.
const char* kPassFrag =
    "#version 450\n"
    "layout(location=0) in vec4 vColor;\n"
    "layout(location=1) in vec2 vUV[8];\n"
    "layout(location=9) in vec4 vColor1;\n"
    "layout(location=0) out vec4 o;\n"
    "layout(set=0, binding=0) uniform sampler2D tex[8];\n"
    "layout(push_constant) uniform Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; } m;\n"
    "void main(){ o = vColor; }\n";

NvkTevBatch base(uint32_t vstart, uint64_t key) {
    NvkTevBatch b{}; b.vstart=vstart; b.vcount=6; b.fragGlsl=kPassFrag; b.shaderKey=key;
    b.z_test=0; b.z_func=3; b.z_write=0; b.blend_mode=0; b.src_factor=1; b.dst_factor=0;
    return b;
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    const float L=-1, M=0, R=1, T=-1, B=1;
    std::vector<NvkTevVertex> v;
    std::vector<NvkTevBatch> batches;

    // (1) clear the alpha mask to 0: fullscreen, COLOUR OFF, ALPHA ON, force dst-alpha 0.
    //     Must NOT touch the (0,0,80) background — proves the colour write mask.
    { uint32_t s=(uint32_t)v.size(); quad(v, L,T, R,B, 255,255,255,255);
      NvkTevBatch b=base(s,0xDA01); b.color_update=0; b.alpha_update=1;
      b.dst_alpha_force=1; b.dst_alpha_val=0; batches.push_back(b); }

    // (2) set the mask to 1 over the LEFT half: COLOUR OFF, ALPHA ON, write fragment alpha (=1)
    //     via alpha blend ONE/ZERO. White colour here must NOT appear (colour mask off).
    { uint32_t s=(uint32_t)v.size(); quad(v, L,T, M,B, 255,255,255,255);
      NvkTevBatch b=base(s,0xDA02); b.color_update=0; b.alpha_update=1;
      b.blend_mode=1; b.src_factor=1 /*ONE*/; b.dst_factor=0 /*ZERO*/; batches.push_back(b); }

    // (3) composite RED only where mask=1: fullscreen, COLOUR ON, blend DST_ALPHA / INV_DST_ALPHA.
    //     left (mask 1): red*1 + bg*0 = red.  right (mask 0): red*0 + bg*1 = bg.
    { uint32_t s=(uint32_t)v.size(); quad(v, L,T, R,B, 220,0,0,255);
      NvkTevBatch b=base(s,0xDA03); b.color_update=1; b.alpha_update=0;
      b.blend_mode=1; b.src_factor=6 /*DST_ALPHA*/; b.dst_factor=7 /*INV_DST_ALPHA*/; batches.push_back(b); }

    sb::gxsdl::frame_begin(0.f, 0.f, 80.f/255.f, 1.f);   // background (0,0,80), clear alpha 1
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), batches.data(), (int)batches.size());
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    // Spec truth: red ONLY in the masked left half; untouched background on the right.
    // Sensitivity — if the renderer ignored dst-alpha (mask treated as 1) the RIGHT would be red;
    // if it ignored the colour write mask, steps 1/2 would have painted white over the background.
    region("masked half = red",   buf, 2,    2, 28, 62, 220,0,0,  1);
    region("unmasked half = bg",  buf, 36,   2, 62, 62, 0,0,80,   1);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
