// boot_lighting_test.cpp — the capture-side lighting wiring (sms_boot_lighting.h), the SHIPPING
// path the native J3D capture uses to light per-vertex COLOR0. Verifies the view-space transform
// of position+normal and the J3DColorChan.mChanCtrl -> ngx decode against hand-computed GX values.
//
// Pure: no Vulkan, no ROM, no GPU. Spec-derived expected values.

#include "sms_boot_lighting.h"

#include <cstdio>
#include <cmath>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }

int main() {
    // ---- 1. eye-space transforms ----
    // A pure rotation+translation modelview: +90deg about Y, translate (10,0,0).
    // Rz: x' = z, z' = -x (for +90 about Y in this convention). Use an explicit matrix.
    float mv[3][4] = {
        { 0, 0, 1, 10 },
        { 0, 1, 0,  0 },
        {-1, 0, 0,  0 },
    };
    float p[3] = {1, 2, 3};
    float ep[3]; sb_eye_pos(mv, p, ep);
    chk(near(ep[0], 0*1+0*2+1*3+10) && near(ep[1], 2) && near(ep[2], -1), "eye_pos rigid");

    float n[3] = {1, 0, 0};
    float en[3]; sb_eye_nrm(mv, n, en);
    chk(near(en[0],0) && near(en[1],0) && near(en[2],-1), "eye_nrm rotate+normalize");

    float zero[3] = {0,0,0}, ez[3]; sb_eye_nrm(mv, zero, ez);
    chk(ez[0]==0 && ez[1]==0 && ez[2]==0, "eye_nrm zero normal -> zero");

    // ---- 2. lit COLOR0 vs a directional-ish positional white light ----
    // Identity modelview, vertex at origin, light at (0,0,10) so the light direction at the
    // vertex is +Z. White light, white material, black ambient, CLAMP diffuse, light0 only.
    float ident[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    ngx::LightSrc L[8];
    L[0].valid = true;
    L[0].color[0]=L[0].color[1]=L[0].color[2]=1.f;
    L[0].pos[0]=0; L[0].pos[1]=0; L[0].pos[2]=10;
    // chanCtrl: enable(b1) | mask light0(b2) | diffFn CLAMP=2(b7..8). matSrc/ambSrc=REG, attn NONE.
    const unsigned cc = (1u<<1) | (1u<<2) | (2u<<7);   // = 0x106
    float matWhite[3]={1,1,1}, ambBlack[3]={0,0,0}, vcol[3]={0,0,0}, out[3];

    float nFront[3]={0,0,1};   // faces the light -> full bright
    sb_light_vertex_color0(cc, matWhite, ambBlack, L, ident, /*pos*/zero, nFront, vcol, out);
    chk(near(out[0],1) && near(out[1],1) && near(out[2],1), "front-lit = full bright");

    float n60[3]={0.8660254f,0,0.5f};   // 60deg from light -> n.l = 0.5
    sb_light_vertex_color0(cc, matWhite, ambBlack, L, ident, zero, n60, vcol, out);
    chk(near(out[0],0.5f,2e-3f), "60deg-lit = half bright");

    float nBack[3]={0,0,-1};   // faces away -> clamp(neg)=0 -> ambient(black)
    sb_light_vertex_color0(cc, matWhite, ambBlack, L, ident, zero, nBack, vcol, out);
    chk(near(out[0],0) && near(out[1],0) && near(out[2],0), "back-lit = dark");

    // ---- 3. ambient floor: same back-facing vertex with grey ambient stays at ambient*mat ----
    float ambGrey[3]={0.25f,0.25f,0.25f};
    sb_light_vertex_color0(cc, matWhite, ambGrey, L, ident, zero, nBack, vcol, out);
    chk(near(out[0],0.25f), "back-lit + grey ambient = ambient floor");

    // ---- 4. lighting disabled -> material colour passes through unchanged ----
    float matTint[3]={0.2f,0.4f,0.6f};
    sb_light_vertex_color0(/*cc no enable*/0u, matTint, ambBlack, L, ident, zero, nBack, vcol, out);
    chk(near(out[0],0.2f) && near(out[1],0.4f) && near(out[2],0.6f), "unlit = matColor passthrough");

    // ---- 5. lit material but no normal -> full material colour (not ambient-only) ----
    sb_light_vertex_color0(cc, matTint, ambBlack, L, ident, zero, /*nrm*/zero, vcol, out);
    chk(near(out[0],0.2f) && near(out[1],0.4f) && near(out[2],0.6f), "lit + no normal = matColor");

    std::printf("boot_lighting_test: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
