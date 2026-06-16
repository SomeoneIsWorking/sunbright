// ngx_light — the GX per-vertex colour-channel lighting, extracted as a PURE unit so
// it can be unit-tested (render_test.cpp test_lighting) against the GX spec and against
// a faithful reimplementation of Dolphin's LightingShaderGen integer pipeline. The
// shipping override (ngx_j3d_shape.cpp light_vertex) calls THIS — it is not a fork.
//
// Model (per GX / Dolphin VideoCommon/LightingShaderGen.cpp GenerateLightShader):
//   mat   = matColor (reg) or vertex colour            (the material colour)
//   lacc  = ambColor (reg) or vertex colour            (the light accumulator; ambient)
//   per enabled light i:
//     attn computed by attnfunc (NONE/Dir=1, SPEC, SPOT)
//     diff computed by diffusefunc (NONE=1, SIGN=dot, CLAMP=max(0,dot))
//     lacc += attn * diff * lightColor
//   out = mat * clamp(lacc, 0..1)
// Inputs/outputs here are FLOAT 0..1 (lightColor 0..1, mat/amb 0..1). This matches GX up
// to the hardware's integer rounding (which is sub-1-LSB and does not affect fidelity at
// the scene scale we care about; the integer form is the test's oracle and they agree).

#ifndef SUNBRIGHT_NGX_LIGHT_H
#define SUNBRIGHT_NGX_LIGHT_H

#include <cmath>

namespace ngx {

struct LightSrc {
    bool  valid = false;
    float color[3] = {0, 0, 0};   // RGB 0..1
    float pos[3]   = {0, 0, 0};    // eye-space position
    float dir[3]   = {0, 0, 0};    // eye-space (half-angle) direction
    float cosA[3]  = {0, 0, 0};    // angle attenuation a0,a1,a2
    float distA[3] = {0, 0, 0};    // distance attenuation k0,k1,k2
};

// GX channel-control (J3DColorChan COLOR0/ALPHA0) decoded fields. cc bit layout:
//   b0 matSource(REG=0/VTX=1)  b1 enableLighting  b2..b5 lightMask[0..3]
//   b6 ambSource  b7..b8 diffuseFn(NONE/SIGN/CLAMP)  b9..b10 attnFn  b11..b14 lightMask[4..7]
// attnFn (b9..b10): GX getAttnFn → 0/2 = NONE, 1 = SPEC, 3 = SPOT.
struct ChanCtl {
    bool matVtx, enable, ambVtx;
    int  diffFn;     // 0 NONE, 1 SIGN, 2 CLAMP
    int  attnSel;    // 0/2 NONE, 1 SPEC, 3 SPOT
    unsigned mask;   // 8-bit light enable mask
};

inline ChanCtl decode_chanctl(unsigned cc) {
    ChanCtl c;
    c.matVtx  = (cc >> 0) & 1;
    c.enable  = (cc >> 1) & 1;
    c.ambVtx  = (cc >> 6) & 1;
    c.diffFn  = (cc >> 7) & 3;
    c.attnSel = (cc >> 9) & 3;
    c.mask    = ((cc >> 2) & 0x0F) | (((cc >> 11) & 0x0F) << 4);
    return c;
}

// Compute COLOR0 lit RGB. eye = eye-space vertex position, en = eye-space UNIT normal,
// matColor/ambColor/vcol0 are 0..1. Returns rgb in out[0..2]. (Alpha is handled by the
// caller; this is the colour-channel diffuse path that determines surface brightness.)
inline void light_color0(const ChanCtl& C, const float matColor[3], const float ambColor[3],
                         const LightSrc lights[8], const float eye[3], const float en[3],
                         const float vcol0[3], float out[3]) {
    float mat[3];
    if (C.matVtx) { mat[0]=vcol0[0]; mat[1]=vcol0[1]; mat[2]=vcol0[2]; }
    else          { mat[0]=matColor[0]; mat[1]=matColor[1]; mat[2]=matColor[2]; }

    if (!C.enable) { out[0]=mat[0]; out[1]=mat[1]; out[2]=mat[2]; return; }

    float illum[3];
    if (C.ambVtx) { illum[0]=vcol0[0]; illum[1]=vcol0[1]; illum[2]=vcol0[2]; }
    else          { illum[0]=ambColor[0]; illum[1]=ambColor[1]; illum[2]=ambColor[2]; }

    for (int i = 0; i < 8; i++) {
        if (!(C.mask & (1u << i)) || !lights[i].valid) continue;
        const LightSrc& L = lights[i];
        float ld[3] = { L.pos[0]-eye[0], L.pos[1]-eye[1], L.pos[2]-eye[2] };
        const float dist2 = ld[0]*ld[0] + ld[1]*ld[1] + ld[2]*ld[2];
        const float dist  = std::sqrt(dist2);
        if (dist > 1e-6f) { ld[0]/=dist; ld[1]/=dist; ld[2]/=dist; }

        float attn = 1.f;
        if (C.attnSel == 1) {                 // GX_AF_SPEC
            const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
            const float ang = ndl >= 0.f ? std::fmax(0.f, en[0]*L.dir[0]+en[1]*L.dir[1]+en[2]*L.dir[2]) : 0.f;
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*ang + L.cosA[2]*ang*ang);
            const float k = L.distA[0] + L.distA[1]*ang + L.distA[2]*ang*ang;
            attn = (k > 1e-6f) ? a / k : 0.f;
        } else if (C.attnSel == 3) {          // GX_AF_SPOT
            const float c = std::fmax(0.f, ld[0]*L.dir[0]+ld[1]*L.dir[1]+ld[2]*L.dir[2]);
            const float a = std::fmax(0.f, L.cosA[0] + L.cosA[1]*c + L.cosA[2]*c*c);
            const float k = L.distA[0] + L.distA[1]*dist + L.distA[2]*dist2;
            attn = (k > 1e-6f) ? a / k : 0.f;
        }

        float diff = 1.f;
        const float ndl = en[0]*ld[0] + en[1]*ld[1] + en[2]*ld[2];
        if (C.diffFn == 1) diff = ndl;                       // SIGN
        else if (C.diffFn == 2) diff = std::fmax(0.f, ndl);  // CLAMP
        const float s = attn * diff;
        illum[0] += s*L.color[0]; illum[1] += s*L.color[1]; illum[2] += s*L.color[2];
    }
    for (int k = 0; k < 3; k++) {
        float v = illum[k]; v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        out[k] = mat[k] * v;
    }
}

}  // namespace ngx

#endif
