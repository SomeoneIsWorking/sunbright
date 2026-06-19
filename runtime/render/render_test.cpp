// render_test — the renderer's unit-test suite (the ngx equivalent of
// sunbright-recomp-test). Bottom-up TDD: each pure renderer unit asserts against
// SPEC-COMPUTED ground truth (hand-derived expected values), NOT against another
// renderer's pixels. Dolphin-free and self-contained so it runs in ctest with no
// ROM, no GPU, no running game.
//
// Why this exists: the renderer was built straight to "draw the whole scene and
// eyeball it," so fidelity bugs (the projection wash, dropped ortho) could never
// be made to go red/green and theories about them couldn't be falsified. This
// suite decomposes the renderer into testable units so a fix MOVES A NUMBER.
//
// Add a unit: write a `static int test_<unit>(char* rep, int cap)` returning the
// failing-case count (0 = pass), register it in kUnits below.

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "ngx_project.h"
#include "ngx_efb_copy.h"
#include "ngx_clip.h"
#include "ngx_light.h"
#include "ngx_imm_geom.h"
#include "ngx_jpa_billboard.h"
#include "ngx_indirect.h"
#include "tev_shader.h"
#include "tev_eval.h"
#include "tex_decode.h"

// ── Units under test (self-test entry points defined in their own .cpp) ──────
extern int sb_ngx_vertex_selftest(char* out, int cap);   // runtime/ngx/ngx_vertex.cpp

namespace {

// ── projection unit ──────────────────────────────────────────────────────────
// Spec-computed ground truth: hand-multiplied P·(eye,1) then the divide, so a
// row-major/column-major slip, a wrong w-row, or a missing ortho path goes red.
int test_projection(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto fail = [&](const char* fmt, float a, float b, float c, float d) {
        fails++; if (pos < cap)
            pos += snprintf(rep + pos, cap - pos, fmt, a, b, c, d);
    };
    auto close = [](float a, float b) { float e = a - b; return (e < 0 ? -e : e) <= 1e-5f; };
    float clip[4], nx, ny;

    // 1. Perspective: row3 = [0,0,-1,0] → w = -ez. Clean synthetic matrix.
    //    P·(1,1,-5): cx=2, cy=3, cz=(-1)(-5)+(-4)(1)=1, cw=(-1)(-5)=5 → NDC (0.4,0.6)
    {
        const float P[16] = { 2,0,0,0,  0,3,0,0,  0,0,-1,-4,  0,0,-1,0 };
        ngx_project_eye(P, 1, 1, -5, clip);
        if (!close(clip[0],2) || !close(clip[1],3) || !close(clip[2],1) || !close(clip[3],5))
            fail("FAIL persp clip got(%.4f,%.4f,%.4f,%.4f) exp(2,3,1,5)\n", clip[0],clip[1],clip[2],clip[3]);
        if (!ngx_ndc_xy(clip, nx, ny) || !close(nx,0.4f) || !close(ny,0.6f))
            fail("FAIL persp ndc got(%.4f,%.4f) exp(0.4,0.6) [w=%.4f]%c\n", nx,ny,clip[3],' ');
    }

    // 2. Orthographic: row3 = [0,0,0,1] → w = 1 (no foreshortening), translation in col3.
    //    P·(2,4,-10): cx=0.5*2+0.25=1.25, cy=0.5*4+0.5=2.5, cz=-0.1*-10-0.2=0.8, cw=1 → NDC (1.25,2.5)
    {
        const float P[16] = { 0.5f,0,0,0.25f,  0,0.5f,0,0.5f,  0,0,-0.1f,-0.2f,  0,0,0,1 };
        ngx_project_eye(P, 2, 4, -10, clip);
        if (!close(clip[0],1.25f) || !close(clip[1],2.5f) || !close(clip[2],0.8f) || !close(clip[3],1.0f))
            fail("FAIL ortho clip got(%.4f,%.4f,%.4f,%.4f) exp(1.25,2.5,0.8,1)\n", clip[0],clip[1],clip[2],clip[3]);
        if (!ngx_ndc_xy(clip, nx, ny) || !close(nx,1.25f) || !close(ny,2.5f))
            fail("FAIL ortho ndc got(%.4f,%.4f) exp(1.25,2.5) [w=%.4f]%c\n", nx,ny,clip[3],' ');
    }

    // 3. Behind the eye (perspective): ez>0 → w<=0 → ngx_ndc_xy must REJECT (no divide).
    {
        const float P[16] = { 2,0,0,0,  0,3,0,0,  0,0,-1,-4,  0,0,-1,0 };
        ngx_project_eye(P, 0, 0, 5, clip);                 // cw = -5
        if (ngx_ndc_xy(clip, nx, ny))
            fail("FAIL behind-eye should reject w=%.4f%c%c%c\n", clip[3],' ',' ',' ');
    }
    return fails;
}

// ── near-plane clip unit ─────────────────────────────────────────────────────
// stride=4 (clip xyzw only) keeps the SH crossing math hand-checkable. Plane is
// d = z + w ≥ 0. Crossing point = a + s·(b-a), s = da/(da-db), lands on d=0.
int test_clip(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto fail = [&](const char* msg) { fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos, "FAIL clip %s\n", msg); };
    auto close = [](float a, float b) { float e = a - b; return (e < 0 ? -e : e) <= 1e-5f; };
    float out[16]; int nf;

    // A. Wholly in front (all d≥0) → 3, passthrough.
    { const float in[12] = { 0,0,1,1,  1,0,1,1,  0,1,1,1 };   // d = 2,2,2
      int n = ngx_clip_near_tri(in, 4, out, &nf);
      if (n != 3 || nf != 3) fail("A: in-front count");
      for (int i = 0; i < 12; i++) if (!close(out[i], in[i])) { fail("A: passthrough"); break; } }

    // B. Wholly behind (all d<0) → 0, dropped.
    { const float in[12] = { 0,0,-2,1,  1,0,-2,1,  0,1,-2,1 };   // d = -1,-1,-1
      int n = ngx_clip_near_tri(in, 4, out, &nf);
      if (n != 0 || nf != 0) fail("B: behind drop"); }

    // C. One vertex behind (v2) → 4-vertex polygon. v0=(0,0,1,1)d2 v1=(1,0,1,1)d2 v2=(0,1,-2,1)d-1.
    //    Crossings on edges v1→v2 (s=2/3 → (1/3,2/3,-1,1)) and v2→v0 (s=1/3 → (0,2/3,-1,1)).
    { const float in[12] = { 0,0,1,1,  1,0,1,1,  0,1,-2,1 };
      int n = ngx_clip_near_tri(in, 4, out, &nf);
      const float exp[16] = { 0,0,1,1,  1,0,1,1,  1.0f/3,2.0f/3,-1,1,  0,2.0f/3,-1,1 };
      if (n != 4 || nf != 2) fail("C: one-behind count");
      else for (int i = 0; i < 16; i++) if (!close(out[i], exp[i])) {
          fail("C: clipped verts"); if (pos < cap) pos += snprintf(rep+pos, cap-pos,
              "   out[%d]=%.4f exp=%.4f\n", i, out[i], exp[i]); break; } }

    // D. Two vertices behind (v1,v2), v0 front → 3-vertex polygon.
    //    v0=(0,0,1,1)d2 v1=(1,0,-2,1)d-1 v2=(0,1,-2,1)d-1.
    //    Crossings v0→v1 (s=2/3 → (2/3,0,-1,1)) and v2→v0 (s=1/3 → (0,2/3,-1,1)).
    { const float in[12] = { 0,0,1,1,  1,0,-2,1,  0,1,-2,1 };
      int n = ngx_clip_near_tri(in, 4, out, &nf);
      const float exp[12] = { 0,0,1,1,  2.0f/3,0,-1,1,  0,2.0f/3,-1,1 };
      if (n != 3 || nf != 1) fail("D: two-behind count");
      else for (int i = 0; i < 12; i++) if (!close(out[i], exp[i])) {
          fail("D: clipped verts"); if (pos < cap) pos += snprintf(rep+pos, cap-pos,
              "   out[%d]=%.4f exp=%.4f\n", i, out[i], exp[i]); break; } }
    return fails;
}

// ── full-frustum clip unit ───────────────────────────────────────────────────
// ngx_clip_frustum_tri clips a triangle against all 6 clip-space planes. The KEY
// invariant: every output vertex lies inside ALL planes (within tolerance), so no
// off-screen / behind-camera geometry survives to project as a screen-spanning spike
// (the title-logo / file-select-transition shred the near-only clip left behind).
int test_clip_frustum(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto fail = [&](const char* msg) { fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos, "FAIL frustum %s\n", msg); };
    auto close = [](float a, float b) { float e = a - b; return (e < 0 ? -e : e) <= 1e-4f; };
    // inside = all 6 half-spaces ≥ -eps: x+w, w-x, y+w, w-y, z+w, -z
    auto inside_all = [](const float* v) {
        const float w = v[3];
        return v[0]+w >= -1e-4f && w-v[0] >= -1e-4f &&
               v[1]+w >= -1e-4f && w-v[1] >= -1e-4f &&
               v[2]+w >= -1e-4f &&    -v[2] >= -1e-4f;
    };
    float out[9 * 4];

    // A. Wholly inside → 3, passthrough (verify the invariant + count).
    { const float in[12] = { 0,0,-0.5f,1,  0.3f,0,-0.5f,1,  0,0.3f,-0.5f,1 };
      int n = ngx_clip_frustum_tri(in, 4, out);
      if (n != 3) fail("A: inside count");
      for (int e = 0; e < n; e++) if (!inside_all(&out[e*4])) { fail("A: inside-invariant"); break; }
      for (int i = 0; i < 12; i++) if (!close(out[i], in[i])) { fail("A: passthrough"); break; } }

    // B. Wholly off to the right (all x > w) → 0 (the off-screen-object case the near-only
    //    clip would have kept as a sliver).
    { const float in[12] = { 5,0,-0.5f,1,  6,0,-0.5f,1,  5,1,-0.5f,1 };  // x≫w
      int n = ngx_clip_frustum_tri(in, 4, out);
      if (n != 0) fail("B: off-right drop"); }

    // C. The SPIKE case: one vertex straddling the near plane far off-axis (NDC.x huge).
    //    near-only clip keeps the interpolated near-plane vertex at x≫w (spike); full clip
    //    must bound EVERY output vertex inside the frustum (no spike survives).
    { const float in[12] = { 0,0,-1,1,   0.5f,0,-1,1,   30,-12,-0.01f,0.01f };
      int n = ngx_clip_frustum_tri(in, 4, out);
      if (n < 3) fail("C: spike fully dropped (expected a clipped poly)");
      for (int e = 0; e < n; e++) if (!inside_all(&out[e*4])) {
          fail("C: spike NOT bounded"); if (pos < cap) pos += snprintf(rep+pos, cap-pos,
              "   out[%d]=(%.3f,%.3f,%.3f,%.3f) x/w=%.1f\n", e, out[e*4],out[e*4+1],out[e*4+2],out[e*4+3],
              out[e*4+3]>1e-6f?out[e*4]/out[e*4+3]:0); break; } }

    // D. Behind-camera vertex (w<0) → its part is removed; survivors bounded.
    { const float in[12] = { 0,0,-0.5f,1,  0.2f,0,-0.5f,1,  0,0.2f,5,-1 };  // v2 behind (w=-1)
      int n = ngx_clip_frustum_tri(in, 4, out);
      for (int e = 0; e < n; e++) if (!inside_all(&out[e*4])) { fail("D: behind not bounded"); break; } }
    return fails;
}

// ── lighting unit ────────────────────────────────────────────────────────────
// GX per-vertex colour-channel lighting (ngx::light_color0). Spec-computed ground
// truth (hand-derived from the GX model / Dolphin LightingShaderGen): out = mat *
// clamp(amb + Σ attn·diff·lightcol). A wrong diffuse sign, a missing clamp, a
// SPOT-attenuation slip, or a mat/amb-source swap goes red. THIS is the function the
// override ships (ngx_j3d_shape.cpp light_vertex calls it) — not a fork.
int test_lighting(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto fail = [&](const char* m, float g0, float g1, float g2) {
        fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos,
            "FAIL lighting %s got(%.4f,%.4f,%.4f)\n", m, g0, g1, g2); };
    auto close = [](float a, float b) { float e = a - b; return (e<0?-e:e) <= 1e-4f; };
    auto chk = [&](const char* m, const float o[3], float e0, float e1, float e2) {
        if (!close(o[0],e0)||!close(o[1],e1)||!close(o[2],e2)) fail(m, o[0],o[1],o[2]); };

    // Light at the origin; vertex 10 units in front (eye -z). ld = (0,0,1), dist=10.
    ngx::LightSrc L[8];  // all invalid by default
    auto mkL = [&](float r,float g,float b){ ngx::LightSrc s; s.valid=true;
        s.color[0]=r;s.color[1]=g;s.color[2]=b; s.pos[0]=0;s.pos[1]=0;s.pos[2]=0;
        s.dir[0]=0;s.dir[1]=0;s.dir[2]=1; return s; };
    const float eye[3]   = {0,0,-10};
    const float nUp[3]   = {0,0,1};    // faces the light (ndl=+1)
    const float nAway[3] = {0,0,-1};   // faces away   (ndl=-1)
    const float white[3] = {1,1,1}, black[3] = {0,0,0};
    float out[3];
    auto C = [](bool mv,bool en,bool av,int df,int at,unsigned mask){
        ngx::ChanCtl c; c.matVtx=mv;c.enable=en;c.ambVtx=av;c.diffFn=df;c.attnSel=at;c.mask=mask; return c; };

    // 1. Lighting DISABLED → out = matColor (no light/ambient touched).
    { float mat[3]={0.2f,0.4f,0.6f};
      ngx::light_color0(C(0,0,0,0,0,0), mat, white, L, eye, nUp, black, out);
      chk("disabled→matColor", out, 0.2f,0.4f,0.6f); }

    // 2. matSource=VTX, disabled → out = vertex colour.
    { float vc[3]={0.8f,0.1f,0.2f};
      ngx::light_color0(C(1,0,0,0,0,0), white, white, L, eye, nUp, vc, out);
      chk("vtx-mat", out, 0.8f,0.1f,0.2f); }

    // 3. Enabled, ambient only (no lights in mask) → out = mat*amb.
    { float amb[3]={0.3f,0.3f,0.3f};
      ngx::light_color0(C(0,1,0,2,0,0), white, amb, L, eye, nUp, black, out);
      chk("ambient-only", out, 0.3f,0.3f,0.3f); }

    // 4. One light, attn NONE, CLAMP diffuse, facing light → mat*(0 + 1·1·0.5)=0.5.
    { L[0]=mkL(0.5f,0.5f,0.5f);
      ngx::light_color0(C(0,1,0,2,0,0x01), white, black, L, eye, nUp, black, out);
      chk("clamp-facing", out, 0.5f,0.5f,0.5f); }

    // 5. Facing AWAY, SIGN diffuse → illum = -0.5 → clamp 0 → black (the dark-surface case).
    { L[0]=mkL(0.5f,0.5f,0.5f);
      ngx::light_color0(C(0,1,0,1,0,0x01), white, black, L, eye, nAway, black, out);
      chk("sign-away→black", out, 0,0,0); }

    // 6. SPOT: cosA=(1,0,0)→a=1, distA=(2,0,0)→k=2, attn=0.5; CLAMP facing → 0.5·1·1=0.5.
    { ngx::LightSrc s=mkL(1,1,1); s.cosA[0]=1; s.distA[0]=2; L[0]=s;
      ngx::light_color0(C(0,1,0,2,3,0x01), white, black, L, eye, nUp, black, out);
      chk("spot-attn", out, 0.5f,0.5f,0.5f); }

    // 7. decode_chanctl: cc=0x068e (the SMS reg/lit world material) → REG mat, lit,
    //    REG amb, SIGN diffuse, SPOT attn, lights{0,1}. (b2..5=0011=0x3, b11..14=0.)
    { ngx::ChanCtl c = ngx::decode_chanctl(0x068e);
      if (c.matVtx||!c.enable||c.ambVtx||c.diffFn!=1||c.attnSel!=3||c.mask!=0x03) {
          fails++; if (pos<cap) pos += snprintf(rep+pos, cap-pos,
              "FAIL decode_chanctl(068e) mv=%d en=%d av=%d df=%d at=%d mask=%02x\n",
              c.matVtx,c.enable,c.ambVtx,c.diffFn,c.attnSel,c.mask); } }
    return fails;
}

// ── TEV swap-table unit ──────────────────────────────────────────────────────
// GX TevStageCombiner channel swap: each swap-table id packs four 2-bit selectors
// (r=(id>>6)&3, g=(id>>4)&3, b=(id>>2)&3, a=id&3; 0=R 1=G 2=B 3=A) → a GLSL swizzle.
// Spec-computed truth, hand-decoded. The id 0x57 case is the LIVE Delfino building
// material (rswap=1 → table id 0x57 = ".ggga"), which ngx previously ignored —
// feeding the full-colour raster instead of the green-broadcast term = the building
// colour wash. Identity id 0x1B must stay ".rgba" (no-op) so all-identity materials
// don't regress.
int test_tev_swizzle(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto chk = [&](uint8_t id, const char* want) {
        std::string got = ngx_tev_swizzle(id);
        if (got != want) { fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos,
            "FAIL swizzle id=0x%02x got=\"%s\" want=\"%s\"\n", id, got.c_str(), want); } };
    chk(0x1B, "rgba");   // 0b00_01_10_11 → identity (j3dDefaultTevSwapTableID)
    chk(0x57, "ggga");   // 0b01_01_01_11 → green broadcast, alpha kept (the building case)
    chk(0x00, "rrrr");   // 0b00_00_00_00 → red broadcast
    chk(0xFF, "aaaa");   // 0b11_11_11_11 → alpha broadcast
    chk(0xE4, "abgr");   // 0b11_10_01_00 → reverse (r=A,g=B,b=G,a=R)
    chk(0x2D, "rbag");   // 0b00_10_11_01 → r=R,g=B,b=A,a=G
    return fails;
}

// ── GX indirect-texturing pure helpers ───────────────────────────────────────
// Locks the IndTexMtx float→S2.10-mantissa quantization (GXBump.c GXSetIndTexMtx:
// (int)(1024*offset)&0x7FF, sign-extended 11-bit) and the GXIndTexMtxID decode that
// the shader generator + capture rely on. Hand-computed against GX semantics.
int test_indirect(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* fmt, int a, int b) {
        fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos, fmt, a, b); };

    struct { float in; int want; } mc[] = {
        {0.0f, 0}, {0.25f, 256}, {0.5f, 512}, {0.999f, 1022},   // 1024*0.999=1022.976 → trunc 1022
        {1.0f, -1024},    // 0x400 sets sign bit → wraps to -1024 (S2.10 overflow, as on HW)
        {-0.25f, -256}, {-0.5f, -512}, {-1.0f, -1024},
    };
    for (auto& c : mc) {
        int got = ngx::ngx_ind_mtx_mantissa(c.in);
        if (got != c.want) failf("FAIL mantissa got=%d want=%d\n", got, c.want);
    }

    // matrix-id decode: idx (0-based) + kind, or disabled.
    struct { uint8_t sel; bool on; int idx; int kind; } dc[] = {
        {0,  false, 0, 0},
        {1,  true,  0, ngx::NGX_ITM_INDIRECT}, {3, true, 2, ngx::NGX_ITM_INDIRECT},
        {5,  true,  0, ngx::NGX_ITM_S},        {7, true, 2, ngx::NGX_ITM_S},
        {9,  true,  0, ngx::NGX_ITM_T},        {11, true, 2, ngx::NGX_ITM_T},
        {4,  false, 0, 0}, {8, false, 0, 0}, {12, false, 0, 0},
    };
    for (auto& c : dc) {
        int idx = -9, kind = -9;
        bool on = ngx::ngx_ind_mtx_decode(c.sel, &idx, &kind);
        if (on != c.on) failf("FAIL decode sel got_on=%d want_on=%d\n", on, c.on);
        if (on && (idx != c.idx || kind != c.kind)) failf("FAIL decode idx/kind got=%d want=%d\n",
            idx * 10 + kind, c.idx * 10 + c.kind);
    }

    // format shift + bias.
    const int fs[4] = {0, 3, 4, 5};
    for (int f = 0; f < 4; f++) {
        if (ngx::ngx_ind_fmt_shift((uint8_t)f) != fs[f]) failf("FAIL fmt_shift f=%d got=%d\n", f, ngx::ngx_ind_fmt_shift((uint8_t)f));
        int want_bias = f == 0 ? -128 : 1;
        if (ngx::ngx_ind_bias_add((uint8_t)f) != want_bias) failf("FAIL bias f=%d got=%d\n", f, ngx::ngx_ind_bias_add((uint8_t)f));
    }
    return fails;
}

// ── texture block-padding / no-shear unit ────────────────────────────────────
// GC textures tile at the FORMAT's block dims; sb_tex_decode uses `width` as both the
// tile-iteration bound and the dst row stride, so the caller MUST pad the logical width
// up to the format's block. Padding to a fixed 8 over-strides the 4-wide formats (a
// width ≡4 mod 8 reads one extra tile per row → DIAGONAL SHEAR — the title-logo RGB5A3
// w=460 bug). This test asserts the pad helper AND end-to-end that a 12-wide (≡4 mod 8)
// RGB5A3 image decodes WITHOUT shear (each tile column is uniform across rows).
int test_tex_pad(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* msg) { fails++; if (pos < cap) pos += snprintf(rep + pos, cap - pos, "%s", msg); };

    // 1. block-aligned pad per format — the mapping whose mistake caused the shear.
    struct { int w, fmt, exp; const char* n; } pc[] = {
        {460, SB_TF_RGB5A3, 460, "RGB5A3 460"},   // 4-block: stays 460 (NOT 464)
        {460, SB_TF_IA4,    464, "IA4 460"},       // 8-block: rounds to 464
        {12,  SB_TF_RGBA8,  12,  "RGBA8 12"},      // 4-block
        {12,  SB_TF_I4,     16,  "I4 12"},         // 8-block
        {225, SB_TF_IA4,    232, "IA4 225"},       // the clean ©2002 case
        {460, SB_TF_RGB565, 460, "RGB565 460"},
    };
    for (auto& c : pc) { int got = sb_tex_pad_w(c.w, c.fmt);
        if (got != c.exp) { char b[96]; snprintf(b,sizeof b,"FAIL pad_w %s got=%d exp=%d\n", c.n, got, c.exp); failf(b); } }

    // 2. End-to-end no-shear: a 12×4 RGB5A3 image (logical w=12 ≡4 mod 8, 3 tiles wide).
    // Each 4-wide tile is a SOLID distinct colour. Decoded with the (padded) width the
    // shipping code computes, every column must be constant down its 4 rows and the three
    // tiles must be distinct. A fixed-8 pad (→16) would read a 4th (nonexistent) tile and
    // shift the dst stride → columns no longer constant = shear → this goes red.
    {
        const int W = 12, H = 4;
        // RGB5A3 opaque (top bit set), R5 = 4/8/12 for tiles 0/1/2 → distinct big-endian values.
        auto v_for = [](int tile) -> uint16_t { return (uint16_t)(0x8000 | ((4 + 4*tile) << 10)); };
        uint8_t src[3 * 32];   // 3 tiles × 4×4 texels × 2 bytes
        for (int tile = 0; tile < 3; tile++) {
            uint16_t V = v_for(tile);
            uint8_t hi = (uint8_t)(V >> 8), lo = (uint8_t)(V & 0xFF);  // big-endian store (decode bswaps)
            for (int b = 0; b < 16; b++) { src[tile*32 + b*2] = hi; src[tile*32 + b*2 + 1] = lo; }
        }
        const int pw = sb_tex_pad_w(W, SB_TF_RGB5A3);   // shipping pad (must be 12, not 16)
        uint32_t dst[12 * 4] = {0};
        sb_tex_decode(dst, src, pw, sb_tex_pad_h(H, SB_TF_RGB5A3), SB_TF_RGB5A3, nullptr, 0);
        // Each column constant down rows; the three tile-columns distinct.
        for (int x = 0; x < W; x++)
            for (int y = 1; y < H; y++)
                if (dst[y*W + x] != dst[x]) { char b[80]; snprintf(b,sizeof b,"FAIL shear col %d row %d\n", x, y); failf(b); }
        if (dst[0] == dst[4] || dst[4] == dst[8] || dst[0] == dst[8]) failf("FAIL tiles not distinct (decode collapsed)\n");
    }
    return fails;
}

// ── EFB-copy encode + tiling unit (own-the-framebuffer GXCopyTex writeback) ───────────────────────
// Round-trip the SHIPPING encode+tiling (ngx_efb_copy.h, used by copytex_writeback) through the
// SHIPPING decoder (sb_tex_decode): build an ARGB image, GC-tile-encode it exactly as the override
// does (BE u16 at tile_offset16), decode it, and assert it matches within the format's quantization.
// Catches a tiling-offset slip, a wrong pixel pack, or an endianness flip — the bandaid-prone parts.
int test_efb_copy(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };

    for (int fmt = 4; fmt <= 5; fmt++) {            // RGB565, RGB5A3
        const int W = 8, H = 8;                     // 2×2 tiles (exercises the tiling, not a single tile)
        uint32_t img[W*H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)              // a non-trivial gradient so neighbours differ
                img[y*W+x] = 0xFF000000u | ((uint32_t)(x*32) << 16) | ((uint32_t)(y*32) << 8) | (uint32_t)((x+y)*16);
        // Encode exactly as copytex_writeback: BE u16 at tile_offset16.
        uint8_t enc[W*H*2] = {0};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint16_t v = ngx_efb::argb_to_tex16(img[y*W+x], fmt);
                uint32_t off = ngx_efb::tile_offset16(x, y, W);
                enc[off] = (uint8_t)(v >> 8); enc[off+1] = (uint8_t)(v & 0xFF);   // big-endian
            }
        // Decode with the shipping decoder and compare within quantization (5/6/5 or 5/5/5 truncation).
        uint32_t dec[W*H] = {0};
        sb_tex_decode(dec, enc, W, H, fmt == 5 ? SB_TF_RGB5A3 : SB_TF_RGB565, nullptr, 0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint32_t o = img[y*W+x], d = dec[y*W+x];
                int or_ = (o>>16)&0xFF, og=(o>>8)&0xFF, ob=o&0xFF;
                int dr = d&0xFF, dg=(d>>8)&0xFF, db=(d>>16)&0xFF;   // decoder packs r|g<<8|b<<16
                int gb = fmt == 5 ? 7 : 7, gg = fmt == 5 ? 7 : 3;   // max |error| from bit truncation
                if (abs(dr-or_) > gb || abs(dg-og) > gg || abs(db-ob) > gb) {
                    char b[128]; snprintf(b,sizeof b,"FAIL fmt=%d (%d,%d) enc->dec rgb(%d,%d,%d) vs (%d,%d,%d)\n",
                                          fmt,x,y,dr,dg,db,or_,og,ob); failf(b);
                }
            }
    }
    // Tiling offset spec-check: texel (5,3) in a width-8 image → tile (0,1), iy=3, j=1 →
    // tile_index = (3/4)*(8/4) + (5/4) = 0*2 + 1 = 1; off = 1*32 + 3*8 + 1*2 = 58.
    if (ngx_efb::tile_offset16(5, 3, 8) != 58u) {
        char b[80]; snprintf(b,sizeof b,"FAIL tile_offset16(5,3,8)=%u exp 58\n", ngx_efb::tile_offset16(5,3,8)); failf(b);
    }
    return fails;
}

// ── immediate-mode GXDrawCube geometry unit ──────────────────────────────────
// ngx misses immediate-mode GX draws (no J3D object); the GXDrawCube override now
// captures them from the spec geometry (ngx_imm_geom.h). Assert the 24 GX_QUADS
// corners match GXDraw.c: every corner is (±k,±k,±k) with k=1/sqrt(3), the eight
// distinct cube vertices each appear exactly 3× (shared by 3 faces), and the quad
// triangulation indexes 12 valid tris over 24 verts. A sign slip in a face basis or
// a wrong winding goes red here, not as a misplaced box on Mario.
int test_imm_cube(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };
    auto close = [](float a, float b){ float e=a-b; return (e<0?-e:e) <= 1e-5f; };

    float c[24][3];
    ngx_imm::cube_corners(c);
    const float k = ngx_imm::kCubeK;

    // 1. Every corner coordinate is exactly ±k (a unit cube inscribed in radius-1 sphere).
    for (int i = 0; i < 24; i++)
        for (int a = 0; a < 3; a++)
            if (!close(c[i][a], k) && !close(c[i][a], -k)) {
                char b[96]; snprintf(b,sizeof b,"FAIL corner %d axis %d = %.5f (not ±k)\n", i, a, c[i][a]); failf(b);
            }

    // 2. The eight distinct cube vertices (±k each) each appear exactly 3× across the
    //    6 faces (each cube vertex is shared by 3 faces).
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                int n = 0;
                for (int i = 0; i < 24; i++)
                    if (close(c[i][0], sx*k) && close(c[i][1], sy*k) && close(c[i][2], sz*k)) n++;
                if (n != 3) { char b[96]; snprintf(b,sizeof b,"FAIL vertex (%+d,%+d,%+d) appears %dx (exp 3)\n", sx,sy,sz,n); failf(b); }
            }

    // 3. Triangulation: 36 indices, all < 24, 12 triangles, each within one face's 4-corner span.
    unsigned idx[36]; ngx_imm::cube_tri_indices(idx);
    for (int t = 0; t < 12; t++) {
        unsigned a = idx[t*3], b = idx[t*3+1], d = idx[t*3+2];
        if (a >= 24 || b >= 24 || d >= 24) { failf("FAIL tri index >= 24\n"); break; }
        if (a/4 != b/4 || a/4 != d/4) { char s[80]; snprintf(s,sizeof s,"FAIL tri %d crosses faces\n", t); failf(s); }
    }
    return fails;
}

// The native Mario occlusion query (ngx_imm::cube_front_depth_vk + imm_occluded), the shipping
// functions used by GXPeekARGB (efb_readback_native.cpp). Asserts: (a) the cube front depth is the
// nearest corner in Vulkan convention z'=(clip_z+w)/w, ignoring behind-eye corners; (b) the occluded
// decision — scene nearer than the cube front ⇒ occluded; Mario's own body (between front and back)
// ⇒ NOT occluded; an occluder wall in front ⇒ occluded; far/invalid inputs ⇒ not occluded.
int test_imm_occlusion(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };
    auto close = [](float a, float b){ float e=a-b; return (e<0?-e:e) <= 1e-5f; };

    // (a) front depth = min (clip_z+w)/w over w>0 corners. Build 3 corners: near (d=0.20),
    //     far (d=0.60), and one behind the eye (w<=0, must be ignored).
    float clip[3][4] = {
        {0.f, 0.f, /*z*/ -0.8f, /*w*/ 1.0f},   // (z+w)/w = 0.2
        {0.f, 0.f, /*z*/ -0.4f, /*w*/ 1.0f},   // (z+w)/w = 0.6
        {0.f, 0.f, /*z*/  0.5f, /*w*/ -0.5f},  // behind eye → ignored
    };
    float front = ngx_imm::cube_front_depth_vk(clip, 3);
    if (!close(front, 0.2f)) { char b[80]; snprintf(b,sizeof b,"FAIL front depth %.4f (exp 0.20)\n", front); failf(b); }

    // (b) decisions with cube front=0.20 (near), back≈0.60. eps=0.
    //   Mario body (between front/back, e.g. 0.40) → NOT occluded (his body never self-occludes).
    if (ngx_imm::imm_occluded(0.40f, 0.20f, 0.0f)) failf("FAIL Mario body self-occluded\n");
    //   Wall in front (0.05 < 0.20) → occluded.
    if (!ngx_imm::imm_occluded(0.05f, 0.20f, 0.0f)) failf("FAIL occluder not detected\n");
    //   Scene exactly at the front → not occluded (LEQUAL-ish; strict <).
    if (ngx_imm::imm_occluded(0.20f, 0.20f, 0.0f)) failf("FAIL equal-depth reported occluded\n");
    //   eps tolerance: scene just barely nearer but within eps → not occluded.
    if (ngx_imm::imm_occluded(0.19f, 0.20f, 0.02f)) failf("FAIL eps not applied\n");
    //   Invalid scene depth (no readback) / far cube → not occluded.
    if (ngx_imm::imm_occluded(-1.0f, 0.20f, 0.0f)) failf("FAIL invalid scene depth occluded\n");
    if (ngx_imm::imm_occluded(0.05f, 1e9f,  0.0f)) failf("FAIL far cube occluded\n");
    return fails;
}

// ── immediate-mode GXDrawSphere geometry unit ────────────────────────────────
// The plaza sky has a flat-matColor GXDrawSphere(8,0x10) backdrop dome (Map/Sky.cpp)
// that ngx must capture (no J3D object). Assert the tessellation matches GXDraw.c:46:
// the right vert/tri counts, every vert on the unit sphere, the spec north-pole +
// first-outer-ring positions, and indices that stay within each band's strip span
// (a wrong major/minor step, ring order, or winding slip goes red here).
int test_imm_sphere(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };
    auto close = [](float a, float b){ float e=a-b; return (e<0?-e:e) <= 1e-4f; };
    const int NM = 8, Nm = 16;

    // 1. counts match the spec (numMajor·(numMinor+1)·2 verts, numMajor·2·numMinor tris).
    const int vc = ngx_imm::sphere_vert_count(NM, Nm), tc = ngx_imm::sphere_tri_count(NM, Nm);
    if (vc != 272) { char b[64]; snprintf(b,sizeof b,"FAIL vert count %d (exp 272)\n", vc); failf(b); }
    if (tc != 256) { char b[64]; snprintf(b,sizeof b,"FAIL tri count %d (exp 256)\n", tc); failf(b); }

    static float v[272][3]; ngx_imm::sphere_verts(NM, Nm, v);
    // 2. every vertex on the unit sphere.
    for (int i = 0; i < vc; i++) {
        const float m2 = v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2];
        if (!close(m2, 1.0f)) { char b[80]; snprintf(b,sizeof b,"FAIL vert %d |v|^2=%.5f (not unit)\n", i, m2); failf(b); break; }
    }
    // 3. spec spot-checks: v[0] = outer ring i=0,j=0 = (sin(π/8),0,cos(π/8)); v[1] = inner = north pole.
    if (!close(v[0][0], 0.38268f) || !close(v[0][1], 0.f) || !close(v[0][2], 0.92388f))
        { char b[96]; snprintf(b,sizeof b,"FAIL v0=(%.5f,%.5f,%.5f) exp (0.38268,0,0.92388)\n", v[0][0],v[0][1],v[0][2]); failf(b); }
    if (!close(v[1][0], 0.f) || !close(v[1][1], 0.f) || !close(v[1][2], 1.0f))
        { char b[96]; snprintf(b,sizeof b,"FAIL v1=(%.5f,%.5f,%.5f) exp north pole (0,0,1)\n", v[1][0],v[1][1],v[1][2]); failf(b); }

    // 4. indices: 3·tc, all < vert count, each triangle inside its band's strip span [base,base+S).
    static unsigned idx[256*3]; ngx_imm::sphere_tri_indices(NM, Nm, idx);
    const int S = (Nm + 1) * 2;
    for (int t = 0; t < tc; t++) {
        const unsigned a = idx[t*3], b = idx[t*3+1], c = idx[t*3+2];
        if (a >= (unsigned)vc || b >= (unsigned)vc || c >= (unsigned)vc) { failf("FAIL sphere tri index out of range\n"); break; }
        const unsigned band = a / (unsigned)S;
        if (b/(unsigned)S != band || c/(unsigned)S != band) { char s[80]; snprintf(s,sizeof s,"FAIL sphere tri %d crosses bands\n", t); failf(s); break; }
    }
    return fails;
}

// ── TEV combiner evaluator unit (tev_eval.h) ─────────────────────────────────
// Pins the GX TEV colour-register mapping (the ti=64 plaza-magenta off-by-one bug):
// the combiner's c0/c1/c2 inputs read tev_color[1..3] (TEVREG0/1/2) and `prev` is
// initialised from tev_color[0] (CPREV) — Dolphin PixelShaderGen
// `c0=COLORS[1],c1=COLORS[2],c2=COLORS[3],prev=COLORS[0]`. A mismap turns CPREV into
// c0 (the actual bug) and goes red here. Also checks the regular-combiner integer math.
int test_tev_eval(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };

    // A single-stage combiner builder. dest=prev, clamp on, bias/op/scale 0.
    auto mk = [](int cc_a,int cc_b,int cc_c,int cc_d, int ca_a,int ca_b,int ca_c,int ca_d){
        NgxTevState s{}; s.num_stages = 1;
        s.stage[0].color_env = ((uint32_t)cc_a<<12)|((uint32_t)cc_b<<8)|((uint32_t)cc_c<<4)|(uint32_t)cc_d | (1u<<19);
        s.stage[0].alpha_env = ((uint32_t)ca_a<<13)|((uint32_t)ca_b<<10)|((uint32_t)ca_c<<7)|((uint32_t)ca_d<<4) | (1u<<19);
        s.stage[0].texmap = 0xff; s.stage[0].texcoord = 0xff; s.stage[0].color_chan = 0xff;
        for (auto& t : s.swap_table) t = 0x1B;   // identity swaps
        // CPREV / C0 / C1 / C2 distinct so a wrong index is unambiguous.
        const int16_t regs[4][4] = {{10,20,30,40},{200,201,202,203},{1,2,3,4},{5,6,7,8}};
        for (int c=0;c<4;c++) for (int k=0;k<4;k++) s.tev_color[c][k] = regs[c][k];
        return s;
    };
    struct NoTex : tev_eval::TexelSource { bool sample(int,int,int,int o[4]) const override { o[0]=o[1]=o[2]=o[3]=255; return true; } } notex;
    const int white[4] = {255,255,255,255};
    auto run = [&](const NgxTevState& s, int out[4]){ tev_eval::eval(s, white, white, notex, out, nullptr); };

    // 1. combiner c0 input (cc index 2 / ca index 1) must read TEVREG0 = tev_color[1] = (200,201,202,a203),
    //    NOT CPREV = tev_color[0]. out = d-term with d=c0. a=b=c=ZERO.
    { NgxTevState s = mk(15,15,15,2, 7,7,7,1); int o[4]; run(s, o);
      if (o[0]!=200||o[1]!=201||o[2]!=202||o[3]!=203) { char b[96];
        snprintf(b,sizeof b,"FAIL c0 maps wrong: got(%d,%d,%d,a%d) exp(200,201,202,a203) [CPREV leak=%s]\n",
                 o[0],o[1],o[2],o[3], (o[0]==10)?"YES":"no"); failf(b); } }
    // 2. combiner c1 = tev_color[2] = (1,2,3,a4); c2 = tev_color[3] = (5,6,7,a8).
    { NgxTevState s = mk(15,15,15,4, 7,7,7,2); int o[4]; run(s, o);
      if (o[0]!=1||o[1]!=2||o[2]!=3||o[3]!=4) { char b[80]; snprintf(b,sizeof b,"FAIL c1 got(%d,%d,%d,a%d) exp(1,2,3,a4)\n",o[0],o[1],o[2],o[3]); failf(b); } }
    { NgxTevState s = mk(15,15,15,6, 7,7,7,3); int o[4]; run(s, o);
      if (o[0]!=5||o[1]!=6||o[2]!=7||o[3]!=8) { char b[80]; snprintf(b,sizeof b,"FAIL c2 got(%d,%d,%d,a%d) exp(5,6,7,a8)\n",o[0],o[1],o[2],o[3]); failf(b); } }
    // 3. prev register initialised from CPREV = tev_color[0] = (10,20,30,a40): stage 0 with d=prev passes it through.
    { NgxTevState s = mk(15,15,15,0, 7,7,7,0); int o[4]; run(s, o);
      if (o[0]!=10||o[1]!=20||o[2]!=30||o[3]!=40) { char b[96];
        snprintf(b,sizeof b,"FAIL prev-init not CPREV: got(%d,%d,%d,a%d) exp(10,20,30,a40)\n",o[0],o[1],o[2],o[3]); failf(b); } }
    // 4. regular-combiner integer math, d=ZERO a=ZERO b=ONE(255) c=HALF(128):
    //    spec = d + ((a<<8 + (b-a)*(c+(c>>7)) + 128) >> 8) = (255*129 + 128) >> 8 = 33023>>8 = 128.
    { NgxTevState s = mk(15,12,13,15, 7,7,7,7); int o[4]; run(s, o);   // a=ZERO,b=ONE,c=HALF,d=ZERO
      if (o[0]!=128) { char b[80]; snprintf(b,sizeof b,"FAIL regular math lerp(0,255,128)=%d exp 128\n",o[0]); failf(b); } }
    return fails;
}

// ── JPA screen-aligned billboard corner math (ngx_jpa_billboard.h) ───────────────
// The N7 particle port builds each particle's camera-facing quad with this; pin the offset/corner
// derivation (JPADrawExecBillBoard) against hand-computed values so a sign/order slip is caught.
int test_jpa_billboard(char* rep, int cap) {
    int pos = 0, fails = 0;
    auto failf = [&](const char* m){ fails++; if (pos < cap) pos += snprintf(rep+pos, cap-pos, "%s", m); };
    auto close = [](float a, float b){ float e=a-b; return (e<0?-e:e) <= 1e-4f; };

    // Centred pivot (uc=0): x0=x1=sx·u4x, y0=y1=sy·u4y. sx=2,sy=3,u4=(10,20),pt=(100,200,-700).
    // half-x = 2·10 = 20, half-y = 3·20 = 60. corners {(-20,60),(20,60),(20,-60),(-20,-60)}+pt.
    float c[4][3];
    ngx_jpa::billboard_corners(2.f, 3.f, 10.f, 20.f, 0.f, 0.f, 100.f, 200.f, -700.f, c);
    const float exp0[4][3] = {{80,260,-700},{120,260,-700},{120,140,-700},{80,140,-700}};
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++)
        if (!close(c[i][k], exp0[i][k])) { char b[96];
            snprintf(b,sizeof b,"FAIL centred corner %d[%d]=%.3f exp %.3f\n", i,k,c[i][k],exp0[i][k]); failf(b); }

    // Off-centre pivot (uc=(2,4)): x1=sx·(u4x−ucx)=1·(10−2)=8, x0=1·(10+2)=12,
    // y0=1·(20+4)=24, y1=1·(20−4)=16. corners {(-12,24),(8,24),(8,-16),(-12,-16)}+pt(0,0,-1).
    float d[4][3];
    ngx_jpa::billboard_corners(1.f, 1.f, 10.f, 20.f, 2.f, 4.f, 0.f, 0.f, -1.f, d);
    const float exp1[4][3] = {{-12,24,-1},{8,24,-1},{8,-16,-1},{-12,-16,-1}};
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++)
        if (!close(d[i][k], exp1[i][k])) { char b[96];
            snprintf(b,sizeof b,"FAIL pivot corner %d[%d]=%.3f exp %.3f\n", i,k,d[i][k],exp1[i][k]); failf(b); }

    // All four corners share the particle's eye Z (screen-aligned, constant depth).
    for (int i = 0; i < 4; i++) if (!close(c[i][2], -700.f)) { failf("FAIL billboard corner Z not constant\n"); break; }

    // TEV combiner encoding (jpa_color_env / jpa_alpha_env vs the shader's decode_cc/decode_ac bit
    // layout). Pin the MODULATE case (type 1: a=ZERO15,b=C0(2),c=TEXC(8),d=ZERO15) → out = C0·TEXC.
    {
        uint32_t ce = ngx_jpa::jpa_color_env(15, 2, 8, 15);
        int a=(ce>>12)&0xf, b=(ce>>8)&0xf, c2=(ce>>4)&0xf, d=ce&0xf, clamp=(ce>>19)&1;
        int bias=(ce>>16)&3, op=(ce>>18)&1, scale=(ce>>20)&3, dest=(ce>>22)&3;
        if (a!=15||b!=2||c2!=8||d!=15) failf("FAIL jpa color_env abcd\n");
        if (!clamp||bias!=0||op!=0||scale!=0||dest!=0) failf("FAIL jpa color_env op/bias/clamp/scale/dest\n");
        // alpha: a=ZERO7,b=TEXA4,c=A0(1),d=ZERO7 → out = TEXA·A0; rswap=tswap=0 (identity).
        uint32_t ae = ngx_jpa::jpa_alpha_env();
        int aa=(ae>>13)&7, ab=(ae>>10)&7, ac=(ae>>7)&7, ad=(ae>>4)&7, aclamp=(ae>>19)&1;
        int rswap=ae&3, tswap=(ae>>2)&3;
        if (aa!=7||ab!=4||ac!=1||ad!=7) failf("FAIL jpa alpha_env abcd\n");
        if (!aclamp||rswap!=0||tswap!=0) failf("FAIL jpa alpha_env clamp/swap\n");
    }
    return fails;
}

struct Unit { const char* name; int (*run)(char* rep, int cap); };

const Unit kUnits[] = {
    {"vertex_decode", sb_ngx_vertex_selftest},
    {"projection",    test_projection},
    {"near_clip",     test_clip},
    {"frustum_clip",  test_clip_frustum},
    {"lighting",      test_lighting},
    {"tev_swizzle",   test_tev_swizzle},
    {"tex_pad",       test_tex_pad},
    {"efb_copy",      test_efb_copy},
    {"imm_cube",      test_imm_cube},
    {"imm_occlusion", test_imm_occlusion},
    {"imm_sphere",    test_imm_sphere},
    {"indirect",      test_indirect},
    {"tev_eval",      test_tev_eval},
    {"jpa_billboard", test_jpa_billboard},
};

}  // namespace

int main() {
    int total_fail = 0;
    char rep[8192];
    for (const Unit& u : kUnits) {
        rep[0] = '\0';
        int fails = u.run(rep, (int)sizeof rep);
        printf("[%s] %s\n", fails == 0 ? "PASS" : "FAIL", u.name);
        if (fails != 0) {
            fputs(rep, stdout);
            total_fail += fails;
        }
    }
    if (total_fail == 0) printf("render_test: all %zu units PASS\n", sizeof(kUnits)/sizeof(kUnits[0]));
    else                 printf("render_test: %d failing case(s)\n", total_fail);
    return total_fail == 0 ? 0 : 1;
}
