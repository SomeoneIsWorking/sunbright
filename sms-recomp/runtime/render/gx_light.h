#pragma once
// gx_light — the GX colour-channel (lighting) evaluation as plain, testable C++.
//
// Same reason as tev_eval.h: this arithmetic decides whether a surface is lit or black, and until
// now it lived inline in scene.cpp with no test and no way to evaluate it outside a frame. A day
// was spent blaming textures for a black surface whose colour CHANNEL was zero.
//
// GX computes, independently for colour and alpha in each channel:
//     output = material * clamp(ambient + SUM enabled lights * attenuation * diffuse, 0, 1)
// with the material and ambient coming either from a register or from the vertex, per the channel
// control. Lights are in VIEW space, which is where the draw matrix has already put the vertex.

#include <cstdint>

#include "native_render.h"

// GXAttnFn, reconstructed from the two bits GXSetChanCtrl writes. It packs the function as
//     bit 9  = (attn_fn != GX_AF_NONE)
//     bit 10 = (attn_fn != GX_AF_SPEC)
// (decomp/sms/src/dolphin/gx/GXLight.c), so neither bit alone is "attenuation on" — the PAIR names
// the function, and reading either in isolation is how a specular channel gets spotlight maths.
enum class SbrAttnFn { None, Spec, Spot };
SbrAttnFn sbr_attn_fn(const SbrChanCtrl& c);

// GXDiffuseFn: 0 NONE (no diffuse term), 1 SIGN (the dot product, UNCLAMPED — it can subtract),
// 2 CLAMP (the dot product clamped at zero).
enum : uint8_t { SBR_DF_NONE = 0, SBR_DF_SIGN = 1, SBR_DF_CLAMP = 2 };

struct SbrLightTrace {
    bool enabled = false;
    float ambient[3]{};
    float material[4]{};
    struct Per {
        int index = -1;
        float dist = 0, cosine = 0, atten = 0, diffuse = 0;
        float direction[3]{};
        float acc[3]{};
    } light[8];
    unsigned lights = 0;
    float out[4]{};
};

// Evaluate colour channel `chan` (0 or 1) for one vertex. `trace` may be null.
void sbr_light_channel(const SbrXfState& xf, int chan, const float vpos[3], const float nrm[3],
                       const float vtxColor[4], float out[4], SbrLightTrace* trace);
