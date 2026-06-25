// tev_advanced_test — deepens rung 3: TEV generator paths the basic combiner test doesn't reach —
// MULTI-STAGE chaining (stage1 reads stage0's PREV), the SCALE shift, and the PE-block ALPHA TEST
// (discard). All run through the SHIPPING sb_tev_gen_fragment and assert pixel-exact GX truth.
//   TL multi-stage : s0 prev=TEX(REPLACE); s1 prev=prev+KONST(ADD) → (240,140,90).
//   TR scale x2     : MODULATE with scale=1 (<<1 inside the combiner) → (202,101,50).
//   BL alpha PASS   : konst-alpha=200, test (a>128) passes → fg (180,40,40) shows over bg.
//   BR alpha DISCARD: konst-alpha=100, test (a>128) fails → discard → bg (60,60,60) shows.
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

NvkTevVertex V(float x, float y, int rr, int gg, int bb) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=.5f; t.w=1.f;
    t.rgba[0]=rr/255.f; t.rgba[1]=gg/255.f; t.rgba[2]=bb/255.f; t.rgba[3]=1.f;
    t.uv[0][0]=0.5f; t.uv[0][1]=0.5f; return t;
}
void quad(std::vector<NvkTevVertex>& v, float x0, float y0, float x1, float y1, int r, int g, int b) {
    v.push_back(V(x0,y0,r,g,b)); v.push_back(V(x1,y0,r,g,b)); v.push_back(V(x1,y1,r,g,b));
    v.push_back(V(x0,y0,r,g,b)); v.push_back(V(x1,y1,r,g,b)); v.push_back(V(x0,y1,r,g,b));
}

// Color combiner reg: bias/op/clamp(1)/scale/dest configurable; default scale 0, dest PREV.
uint32_t cc(int a, int b, int c, int d, int scale=0, int dest=0) {
    return ((uint32_t)a<<12)|((uint32_t)b<<8)|((uint32_t)c<<4)|(uint32_t)d
         | (1u<<19) | ((uint32_t)(scale&3)<<20) | ((uint32_t)(dest&3)<<22);
}
uint32_t ac_texa() { return (7u<<13)|(7u<<10)|(7u<<7)|(4u<<4)|(1u<<19); }      // alpha = TEXA
uint32_t ac_konsta() { return (7u<<13)|(7u<<10)|(7u<<7)|(6u<<4)|(1u<<19); }    // alpha = KONSTA

void set_stage(NgxTevStage& s, uint32_t ce, uint32_t ae, int kcsel, int kasel) {
    s.color_env=ce; s.alpha_env=ae; s.texcoord=0; s.texmap=0; s.color_chan=4;
    s.kcsel=(uint8_t)kcsel; s.kasel=(uint8_t)kasel;
}
NgxTevState blank() { NgxTevState st{}; for (auto& t: st.swap_table) t=0x1B; return st; }
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    const uint8_t texRGB[4] = { 200,100,50,255 };
    const uint8_t texFG[4]  = { 180,40,40,255 };

    std::vector<NvkTevVertex> v;
    std::vector<std::string> glsl;
    std::vector<NvkTevBatch> batches;
    glsl.reserve(8);

    auto mk = [&](float x0,float y0,float x1,float y1, const NgxTevState& st, const uint8_t* tex,
                  uint64_t key, int kon0r=0,int kon0g=0,int kon0b=0,int kon0a=255) -> void {
        uint32_t s=(uint32_t)v.size(); quad(v, x0,y0,x1,y1, 128,128,128);
        glsl.push_back(sb_tev_gen_fragment(st));
        NvkTevBatch b{}; b.vstart=s; b.vcount=6; b.fragGlsl=glsl.back().c_str(); b.shaderKey=key;
        b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
        b.tex[0].rgba=tex; b.tex[0].w=1; b.tex[0].h=1; b.tex[0].linear=0; b.tex[0].min_filter=0;
        b.tex[0].wrap_s=0; b.tex[0].wrap_t=0;
        std::memset(&b.push,0,sizeof b.push);
        b.push.kcolor[0][0]=kon0r; b.push.kcolor[0][1]=kon0g; b.push.kcolor[0][2]=kon0b; b.push.kcolor[0][3]=kon0a;
        batches.push_back(b);
    };

    // ── TL multi-stage: s0 prev=TEXC (REPLACE); s1 prev = prev + KONST (a=ZERO b=KONSTC c=ONE d=PREVC).
    //   TEX=(200,100,50), KONST=(40,40,40) → (240,140,90).
    { NgxTevState st = blank(); st.num_stages = 2;
      set_stage(st.stage[0], cc(15,15,15,8),       ac_texa(), 0, 0);            // REPLACE tex
      set_stage(st.stage[1], cc(15,14,12,0),       ac_texa(), 12, 0);          // prev + konst
      mk(-1,-1, 0,0, st, texRGB, 0x6000, 40,40,40,255); }

    // ── TR scale x2: MODULATE (a=ZERO b=TEXC c=RASC d=ZERO) with scale=1.
    //   inner=TEX*129; <<1; +128; >>8 → R=202 G=101 B=50.
    { NgxTevState st = blank(); st.num_stages = 1;
      set_stage(st.stage[0], cc(15,8,10,15, /*scale*/1), ac_texa(), 0, 0);
      mk(0,-1, 1,0, st, texRGB, 0x6001); }

    // ── BL alpha PASS: bg(60,60,60) opaque, then fg REPLACE tex(180,40,40) alpha=KONSTA=200, test a>128.
    { NgxTevState bg = blank(); bg.num_stages=1; set_stage(bg.stage[0], cc(15,15,15,8), ac_texa(),0,0);
      const uint8_t texBg[4]={60,60,60,255}; static uint8_t s_texBgL[4]; std::memcpy(s_texBgL,texBg,4);
      mk(-1,0, 0,1, bg, s_texBgL, 0x6002);
      NgxTevState fg = blank(); fg.num_stages=1; set_stage(fg.stage[0], cc(15,15,15,8), ac_konsta(),0,28);
      fg.pe.alpha_test=1; fg.pe.comp0=4; fg.pe.ref0=128; fg.pe.aop=0; fg.pe.comp1=7; fg.pe.ref1=0;
      mk(-1,0, 0,1, fg, texFG, 0x6003, 0,0,0,200); }

    // ── BR alpha DISCARD: same fg but KONSTA=100 (≤128) → discard → bg(60,60,60) survives.
    { NgxTevState bg = blank(); bg.num_stages=1; set_stage(bg.stage[0], cc(15,15,15,8), ac_texa(),0,0);
      static uint8_t s_texBgR[4]={60,60,60,255};
      mk(0,0, 1,1, bg, s_texBgR, 0x6004);
      NgxTevState fg = blank(); fg.num_stages=1; set_stage(fg.stage[0], cc(15,15,15,8), ac_konsta(),0,28);
      fg.pe.alpha_test=1; fg.pe.comp0=4; fg.pe.ref0=128; fg.pe.aop=0; fg.pe.comp1=7; fg.pe.ref1=0;
      mk(0,0, 1,1, fg, texFG, 0x6005, 0,0,0,100); }

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(v.data(), (int)v.size(), batches.data(), (int)batches.size());
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    const int lo0=0, hi0=28, lo1=36, hi1=64;
    region("multi-stage", buf, lo0,lo0, hi0,hi0, 240,140,90, 0);
    region("scale x2",    buf, lo1,lo0, hi1,hi0, 202,101,50, 0);
    region("alpha PASS",  buf, lo0,lo1, hi0,hi1, 180,40,40,  0);
    region("alpha DISCARD",buf, lo1,lo1, hi1,hi1, 60,60,60,  0);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
