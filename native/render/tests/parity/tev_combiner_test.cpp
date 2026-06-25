// tev_combiner_test — rung 3 of the parity-focused renderer TDD ladder: per-TEV-stage combiner math.
//
// Rungs 1-2 proved the renderer rasterizes and samples textures correctly. This rung proves the GX
// TEV color-combiner integer math is faithful: it builds real NgxTevState combiner configs, runs them
// through the SHIPPING shader generator (sb_tev_gen_fragment — the exact function the live renderer
// uses per material), feeds known raster/texture/konst/tevreg inputs through sb::gxsdl, and asserts
// every readback pixel equals the GX integer combiner result HAND-COMPUTED here (literal constants,
// not a shared formula) — so a wrong combiner, a binding-remap bug, a push-constant plumbing error,
// or a clamp mistake all FAIL. The four stages cover the distinct combiner paths:
//   REPLACE (d only), MODULATE (d=0 + lerp a*c), KONST-MODULATE (konst as a combiner input),
//   ADD-with-clamp (d + lerp, channel saturates at 255).
//
// GX color combiner (per write_regular in tev_shader.cpp, faithful to Dolphin PixelShaderGen):
//   out = clamp( ((d + bias) << sL)  OP  ((((a<<8) + (b-a)*(c+(c>>7))) << sL) + lerpBias) >> 8 )
// with bias 0, scale 0, clamp [0,255], OP '+'  → out = clamp(d + ((a<<8 + (b-a)*(c+(c>>7)) + 128) >> 8)).
#include "gx_sdlgpu.h"
#include "gx_geom.h"
#include "tev_shader.h"        // sb_tev_gen_fragment(NgxTevState)
#include "ngx_render_data.h"   // NgxTevState / NgxTevStage

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

// Assert a rectangular region [x0,x1)x[y0,y1) is uniformly (r,g,b) within tol (skips a guard band the
// caller passes via the rect itself). Called with tol 0: the GX integer combiner is pixel-EXACT here
// (solid NEAREST texture + exact raster + round() in-shader), so a 1-unit error must FAIL.
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

// A vertex with a solid raster colour (0..255 per channel) and constant UV (solid texture sampled).
NvkTevVertex V(float x, float y, int rr, int gg, int bb) {
    NvkTevVertex t{}; t.x=x; t.y=y; t.z=.5f; t.w=1.f;
    t.rgba[0]=rr/255.f; t.rgba[1]=gg/255.f; t.rgba[2]=bb/255.f; t.rgba[3]=1.f;
    t.rgba1[0]=t.rgba1[1]=t.rgba1[2]=t.rgba1[3]=0.f;
    t.uv[0][0]=0.5f; t.uv[0][1]=0.5f; return t;
}

// Build one combiner-color reg value (decode_cc layout). bias/op/scale 0, clamp 1, dest PREV.
uint32_t cc(int a, int b, int c, int d) {
    return ((uint32_t)a<<12)|((uint32_t)b<<8)|((uint32_t)c<<4)|(uint32_t)d | (1u<<19);
}
// Alpha combiner = REPLACE from TEXA (keeps prev.a sane; no alpha test set so it never discards).
uint32_t ac_replace_texa() {
    return (7u<<13)|(7u<<10)|(7u<<7)|(4u<<4) | (1u<<19);
}

// A single-stage NgxTevState with the given color combiner. color_chan=4 (COLOR0A0 → raster=col0),
// texmap0/texcoord0, kcsel selects KONST0.rgb (12). All swap tables identity.
NgxTevState one_stage(uint32_t color_env) {
    NgxTevState st{}; st.num_stages = 1;
    NgxTevStage& s = st.stage[0];
    s.color_env = color_env;
    s.alpha_env = ac_replace_texa();
    s.texcoord = 0; s.texmap = 0; s.color_chan = 4;
    s.kcsel = 12;   // KSEL_C[12] = m.kcolor[0].rgb
    s.kasel = 0;    // KSEL_A[0]  = 255 (unused; no konst-alpha input)
    for (auto& t : st.swap_table) t = 0x1B;   // identity
    return st;
}

struct Case {
    const char* name;
    uint32_t color_env;
    int rasR, rasG, rasB;       // raster colour (vertex)
    int konR, konG, konB;       // push.kcolor[0]
    int expR, expG, expB;       // HAND-COMPUTED GX integer-combiner result
};

// Texture is a solid RGBA8 texel (200,100,50) for every case (textemp = 200,100,50).
constexpr int TR=200, TG=100, TB=50;
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // GX combiner-input enum (color): ZERO=15, TEXC=8, RASC=10, KONSTC=14, ONE(255)=12.
    // Four cases, each a distinct combiner path. Expected values are hand-derived from the integer
    // formula at the top of this file (TEX=200,100,50; see each case for the arithmetic).
    const Case cases[4] = {
        // REPLACE: out = TEXC. d=TEXC, a=b=c=ZERO → lerp=(0+128)>>8=0; out=TEX. = (200,100,50).
        { "REPLACE", cc(15,15,15,8), 128,128,128, 0,0,0, 200,100,50 },
        // MODULATE: out = TEX*RAS. a=ZERO,b=TEXC,c=RASC,d=ZERO. RAS=128 → c+(c>>7)=129.
        //   R:(200*129+128)>>8=101  G:(100*129+128)>>8=50  B:(50*129+128)>>8=25.
        { "MODULATE", cc(15,8,10,15), 128,128,128, 0,0,0, 101,50,25 },
        // KONST-MODULATE: out = TEX*KONST. a=ZERO,b=TEXC,c=KONSTC,d=ZERO. KON=200 → 201.
        //   R:(200*201+128)>>8=157  G:(100*201+128)>>8=79  B:(50*201+128)>>8=39.
        { "KONST_MOD", cc(15,8,14,15), 128,128,128, 200,200,200, 157,79,39 },
        // ADD+clamp: out = TEX + RAS. a=ZERO,b=RASC,c=ONE(255),d=TEXC. lerp=RAS*256>>8=128.
        //   R:200+128=328→255  G:100+128=228  B:50+128=178.
        { "ADD_CLAMP", cc(15,10,12,8), 128,128,128, 0,0,0, 255,228,178 },
    };

    // Solid texture (one texel) — NEAREST so textemp is exactly the texel.
    const uint8_t tex[4] = { (uint8_t)TR, (uint8_t)TG, (uint8_t)TB, 255 };

    // Each case fills one NDC quadrant (y-down clip: NDC y=-1 = top; rung-1 orientation).
    // TL: x[-1,0] y[-1,0]; TR: x[0,1] y[-1,0]; BL: x[-1,0] y[0,1]; BR: x[0,1] y[0,1].
    const float qx[4][2] = { {-1,0},{0,1},{-1,0},{0,1} };
    const float qy[4][2] = { {-1,0},{-1,0},{0,1},{0,1} };

    std::vector<NvkTevVertex> verts;
    std::vector<std::string> glsl(4);   // keep shader sources alive (batch holds const char*)
    NvkTevBatch batches[4];

    for (int i = 0; i < 4; ++i) {
        const Case& C = cases[i];
        const float x0=qx[i][0], x1=qx[i][1], y0=qy[i][0], y1=qy[i][1];
        uint32_t vs = (uint32_t)verts.size();
        verts.push_back(V(x0,y0, C.rasR,C.rasG,C.rasB));
        verts.push_back(V(x1,y0, C.rasR,C.rasG,C.rasB));
        verts.push_back(V(x1,y1, C.rasR,C.rasG,C.rasB));
        verts.push_back(V(x0,y0, C.rasR,C.rasG,C.rasB));
        verts.push_back(V(x1,y1, C.rasR,C.rasG,C.rasB));
        verts.push_back(V(x0,y1, C.rasR,C.rasG,C.rasB));

        NgxTevState st = one_stage(C.color_env);
        glsl[i] = sb_tev_gen_fragment(st);

        NvkTevBatch& b = batches[i];
        b = NvkTevBatch{};
        b.vstart = vs; b.vcount = 6;
        b.fragGlsl = glsl[i].c_str();
        b.shaderKey = 0x3000u + i;
        b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
        b.tex[0].rgba=tex; b.tex[0].w=1; b.tex[0].h=1;
        b.tex[0].linear=0; b.tex[0].min_filter=0; b.tex[0].wrap_s=0; b.tex[0].wrap_t=0;
        // push: kcolor[0] = this case's konst; tevreg all 0 (PREV/C0/C1/C2 init = 0, unused here).
        std::memset(&b.push, 0, sizeof b.push);
        b.push.kcolor[0][0]=C.konR; b.push.kcolor[0][1]=C.konG;
        b.push.kcolor[0][2]=C.konB; b.push.kcolor[0][3]=255;
    }

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), batches, 4);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    // Assert each quadrant interior (4px guard band around the x/y=32 seam).
    const int lo0=0, hi0=28, lo1=36, hi1=64;
    struct Q { int x0,y0,x1,y1; } quad[4] = {
        {lo0,lo0,hi0,hi0}, {lo1,lo0,hi1,hi0}, {lo0,lo1,hi0,hi1}, {lo1,lo1,hi1,hi1} };
    for (int i = 0; i < 4; ++i)
        region(cases[i].name, buf, quad[i].x0,quad[i].y0,quad[i].x1,quad[i].y1,
               cases[i].expR,cases[i].expG,cases[i].expB, 0);

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
