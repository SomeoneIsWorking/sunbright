// gx_light — see gx_light.h.

#include "gx_light.h"

#include <algorithm>
#include <cmath>

namespace {

float dot3(const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

} // namespace

SbrAttnFn sbr_attn_fn(const SbrChanCtrl& c) {
    // attnEnable holds bit 9, attnSpot holds bit 10 — the names are historical; what they mean is
    // fixed by GXSetChanCtrl's encoding above.
    if (!c.attnEnable) return SbrAttnFn::None;      // bit 9 clear  -> attn_fn == GX_AF_NONE
    if (!c.attnSpot)   return SbrAttnFn::Spec;      // bit 10 clear -> attn_fn == GX_AF_SPEC
    return SbrAttnFn::Spot;
}

void sbr_light_channel(const SbrXfState& xf, int chan, const float vpos[3], const float nrm[3],
                       const float vtxColor[4], float out[4], SbrLightTrace* trace) {
    const SbrChanCtrl& c = xf.chan[chan & 1];
    const float* mat = c.matSrcVertex ? vtxColor : xf.material[chan & 1];

    if (trace != nullptr) {
        trace->enabled = c.enableLight != 0;
        for (int i = 0; i < 4; ++i) trace->material[i] = mat[i];
    }

    if (!c.enableLight) {
        for (int i = 0; i < 4; ++i) out[i] = mat[i];
        if (trace != nullptr) for (int i = 0; i < 4; ++i) trace->out[i] = out[i];
        return;
    }

    const float* amb = c.ambSrcVertex ? vtxColor : xf.ambient[chan & 1];
    float acc[3] = {amb[0], amb[1], amb[2]};
    if (trace != nullptr) for (int i = 0; i < 3; ++i) trace->ambient[i] = amb[i];

    const SbrAttnFn fn = sbr_attn_fn(c);
    unsigned traced = 0;

    for (int li = 0; li < 8; ++li) {
        if (!((c.lightMask >> li) & 1)) continue;
        const SbrLight& L = xf.light[li];

        float ldir[3] = {L.pos[0] - vpos[0], L.pos[1] - vpos[1], L.pos[2] - vpos[2]};
        const float d2 = dot3(ldir, ldir);
        const float d = std::sqrt(d2);
        if (d > 1e-6f) { ldir[0] /= d; ldir[1] /= d; ldir[2] /= d; }

        float atten = 1.0f;
        float cosine = 0.0f;
        if (fn == SbrAttnFn::Spot) {
            // The angular polynomial in cos(angle to the light's own direction), over the distance
            // polynomial. Both are quadratics in their own variable. A light whose distance
            // coefficients are all zero is a dead slot — its ratio is 0/0 — and must contribute
            // nothing rather than a NaN that poisons the whole channel.
            cosine = std::max(0.0f, dot3(ldir, L.dir));
            const float cosAttn = L.cosAtt[0] + L.cosAtt[1] * cosine + L.cosAtt[2] * cosine * cosine;
            const float distAttn = L.distAtt[0] + L.distAtt[1] * d + L.distAtt[2] * d2;
            atten = (distAttn > 0.0f) ? std::max(0.0f, cosAttn / distAttn) : 0.0f;
        } else if (fn == SbrAttnFn::Spec) {
            // SPECULAR. The light's "position" register holds a DIRECTION here and the attenuation
            // is driven by the surface normal, not by distance — which is why running the spotlight
            // formula for a specular channel is not a small error but a different function.
            const float nl = dot3(nrm, ldir);
            float a = (nl >= 0.0f) ? std::max(0.0f, dot3(nrm, L.dir)) : 0.0f;
            const float cosAttn = L.cosAtt[0] + L.cosAtt[1] * a + L.cosAtt[2] * a * a;
            float dv[3] = {L.distAtt[0], L.distAtt[1], L.distAtt[2]};
            if (c.diffuseFn != SBR_DF_NONE) {
                const float len = std::sqrt(dot3(dv, dv));
                if (len > 1e-6f) { dv[0] /= len; dv[1] /= len; dv[2] /= len; }
            }
            const float distAttn = std::max(0.0f, dv[0] + dv[1] * a + dv[2] * a * a);
            atten = (distAttn > 0.0f) ? std::max(0.0f, cosAttn / distAttn) : 0.0f;
        }

        // GX_DF_SIGN is UNCLAMPED: a light behind the surface SUBTRACTS. That is correct hardware
        // behaviour and is how a legitimately back-facing surface reaches black.
        float diffuse = 1.0f;
        if (c.diffuseFn != SBR_DF_NONE) {
            diffuse = dot3(nrm, ldir);
            if (c.diffuseFn == SBR_DF_CLAMP) diffuse = std::max(0.0f, diffuse);
        }

        const float k = atten * diffuse;
        acc[0] += k * L.color[0];
        acc[1] += k * L.color[1];
        acc[2] += k * L.color[2];

        if (trace != nullptr && traced < 8) {
            SbrLightTrace::Per& p = trace->light[traced++];
            p.index = li; p.dist = d; p.cosine = cosine; p.atten = atten; p.diffuse = diffuse;
            for (int i = 0; i < 3; ++i) p.acc[i] = acc[i];
        }
    }

    for (int i = 0; i < 3; ++i) out[i] = std::clamp(mat[i] * acc[i], 0.0f, 1.0f);
    out[3] = mat[3];

    if (trace != nullptr) {
        trace->lights = traced;
        for (int i = 0; i < 4; ++i) trace->out[i] = out[i];
    }
}
