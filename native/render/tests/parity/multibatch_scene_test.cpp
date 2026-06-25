// multibatch_scene_test — rung 5: a multi-batch composite scene (combiner + depth + blend interact).
//
// Rungs 1-4 tested each renderer feature in isolation. This rung composites FOUR batches with
// DIFFERENT materials/pipelines into one frame and asserts the final image per region against
// hand-computed truth — proving batch ordering, per-batch pipeline switching (shader/blend/depth
// cache keyed by state), and cross-batch depth + blend all compose correctly:
//   B0 background  full-screen REPLACE texture (50,50,50), z=0.9 write       → whole screen grey.
//   B1 centre      MODULATE tex(200,100,50)*ras(128) = (101,50,25), z=0.5    → overwrites bg centre.
//   B2 fg-left     alpha-over (200,0,0,a=128) over B1's (101,50,25), z=0.3   → composited (tol 1).
//   B3 reject      green (0,255,0) at z=0.95 over the centre-right, LEQUAL   → REJECTED (0.95>0.9).
// B3 makes the modulate-only region a cross-batch DEPTH-REJECT assertion: if depth is broken the
// region turns green and FAILS.
#include "gx_sdlgpu.h"
#include "gx_geom.h"
#include "tev_shader.h"
#include "ngx_render_data.h"

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
        std::fprintf(stderr,"FAIL %-20s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-20s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

NvkTevVertex V(float x, float y, float z, int rr, int gg, int bb, int aa) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=z; t.w=1.f;
    t.rgba[0]=rr/255.f; t.rgba[1]=gg/255.f; t.rgba[2]=bb/255.f; t.rgba[3]=aa/255.f;
    t.uv[0][0]=0.5f; t.uv[0][1]=0.5f; return t;
}
void quad(std::vector<NvkTevVertex>& v, float x0, float y0, float x1, float y1, float z,
          int r, int g, int b, int a) {
    v.push_back(V(x0,y0,z,r,g,b,a)); v.push_back(V(x1,y0,z,r,g,b,a)); v.push_back(V(x1,y1,z,r,g,b,a));
    v.push_back(V(x0,y0,z,r,g,b,a)); v.push_back(V(x1,y1,z,r,g,b,a)); v.push_back(V(x0,y1,z,r,g,b,a));
}

uint32_t cc(int a, int b, int c, int d) {
    return ((uint32_t)a<<12)|((uint32_t)b<<8)|((uint32_t)c<<4)|(uint32_t)d | (1u<<19);
}
// Single-stage TEV state with the given colour combiner; alpha = REPLACE from TEXA.
NgxTevState one_stage(uint32_t color_env) {
    NgxTevState st{}; st.num_stages = 1;
    st.stage[0].color_env = color_env;
    st.stage[0].alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(4u<<4)|(1u<<19);
    st.stage[0].texcoord = 0; st.stage[0].texmap = 0; st.stage[0].color_chan = 4;
    for (auto& t : st.swap_table) t = 0x1B;
    return st;
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
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    const uint8_t texGrey[4] = { 50,50,50,255 };       // bg REPLACE source
    const uint8_t texRGB[4]  = { 200,100,50,255 };     // centre MODULATE source

    std::vector<NvkTevVertex> v;
    std::vector<std::string> glsl(2);
    std::vector<NvkTevBatch> batches;

    auto add_tev = [&](float x0,float y0,float x1,float y1,float z, uint32_t color_env,
                       const uint8_t* tex, int gi, uint64_t key) {
        uint32_t s=(uint32_t)v.size(); quad(v, x0,y0,x1,y1,z, 128,128,128,255);
        glsl[gi] = sb_tev_gen_fragment(one_stage(color_env));
        NvkTevBatch b{}; b.vstart=s; b.vcount=6; b.fragGlsl=glsl[gi].c_str(); b.shaderKey=key;
        b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
        b.tex[0].rgba=tex; b.tex[0].w=1; b.tex[0].h=1; b.tex[0].linear=0; b.tex[0].min_filter=0;
        b.tex[0].wrap_s=0; b.tex[0].wrap_t=0;
        std::memset(&b.push,0,sizeof b.push);
        batches.push_back(b);
    };
    auto add_pass = [&](float x0,float y0,float x1,float y1,float z, int r,int g,int bl,int a,
                        bool blend, uint64_t key) {
        uint32_t s=(uint32_t)v.size(); quad(v, x0,y0,x1,y1,z, r,g,bl,a);
        NvkTevBatch b{}; b.vstart=s; b.vcount=6; b.fragGlsl=kPassFrag; b.shaderKey=key;
        b.z_test=1; b.z_func=3; b.z_write=1;
        if (blend) { b.blend_mode=1; b.src_factor=4; b.dst_factor=5; }
        batches.push_back(b);
    };

    // B0 background (REPLACE = d:TEXC), B1 centre (MODULATE = b:TEXC c:RASC).
    add_tev(-1,-1, 1,1, 0.9f, cc(15,15,15,8),  texGrey, 0, 0x5000);
    add_tev(-0.5f,-0.5f, 0.5f,0.5f, 0.5f, cc(15,8,10,15), texRGB, 1, 0x5001);
    // B2 alpha-over the centre-LEFT half; B3 green at z=0.95 over the centre-RIGHT half (depth-rejected).
    add_pass(-0.5f,-0.5f, 0.0f,0.5f, 0.3f,  200,0,0,128,  true,  0x5002);
    add_pass( 0.0f,-0.5f, 0.5f,0.5f, 0.95f, 0,255,0,255,  false, 0x5003);

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), batches.data(), (int)batches.size());
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    // NDC[-0.5,0.5] → pixels [16,48]; centre seam x=32. Guard bands around every edge.
    region("bg grey",       buf, 2,2,   14,14,  50,50,50,    0);   // outside centre = REPLACE bg
    region("modulate(depth)",buf, 34,18, 46,46,  101,50,25,   0);   // B1 survives; B3 green rejected
    region("alpha-over",    buf, 18,18, 30,46,  151,25,12,    1);   // B2 over B1: 200*.502+101*.498 etc

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
