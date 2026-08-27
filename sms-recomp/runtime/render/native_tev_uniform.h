#pragma once

#include <cstdint>

struct SbrTevState;

// Shader-facing TEV uniform block. std140 gives every member a 16-byte vector slot, so selector
// values remain one per component instead of being packed into GX register words.
struct SbrNativeTevUniform {
    int32_t cSel[16][4];
    int32_t cOp[16][4];
    int32_t aSel[16][4];
    int32_t aOp[16][4];
    int32_t dest[16][4];
    float konst[16][4];
    float regInit[4][4];
    int32_t swapTable[4][4];
    int32_t control[4];
    float alphaRef[4];
};

// Translate persistent GX TEV state into the exact layout consumed by geom.frag.glsl.
void sbr_native_pack_tev_uniform(const SbrTevState& tev, SbrNativeTevUniform& uniform);
