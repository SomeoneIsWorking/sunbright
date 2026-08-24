// gx_light — see gx_light.h.

#include "gx_light.h"

#include <algorithm>
#include <cmath>

namespace {

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void evaluate_lighting(const SbrXfState& xf, const SbrChanCtrl& control, const float ambient[4],
                       const float vpos[3], const float nrm[3], float accumulator[4],
                       SbrLightTrace* trace) {
    for (int component = 0; component < 4; ++component)
        accumulator[component] = ambient[component];
    if (trace != nullptr)
        for (int component = 0; component < 3; ++component)
            trace->ambient[component] = ambient[component];

    const SbrAttnFn fn = sbr_attn_fn(control);
    unsigned traced = 0;

    for (int lightIndex = 0; lightIndex < 8; ++lightIndex) {
        if (!((control.lightMask >> lightIndex) & 1))
            continue;
        const SbrLight& light = xf.light[lightIndex];

        float direction[3] = {light.pos[0] - vpos[0], light.pos[1] - vpos[1],
                              light.pos[2] - vpos[2]};
        const float distanceSquared = dot3(direction, direction);
        const float distance = std::sqrt(distanceSquared);
        if (distance > 1e-6f) {
            direction[0] /= distance;
            direction[1] /= distance;
            direction[2] /= distance;
        }

        float attenuation = 1.0f;
        float cosine = 0.0f;
        if (fn == SbrAttnFn::Spot) {
            cosine = std::max(0.0f, dot3(direction, light.dir));
            const float angular =
                light.cosAtt[0] + light.cosAtt[1] * cosine + light.cosAtt[2] * cosine * cosine;
            const float radial =
                light.distAtt[0] + light.distAtt[1] * distance + light.distAtt[2] * distanceSquared;
            attenuation = radial > 0.0f ? std::max(0.0f, angular / radial) : 0.0f;
        } else if (fn == SbrAttnFn::Spec) {
            const float normalLight = dot3(nrm, direction);
            const float normalDirection =
                normalLight >= 0.0f ? std::max(0.0f, dot3(nrm, light.dir)) : 0.0f;
            cosine = normalDirection;
            const float angular = light.cosAtt[0] + light.cosAtt[1] * normalDirection +
                                  light.cosAtt[2] * normalDirection * normalDirection;
            float radialCoefficients[3] = {light.distAtt[0], light.distAtt[1], light.distAtt[2]};
            if (control.diffuseFn != SBR_DF_NONE) {
                const float length = std::sqrt(dot3(radialCoefficients, radialCoefficients));
                if (length > 1e-6f) {
                    radialCoefficients[0] /= length;
                    radialCoefficients[1] /= length;
                    radialCoefficients[2] /= length;
                }
            }
            const float radial =
                std::max(0.0f, radialCoefficients[0] + radialCoefficients[1] * normalDirection +
                                   radialCoefficients[2] * normalDirection * normalDirection);
            attenuation = radial > 0.0f ? std::max(0.0f, angular / radial) : 0.0f;
        }

        // GX_DF_SIGN is deliberately unclamped: a back-facing light subtracts.
        float diffuse = 1.0f;
        if (control.diffuseFn != SBR_DF_NONE) {
            diffuse = dot3(nrm, direction);
            if (control.diffuseFn == SBR_DF_CLAMP)
                diffuse = std::max(0.0f, diffuse);
        }

        const float contribution = attenuation * diffuse;
        for (int component = 0; component < 4; ++component)
            accumulator[component] += contribution * light.color[component];

        if (trace != nullptr && traced < 8) {
            SbrLightTrace::Per& point = trace->light[traced++];
            point.index = lightIndex;
            point.dist = distance;
            point.cosine = cosine;
            point.atten = attenuation;
            point.diffuse = diffuse;
            for (int component = 0; component < 3; ++component) {
                point.direction[component] = direction[component];
                point.acc[component] = accumulator[component];
            }
        }
    }

    if (trace != nullptr)
        trace->lights = traced;
}

} // namespace

SbrAttnFn sbr_attn_fn(const SbrChanCtrl& c) {
    // attnEnable holds bit 9, attnSpot holds bit 10 — the names are historical; what they mean is
    // fixed by GXSetChanCtrl's encoding above.
    if (!c.attnEnable)
        return SbrAttnFn::None; // bit 9 clear  -> attn_fn == GX_AF_NONE
    if (!c.attnSpot)
        return SbrAttnFn::Spec; // bit 10 clear -> attn_fn == GX_AF_SPEC
    return SbrAttnFn::Spot;
}

void sbr_light_channel(const SbrXfState& xf, int chan, const float vpos[3], const float nrm[3],
                       const float vtxColor[4], float out[4], SbrLightTrace* trace) {
    const int channel = chan & 1;
    const SbrChanCtrl& colorControl = xf.chan[channel];
    const SbrChanCtrl& alphaControl = xf.chan[channel + 2];
    const float* colorMaterial = colorControl.matSrcVertex ? vtxColor : xf.material[channel];
    const float* alphaMaterial = alphaControl.matSrcVertex ? vtxColor : xf.material[channel];

    if (trace != nullptr) {
        trace->enabled = colorControl.enableLight != 0;
        for (int component = 0; component < 3; ++component)
            trace->material[component] = colorMaterial[component];
        trace->material[3] = alphaMaterial[3];
    }

    if (colorControl.enableLight) {
        const float* ambient = colorControl.ambSrcVertex ? vtxColor : xf.ambient[channel];
        float accumulator[4];
        evaluate_lighting(xf, colorControl, ambient, vpos, nrm, accumulator, trace);
        // GX clamps the lighting accumulator before multiplying by material. Clamping the product
        // instead over-brightens a dim material whenever ambient plus lights exceeds one.
        for (int component = 0; component < 3; ++component)
            out[component] =
                colorMaterial[component] * std::clamp(accumulator[component], 0.0f, 1.0f);
    } else {
        for (int component = 0; component < 3; ++component)
            out[component] = colorMaterial[component];
    }

    if (alphaControl.enableLight) {
        const float* ambient = alphaControl.ambSrcVertex ? vtxColor : xf.ambient[channel];
        float accumulator[4];
        evaluate_lighting(xf, alphaControl, ambient, vpos, nrm, accumulator, nullptr);
        out[3] = alphaMaterial[3] * std::clamp(accumulator[3], 0.0f, 1.0f);
    } else {
        out[3] = alphaMaterial[3];
    }

    if (trace != nullptr) {
        for (int component = 0; component < 4; ++component)
            trace->out[component] = out[component];
    }
}
