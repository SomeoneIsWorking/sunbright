#pragma once
#include <cmath>

// ───────────────────────────────────────────────────────────────────────────
// Pure, Dolphin/GX-free port of the SMS stage-light load — TLightCommon::setLight
// (GMSE01 @0x80229a30) / TLightMario::setLight (@0x80229610, byte-identical). This is
// the REAL GX-light source for a stage scene, decompiled from the US DOL (the community
// decomp left TLightCommon::setLight an empty stub — LightUtil.cpp).
//
// setLight(graphics, idx) loads up to THREE GX lights, from ONE Light-Group entry plus
// gpLightManager's "effect" (specular sun) light:
//   • GX_LIGHT0 (id 1, ALWAYS)  — positional: view-transform getLightPosition(idx),
//       colour getLightColor(idx), flat attn.                  GXLoadLightObjImm(.,1)
//   • GX_LIGHT1 (id 2, IFF gpLightManager->unk54 && unk55) — the effect light:
//       view-transform the manager's effect position, colour = manager colour scaled.
//                                                              GXLoadLightObjImm(.,2)
//   • GX_LIGHT2 (id 4, ALWAYS)  — directional: dir = -normalize(getLightPosition(idx)),
//       same colour, specular attn.                            GXLoadLightObjImm(.,4)
// (getLightPosition/getLightColor read Light-Group[idx + unk24]; the option scene uses
//  the white sun at Light-Group[0].)
//
// VALUE-VERIFIED (GX-command-stream oracle, 2026-06-30): stage-15 file-select loads
// exactly 3 white lights into slots 0/1/2 — proving the effect light is ON there. The
// old scene_drive workaround blindly loaded 8 Light-Group entries into GX_LIGHT0..7 (a
// pre-oracle guess); this is the faithful selection. See memory
// [[fileselect-lighting-3-vs-8-divergence]].
//
// This header computes each light's POSITION / COLOUR / SPECULAR-flag the way setLight
// does; the caller feeds them into GXInitLight*/GXInitSpecularDir + GXLoadLightObjImm at
// the correct slot. Attenuation uses the GC GXInitLightObj defaults (cos a=1,0,0;
// dist k=1,0,0) — the SDA constants the decomp reads (c17a8=1.0, c17a4=0.0); they tune
// per-vertex intensity, not the light COUNT/COLOUR the value oracle compares.
// ───────────────────────────────────────────────────────────────────────────

namespace sb {

struct SetLightIn {
    float lgColor[4];   // Light-Group[idx+unk24] colour, 0..1 (getLightColor)
    float lgPos[3];     // Light-Group[idx+unk24] world position (getLightPosition)
    float view[12];     // 3x4 view matrix, row-major (graphics->mViewMtx; PSMTXMultVec)
    bool  effectOn;     // gpLightManager unk54 && unk55 — effect/specular-light gate
    float effPos[3];    // effect-light world pos (gpLightManager unk1c..; = Light-Group[0])
    float effColor[4];  // effect colour scaled (manager unk18 * unk28), 0..1
};

// One output light, indexed by GX slot: out[0]=GX_LIGHT0, out[1]=GX_LIGHT1 (effect),
// out[2]=GX_LIGHT2. `present` mirrors which GXLoadLightObjImm calls setLight makes.
struct OutLight {
    bool  present;
    float pos[3];       // GXInitLightPos arg, or (specular) GXInitSpecularDir direction
    float color[4];     // GXInitLightColor (0..1)
    bool  specular;     // GX_LIGHT2 directional: load via GXInitSpecularDir(pos)
};

// PSMTXMultVec: out = M * (in, 1), M row-major 3x4.
inline void setlight_mtx_mul_vec(const float m[12], const float in[3], float out[3]) {
    out[0] = m[0]*in[0] + m[1]*in[1] + m[2]*in[2]  + m[3];
    out[1] = m[4]*in[0] + m[5]*in[1] + m[6]*in[2]  + m[7];
    out[2] = m[8]*in[0] + m[9]*in[1] + m[10]*in[2] + m[11];
}

// PSVECNormalize (degenerate -> zero), matching FUN_8034a5d0 usage in setLight.
inline void setlight_normalize3(const float in[3], float out[3]) {
    float len = std::sqrt(in[0]*in[0] + in[1]*in[1] + in[2]*in[2]);
    if (len > 0.f) { out[0]=in[0]/len; out[1]=in[1]/len; out[2]=in[2]/len; }
    else           { out[0]=out[1]=out[2]=0.f; }
}

// Fill out[0..2] (GX_LIGHT0/1/2) and return the number of present lights (2 or 3),
// faithful to setLight's GXLoadLightObjImm(.,1)/(.,2)/(.,4) sequence.
inline int build_stage_lights(const SetLightIn& in, OutLight out[3]) {
    // GX_LIGHT0 — positional sun.
    out[0].present  = true;
    out[0].specular = false;
    setlight_mtx_mul_vec(in.view, in.lgPos, out[0].pos);
    for (int i = 0; i < 4; ++i) out[0].color[i] = in.lgColor[i];

    // GX_LIGHT1 — effect/specular light (only if gpLightManager has it enabled).
    out[1].present  = in.effectOn;
    out[1].specular = false;
    if (in.effectOn) {
        setlight_mtx_mul_vec(in.view, in.effPos, out[1].pos);
        for (int i = 0; i < 4; ++i) out[1].color[i] = in.effColor[i];
    } else {
        out[1].pos[0] = out[1].pos[1] = out[1].pos[2] = 0.f;
        for (int i = 0; i < 4; ++i) out[1].color[i] = 0.f;
    }

    // GX_LIGHT2 — directional: dir = -normalize(world light pos).
    out[2].present  = true;
    out[2].specular = true;
    float nrm[3];
    setlight_normalize3(in.lgPos, nrm);
    out[2].pos[0] = -nrm[0]; out[2].pos[1] = -nrm[1]; out[2].pos[2] = -nrm[2];
    for (int i = 0; i < 4; ++i) out[2].color[i] = in.lgColor[i];

    return (out[0].present ? 1 : 0) + (out[1].present ? 1 : 0) + (out[2].present ? 1 : 0);
}

} // namespace sb
