// sms_boot_lighting.h — per-vertex GX colour-channel lighting for the owned native scene
// capture, as a PURE (Dolphin-free, GPU-free) unit so it is unit-tested (boot_lighting_test).
//
// The shipping capture (sms_boot_j3d_capture.cpp) calls THIS to turn a vertex's model-space
// position+normal + the material's colour channel + the live hardware lights into a lit COLOR0
// raster RGB, exactly as GX computes it in VIEW space. It is a thin transform wrapper over the
// already-tested ngx::light_color0 (runtime/ngx/ngx_light.h) — it is not a fork of it.
//
// SPACE: GX lighting runs in VIEW/eye space. The shape's draw matrix (mDrawMatrices, loaded via
// GXLoadPosMtxImm) is the MODELVIEW (model→view), and the scene loads its light positions in view
// space (GXLoadLightObjImm) — so transforming the vertex pos/normal by the draw matrix lands them
// in the SAME space as the lights. SMS joint draw matrices are rigid (rotation+translation, no
// scale), so the normal's correct transform (inverse-transpose) equals the modelview upper-3x3.
#pragma once
#include "ngx_light.h"
#include <cmath>

namespace sb::render {

// model-space position -> view/eye space via the 3x4 modelview.
inline void sb_eye_pos(const float posMtx[3][4], const float p[3], float out[3]) {
    for (int k = 0; k < 3; ++k)
        out[k] = posMtx[k][0]*p[0] + posMtx[k][1]*p[1] + posMtx[k][2]*p[2] + posMtx[k][3];
}

// model-space normal -> view/eye space (modelview upper-3x3, normalized). Rigid-transform
// assumption (see header note). Returns a zero vector if the input normal is ~zero (a shape
// with no NRM attribute): the caller treats that as "unlit" so a normal-less shape stays bright.
inline void sb_eye_nrm(const float posMtx[3][4], const float n[3], float out[3]) {
    float e[3];
    for (int k = 0; k < 3; ++k)
        e[k] = posMtx[k][0]*n[0] + posMtx[k][1]*n[1] + posMtx[k][2]*n[2];
    const float len = std::sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
    if (len > 1e-6f) { out[0]=e[0]/len; out[1]=e[1]/len; out[2]=e[2]/len; }
    else             { out[0]=out[1]=out[2]=0.f; }
}

// Compute one vertex's lit COLOR0 raster RGB (0..1). `chanCtrl` is J3DColorChan::mChanCtrl,
// whose 16-bit layout is exactly what ngx::decode_chanctl() consumes. matColor3/ambColor3/vcol0
// are 0..1. If the channel disables lighting, light_color0 returns the material colour unchanged.
// If the vertex has no usable normal (|n|~0) the surface is returned unlit (full material colour)
// rather than ambient-only, so normal-less geometry doesn't go dark.
inline void sb_light_vertex_color0(unsigned chanCtrl,
                                   const float matColor3[3], const float ambColor3[3],
                                   const ngx::LightSrc lights[8], const float posMtx[3][4],
                                   const float modelPos[3], const float modelNrm[3],
                                   const float vcol0_3[3], float out3[3]) {
    const ngx::ChanCtl C = ngx::decode_chanctl(chanCtrl);
    float eye[3], en[3];
    sb_eye_pos(posMtx, modelPos, eye);
    sb_eye_nrm(posMtx, modelNrm, en);
    const bool no_normal = (en[0]==0.f && en[1]==0.f && en[2]==0.f);
    if (C.enable && no_normal) {              // lit material, but this vertex has no normal
        const float* m = C.matVtx ? vcol0_3 : matColor3;
        out3[0]=m[0]; out3[1]=m[1]; out3[2]=m[2];
        return;
    }
    ngx::light_color0(C, matColor3, ambColor3, lights, eye, en, vcol0_3, out3);
}

} // namespace sb::render
