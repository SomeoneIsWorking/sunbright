// fog_test — parity rung for GX fog (J3DFog → GXSetFog), the LOOP-QUEUE #1 divergence.
//
// The plaza sky/water layer carries a GX_FOG_PERSP_LIN (type 2) fog the native renderer did not
// apply, so that far layer stayed full-bright (no distance haze). This test proves the ported fog
// is faithful: it renders a set of full-height strips, each at a KNOWN constant eye-space distance
// (vertex clip.w == -ez), through the SHIPPING shader generator (sb_tev_gen_fragment with a fog-
// enabled NgxPEState — the exact path the live renderer uses), and asserts every strip's readback
// pixel equals the GX linear fog blend HAND-COMPUTED here: mix(base, fogColor, clamp((ze-start)/
// (end-start), 0, 1)). The strips bracket the ramp — below start (clamped 0 = pure base), the two
// interior knees, and at/above end (clamped 1 = pure fog colour) — so a missing fog, a wrong ramp
// slope, an un-clamped factor, or a non-perspective eye-z all FAIL. (A renderer that ignored fog
// would show the base colour in every strip → the interior/far strips fail by ~25-150 per channel.)
//
// The factor is independent of the projection near/far: GXSetFog's A/B reconstruct ze from the
// window depth using the same near/far, so feeding the TRUE eye-space distance (perspective-correct
// vEyeZ) recovers the exact linear ramp from start/end alone (see write_fog in tev_shader.cpp).
#include "gx_sdlgpu.h"
#include "gx_geom.h"
#include "tev_shader.h"        // sb_tev_gen_fragment(NgxTevState)
#include "ngx_render_data.h"   // NgxTevState / NgxPEState

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

using sb::render::NvkTevVertex;
using sb::render::NvkTevBatch;

namespace {
constexpr int W = 96, H = 64;
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
        std::fprintf(stderr,"FAIL %-14s %d/%d wrong; worst (%d,%d)=(%d,%d,%d) exp(%d,%d,%d)\n",
                     what,bad,total,wx,wy,p[0],p[1],p[2],r,g,b); ++g_fails; }
    else std::fprintf(stderr,"ok   %-14s all %d px = (%d,%d,%d)\n",what,total,r,g,b);
}

// Fog params (GX_FOG_PERSP_LIN). Base raster colour the combiner outputs (REPLACE-from-raster).
constexpr float FOG_START = 100.0f, FOG_END = 500.0f;
constexpr int FOG_R = 60, FOG_G = 120, FOG_B = 255;     // fog colour (0..255)
constexpr int BASE_R = 200, BASE_G = 40, BASE_B = 40;   // base (un-fogged) colour

// One vertex at NDC (nx,ny) and eye distance ze. clip.w = ze, clip.xyz = ndc*ze so the GPU divide
// recovers the NDC; vEyeZ (= clip.w) is then exactly ze across the strip (constant → no interp drift).
NvkTevVertex V(float nx, float ny, float ze) {
    NvkTevVertex t{};
    t.x = nx*ze; t.y = ny*ze; t.z = 0.5f*ze; t.w = ze;
    t.rgba[0]=BASE_R/255.f; t.rgba[1]=BASE_G/255.f; t.rgba[2]=BASE_B/255.f; t.rgba[3]=1.f;
    t.uv[0][0]=0.5f; t.uv[0][1]=0.5f;
    return t;
}

// Single-stage REPLACE-from-raster (out = RASC). color_chan=4 (COLOR0A0 → raster=col0).
NgxTevState replace_raster_with_fog() {
    NgxTevState st{}; st.num_stages = 1;
    NgxTevStage& s = st.stage[0];
    s.color_env = (15u<<12)|(15u<<8)|(15u<<4)|10u | (1u<<19);  // a=b=c=ZERO d=RASC, clamp
    s.alpha_env = (7u<<13)|(7u<<10)|(7u<<7)|(4u<<4) | (1u<<19); // alpha REPLACE from TEXA
    s.texcoord = 0; s.texmap = 0; s.color_chan = 4;
    for (auto& t : st.swap_table) t = 0x1B;   // identity
    st.pe.fog_type   = 2;                      // GX_FOG_PERSP_LIN
    st.pe.fog_startz = FOG_START; st.pe.fog_endz = FOG_END;
    st.pe.fog_color[0]=FOG_R; st.pe.fog_color[1]=FOG_G; st.pe.fog_color[2]=FOG_B;
    return st;
}

int mixb(int base, int fog, float f) {   // GPU-faithful: float mix then unorm round
    float o = (base/255.f) * (1.f - f) + (fog/255.f) * f;
    return (int)std::lround(o * 255.f);
}
}

int main() {
    if (!sb::gxsdl::init(W, H)) { std::fprintf(stderr, "FAIL: init (need SDL_VIDEODRIVER=offscreen)\n"); return 1; }
    std::vector<uint8_t> buf((size_t)W * H * 4, 0);

    // Strips bracketing the linear ramp: below start (clamp 0), the two interior knees, at/above end
    // (clamp 1). Each strip is one eye-distance; expected colour = mix(base, fog, clamp ramp).
    const float ze[6]  = { 50.f, 100.f, 200.f, 300.f, 500.f, 650.f };
    const int   N = 6;

    auto factor = [](float z){ float f = (z - FOG_START) / (FOG_END - FOG_START);
                               return f < 0.f ? 0.f : f > 1.f ? 1.f : f; };

    std::string glsl = sb_tev_gen_fragment(replace_raster_with_fog());
    const uint8_t white[4] = { 255,255,255,255 };

    std::vector<NvkTevVertex> verts;
    std::vector<NvkTevBatch> batches(N);
    for (int i = 0; i < N; ++i) {
        // NDC x split into N equal columns; full height y in [-1,1].
        float x0 = -1.f + 2.f*i/N, x1 = -1.f + 2.f*(i+1)/N;
        uint32_t vs = (uint32_t)verts.size();
        verts.push_back(V(x0,-1.f, ze[i]));
        verts.push_back(V(x1,-1.f, ze[i]));
        verts.push_back(V(x1, 1.f, ze[i]));
        verts.push_back(V(x0,-1.f, ze[i]));
        verts.push_back(V(x1, 1.f, ze[i]));
        verts.push_back(V(x0, 1.f, ze[i]));

        NvkTevBatch& b = batches[i]; b = NvkTevBatch{};
        b.vstart = vs; b.vcount = 6;
        b.fragGlsl = glsl.c_str(); b.shaderKey = 0x4000u;   // same shader for all strips
        b.z_test=1; b.z_func=3; b.z_write=1; b.blend_mode=0;
        b.tex[0].rgba=white; b.tex[0].w=1; b.tex[0].h=1;
        b.tex[0].linear=0; b.tex[0].min_filter=0; b.tex[0].wrap_s=0; b.tex[0].wrap_t=0;
        std::memset(&b.push, 0, sizeof b.push);
        std::memset(b.push.kcolor, 0xFF, sizeof b.push.kcolor);
    }

    sb::gxsdl::frame_begin(0,0,0,1);
    sb::gxsdl::draw_tev(verts.data(), (int)verts.size(), batches.data(), N);
    sb::gxsdl::frame_end();
    sb::gxsdl::readback(buf.data(), W, H);

    // Assert each strip interior (3px guard band around the column seams + edges).
    for (int i = 0; i < N; ++i) {
        int cx0 = W*i/N + 3, cx1 = W*(i+1)/N - 3;
        float f = factor(ze[i]);
        int er = mixb(BASE_R, FOG_R, f), eg = mixb(BASE_G, FOG_G, f), eb = mixb(BASE_B, FOG_B, f);
        char nm[24]; std::snprintf(nm, sizeof nm, "ze=%.0f f=%.2f", ze[i], f);
        region(nm, buf, cx0, 3, cx1, H-3, er, eg, eb, 2);
    }

    // Sensitivity: the near (ze<=start) strips MUST be pure base and the far (ze>=end) strip MUST be
    // pure fog colour — i.e. fog actually moved the colour. (Asserted above; a no-fog renderer fails
    // every interior/far strip.) Cross-check the spread is real:
    {
        const uint8_t* near_px = px(buf, W*0/N + W/(2*N), H/2);   // ze=50, factor 0
        const uint8_t* far_px  = px(buf, W*5/N + W/(2*N), H/2);   // ze=650, factor 1
        if (std::abs(near_px[2]-far_px[2]) < 100) {
            std::fprintf(stderr, "FAIL sensitivity: near.b=%d far.b=%d — fog did not move the colour\n",
                         near_px[2], far_px[2]); ++g_fails;
        } else {
            std::fprintf(stderr, "ok   sensitivity   near.b=%d far.b=%d (fog ramp is real)\n",
                         near_px[2], far_px[2]);
        }
    }

    std::fprintf(stderr, "%s (%d failures)\n", g_fails ? "FAILED" : "PASSED", g_fails);
    return g_fails ? 1 : 0;
}
