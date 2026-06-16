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

#include "ngx_project.h"
#include "ngx_clip.h"
#include "ngx_light.h"

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

struct Unit { const char* name; int (*run)(char* rep, int cap); };

const Unit kUnits[] = {
    {"vertex_decode", sb_ngx_vertex_selftest},
    {"projection",    test_projection},
    {"near_clip",     test_clip},
    {"lighting",      test_lighting},
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
