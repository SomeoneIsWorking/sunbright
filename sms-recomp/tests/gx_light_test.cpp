// gx_light_test — the colour-channel evaluation checked against the SDK's own encoding and the
// GX lighting equation, not against another run of the same code.

#include "../runtime/render/gx_light.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_near(float got, float want, const char* what, float eps = 1.0f / 512.0f) {
    if (std::fabs(got - want) > eps) {
        std::printf("FAIL: %s — got %.5f want %.5f\n", what, got, want);
        ++g_failures;
    }
}

SbrXfState base() {
    SbrXfState xf{};
    xf.numChans = 1;
    for (int ch = 0; ch < 2; ++ch) {
        xf.chan[ch].matSrcVertex = 0;
        xf.chan[ch].ambSrcVertex = 0;
        for (int i = 0; i < 4; ++i)
            xf.material[ch][i] = 1.0f;
        for (int i = 0; i < 3; ++i)
            xf.ambient[ch][i] = 0.0f;
    }
    for (auto& L : xf.light) {
        L.color[0] = L.color[1] = L.color[2] = L.color[3] = 1.0f;
        L.cosAtt[0] = 1.0f;
        L.cosAtt[1] = L.cosAtt[2] = 0.0f;
        L.distAtt[0] = 1.0f;
        L.distAtt[1] = L.distAtt[2] = 0.0f;
    }
    return xf;
}

// ---------------------------------------------------------------------------------------------
// THE ATTENUATION-FUNCTION DECODE. GXSetChanCtrl (decomp/sms/src/dolphin/gx/GXLight.c) writes
//     bit 9  = (attn_fn != GX_AF_NONE)     [GX_AF_NONE == 2]
//     bit 10 = (attn_fn != GX_AF_SPEC)     [GX_AF_SPEC == 0]
// so the three legal bit pairs are the three functions, and (0,0) cannot be produced. This is the
// pair aurora once had SWAPPED, which turned a specular channel into "no attenuation" and
// over-brightened Mario — so it gets asserted directly rather than assumed.
void test_attn_fn_decode() {
    SbrChanCtrl c{};
    c.attnEnable = 0;
    c.attnSpot = 0;
    check(sbr_attn_fn(c) == SbrAttnFn::None, "attnFn: bit9=0 -> GX_AF_NONE");
    c.attnEnable = 0;
    c.attnSpot = 1;
    check(sbr_attn_fn(c) == SbrAttnFn::None, "attnFn: bit9=0 -> NONE regardless of bit10");
    c.attnEnable = 1;
    c.attnSpot = 0;
    check(sbr_attn_fn(c) == SbrAttnFn::Spec, "attnFn: bit9=1 bit10=0 -> GX_AF_SPEC");
    c.attnEnable = 1;
    c.attnSpot = 1;
    check(sbr_attn_fn(c) == SbrAttnFn::Spot, "attnFn: bit9=1 bit10=1 -> GX_AF_SPOT");
}

// Lighting disabled: the channel IS the material colour, with no ambient and no lights.
void test_lighting_disabled() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 0;
    xf.material[0][0] = 0.25f;
    xf.material[0][1] = 0.5f;
    xf.material[0][2] = 0.75f;
    xf.ambient[0][0] = 1.0f; // must be ignored
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.25f, "lighting off: red is the material register");
    check_near(out[1], 0.50f, "lighting off: green is the material register");
    check_near(out[2], 0.75f, "lighting off: ambient is not added when lighting is off");
}

// The material and ambient sources are independent, and either can come from the vertex.
void test_colour_sources() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 0; // ambient only
    xf.chan[0].matSrcVertex = 1;
    xf.chan[0].ambSrcVertex = 0;
    xf.ambient[0][0] = xf.ambient[0][1] = xf.ambient[0][2] = 0.5f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1};
    const float vc[4] = {0.5f, 1.0f, 1.0f, 1.0f};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.25f, "matSrc=vertex: 0.5 (vertex) * 0.5 (ambient register)");

    xf.chan[0].matSrcVertex = 0;
    xf.chan[0].ambSrcVertex = 1;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.5f, "ambSrc=vertex: 1.0 (material register) * 0.5 (vertex)");
}

// ---------------------------------------------------------------------------------------------
// The three diffuse functions, against a light directly BEHIND the surface — the case that made
// the plaza black, and the one where SIGN and CLAMP differ.
void test_diffuse_functions() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 1;
    xf.chan[0].attnEnable = 0; // GX_AF_NONE: attenuation is 1
    xf.ambient[0][0] = xf.ambient[0][1] = xf.ambient[0][2] = 0.5f;
    // Surface normal points +Z; the light sits at -Z, so the dot product is -1.
    xf.light[0].pos[0] = 0;
    xf.light[0].pos[1] = 0;
    xf.light[0].pos[2] = -100.0f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];

    xf.chan[0].diffuseFn = SBR_DF_NONE;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 1.0f, "DF_NONE: the light contributes its full colour, 0.5 + 1.0 clamped");

    xf.chan[0].diffuseFn = SBR_DF_CLAMP;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.5f, "DF_CLAMP: a back-facing light contributes nothing, ambient survives");

    xf.chan[0].diffuseFn = SBR_DF_SIGN;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.0f, "DF_SIGN: a back-facing light SUBTRACTS, 0.5 - 1.0 clamps to black");

    // Facing the light, SIGN and CLAMP must agree.
    xf.light[0].pos[2] = 100.0f;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 1.0f, "DF_SIGN facing the light: 0.5 + 1.0 clamped to 1.0");
    xf.chan[0].diffuseFn = SBR_DF_CLAMP;
    float out2[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out2, nullptr);
    check_near(out2[0], out[0], "SIGN and CLAMP agree when the light is in front");
}

// ---------------------------------------------------------------------------------------------
// Distance attenuation is the quadratic dot(distAtt, (1, d, d*d)) in the DENOMINATOR, so
// distAtt = (0,0,1) gives an inverse-square falloff.
void test_distance_attenuation() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 1;
    xf.chan[0].attnEnable = 1;
    xf.chan[0].attnSpot = 1; // SPOT
    xf.chan[0].diffuseFn = SBR_DF_NONE;
    xf.light[0].distAtt[0] = 0.0f;
    xf.light[0].distAtt[1] = 0.0f;
    xf.light[0].distAtt[2] = 1.0f;
    xf.light[0].pos[2] = 2.0f; // distance 2 -> 1/4
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.25f, "SPOT: inverse-square distance attenuation at d=2");

    xf.light[0].pos[2] = 1.0f;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 1.0f, "SPOT: at d=1 the same light is at full strength");

    // A dead slot — every distance coefficient zero — is 0/0. It must contribute NOTHING rather
    // than a NaN, which would poison the whole channel.
    xf.light[0].distAtt[0] = xf.light[0].distAtt[1] = xf.light[0].distAtt[2] = 0.0f;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check(!std::isnan(out[0]), "SPOT: an all-zero-distAtt light does not produce NaN");
    check_near(out[0], 0.0f, "SPOT: an all-zero-distAtt light contributes nothing");

    // GX_AF_NONE ignores attenuation entirely, however far away the light is.
    xf.chan[0].attnEnable = 0;
    xf.light[0].pos[2] = 10000.0f;
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 1.0f, "AF_NONE: distance is irrelevant, attenuation is 1");
}

// The angular term: cosAtt against the light's own direction, clamped at zero so a surface behind
// the cone gets nothing rather than a negative contribution.
void test_spot_angle() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 1;
    xf.chan[0].attnEnable = 1;
    xf.chan[0].attnSpot = 1;
    xf.chan[0].diffuseFn = SBR_DF_NONE;
    // cosAtt = (0,1,0): the contribution is exactly cos(angle).
    xf.light[0].cosAtt[0] = 0.0f;
    xf.light[0].cosAtt[1] = 1.0f;
    xf.light[0].cosAtt[2] = 0.0f;
    xf.light[0].pos[2] = 1.0f; // light on +Z, so ldir is +Z
    xf.light[0].dir[0] = 0;
    xf.light[0].dir[1] = 0;
    xf.light[0].dir[2] = 1.0f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 1.0f, "SPOT angle: ldir aligned with the light direction gives cos = 1");

    xf.light[0].dir[2] = -1.0f; // pointing the other way
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.0f, "SPOT angle: an opposed direction clamps to 0, never negative");
}

// Specular attenuation is driven by the surface normal and light direction, not by the light's
// distance from the vertex. This is the distinction the former private scene evaluator erased by
// treating every enabled attenuation function as SPOT. The SPOT leg is the negative control: with
// the same coefficients it must fall off as 1/d^2, proving the test can distinguish the two modes.
void test_specular_is_not_spotlight() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 1;
    xf.chan[0].attnEnable = 1;
    xf.chan[0].attnSpot = 0; // SPEC
    xf.chan[0].diffuseFn = SBR_DF_NONE;
    xf.light[0].pos[2] = 100.0f;
    xf.light[0].dir[2] = 1.0f;
    xf.light[0].cosAtt[0] = 0.0f;
    xf.light[0].cosAtt[1] = 0.0f;
    xf.light[0].cosAtt[2] = 1.0f;
    xf.light[0].distAtt[0] = 0.0f;
    xf.light[0].distAtt[1] = 0.0f;
    xf.light[0].distAtt[2] = 1.0f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    SbrLightTrace trace{};
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, &trace);
    check_near(out[0], 1.0f, "SPEC: aligned normal/light direction is full strength at d=100");
    check_near(trace.light[0].cosine, 1.0f,
               "SPEC trace reports the normal-driven attenuation coordinate");

    xf.chan[0].attnSpot = 1; // negative control: same data, SPOT
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.0001f, "SPOT control: the same coefficients fall off as 1/d^2");
}

// Two lights accumulate additively, which is how a pair of back-facing SIGN lights drives a
// channel below zero — the plaza case.
void test_two_lights_accumulate() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 0x3;
    xf.chan[0].attnEnable = 0; // AF_NONE, attenuation 1
    xf.chan[0].diffuseFn = SBR_DF_SIGN;
    xf.ambient[0][0] = xf.ambient[0][1] = xf.ambient[0][2] = 0.5f;
    xf.light[0].pos[2] = -100.0f; // both behind the surface
    xf.light[1].pos[2] = -100.0f;
    for (int i = 0; i < 3; ++i) {
        xf.light[0].color[i] = 0.25f;
        xf.light[1].color[i] = 0.25f;
    }
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    SbrLightTrace tr{};
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, &tr);
    // 0.5 + (-1 * 0.25) + (-1 * 0.25) = 0.0
    check_near(out[0], 0.0f, "two back-facing SIGN lights subtract additively to exactly zero");
    check(tr.lights == 2, "trace records both enabled lights");
    check_near(tr.light[0].diffuse, -1.0f, "trace: the first light's diffuse is -1");
}

// Channel 1 has its OWN control, material and ambient registers — the whole point of evaluating it
// separately rather than reusing channel 0's result.
void test_channel_1_is_independent() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 0;
    xf.ambient[0][0] = 0.0f; // channel 0: black
    xf.chan[1].enableLight = 0;
    xf.material[1][0] = 0.75f; // channel 1: material colour, lighting off
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float c0[4], c1[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, c0, nullptr);
    sbr_light_channel(xf, 1, vpos, nrm, vc, c1, nullptr);
    check_near(c0[0], 0.0f, "channel 0 is black here");
    check_near(c1[0], 0.75f, "channel 1 is NOT channel 0 — it has its own registers and control");
}

void test_accumulator_clamps_before_material() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 1;
    xf.chan[0].lightMask = 1;
    xf.chan[0].diffuseFn = SBR_DF_NONE;
    xf.chan[0].attnEnable = 0;
    xf.material[0][0] = 0.5f;
    xf.ambient[0][0] = 1.0f;
    xf.light[0].color[0] = 1.0f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[0], 0.5f,
               "lighting clamps 1 ambient + 1 light before multiplying by 0.5 material");
}

void test_alpha_has_its_own_control() {
    SbrXfState xf = base();
    xf.chan[0].enableLight = 0;
    xf.chan[2].matSrcVertex = 0;
    xf.chan[2].enableLight = 1;
    xf.chan[2].lightMask = 0;
    xf.chan[2].ambSrcVertex = 0;
    xf.material[0][3] = 0.5f;
    xf.ambient[0][3] = 0.25f;
    const float vpos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    float out[4];
    sbr_light_channel(xf, 0, vpos, nrm, vc, out, nullptr);
    check_near(out[3], 0.125f,
               "alpha0 control lights material alpha independently of colour0 control");
}

} // namespace

int main() {
    test_attn_fn_decode();
    test_lighting_disabled();
    test_colour_sources();
    test_diffuse_functions();
    test_distance_attenuation();
    test_spot_angle();
    test_specular_is_not_spotlight();
    test_two_lights_accumulate();
    test_channel_1_is_independent();
    test_accumulator_clamps_before_material();
    test_alpha_has_its_own_control();
    if (g_failures == 0) {
        std::printf("gx_light: all checks passed\n");
        return 0;
    }
    std::printf("gx_light: %d check(s) FAILED\n", g_failures);
    return 1;
}
