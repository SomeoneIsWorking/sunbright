// setlight_test — unit tests for the pure port of TLightCommon::setLight
// (native/render/sms_boot_setlight.h), decompiled from GMSE01 @0x80229a30 /
// TLightMario @0x80229610. Asserts spec-computed ground truth for the stage-light
// selection that the GX-command-stream value oracle measures (light COUNT + per-light
// position/colour/specular). No Dolphin, no GPU, no ROM.

#include "sms_boot_setlight.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static bool feq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Identity 3x4 view matrix (row-major).
static void identity_view(float v[12]) {
    for (int i = 0; i < 12; ++i) v[i] = 0.f;
    v[0] = v[5] = v[10] = 1.f;
}

// ── 1. Effect ON → exactly 3 lights in slots 0/1/2 (the file-select oracle case). ──
static void test_effect_on_three_lights() {
    sb::SetLightIn in{};
    in.lgColor[0]=1; in.lgColor[1]=1; in.lgColor[2]=1; in.lgColor[3]=1;   // white sun
    in.lgPos[0]=200000; in.lgPos[1]=500000; in.lgPos[2]=200000;          // Light-Group[0]
    identity_view(in.view);
    in.effectOn = true;
    in.effPos[0]=200000; in.effPos[1]=500000; in.effPos[2]=200000;
    in.effColor[0]=1; in.effColor[1]=1; in.effColor[2]=1; in.effColor[3]=1;

    sb::OutLight out[3];
    int n = sb::build_stage_lights(in, out);
    CHECK(n == 3, "effect-on loads exactly 3 lights (matches stage-15 oracle)");
    CHECK(out[0].present && out[1].present && out[2].present, "slots 0,1,2 all present");
    // All three white (oracle: 3 white lights).
    for (int s = 0; s < 3; ++s)
        CHECK(feq(out[s].color[0],1) && feq(out[s].color[1],1) && feq(out[s].color[2],1),
              "light colour white");
    // GX_LIGHT0 positional under identity view = raw world pos.
    CHECK(feq(out[0].pos[0],200000) && feq(out[0].pos[1],500000) && feq(out[0].pos[2],200000),
          "GX_LIGHT0 positional = view*lgPos");
    CHECK(!out[0].specular && !out[1].specular, "L0/L1 are not specular");
    CHECK(out[2].specular, "GX_LIGHT2 is the specular/directional light");
}

// ── 2. Effect OFF → only GX_LIGHT0 and GX_LIGHT2 load (slot 1 absent), count 2. ──
static void test_effect_off_two_lights() {
    sb::SetLightIn in{};
    in.lgColor[0]=in.lgColor[1]=in.lgColor[2]=in.lgColor[3]=1;
    in.lgPos[0]=200000; in.lgPos[1]=500000; in.lgPos[2]=200000;
    identity_view(in.view);
    in.effectOn = false;

    sb::OutLight out[3];
    int n = sb::build_stage_lights(in, out);
    CHECK(n == 2, "effect-off loads 2 lights (GX_LIGHT0 + GX_LIGHT2)");
    CHECK(out[0].present && !out[1].present && out[2].present, "slot 1 (effect) absent");
}

// ── 3. GX_LIGHT2 direction = -normalize(world light pos) (the FUN_8034a5d0 + negate). ──
static void test_directional_is_negated_unit() {
    sb::SetLightIn in{};
    in.lgColor[0]=in.lgColor[1]=in.lgColor[2]=in.lgColor[3]=1;
    in.lgPos[0]=3; in.lgPos[1]=0; in.lgPos[2]=4;   // |.|=5
    identity_view(in.view);
    in.effectOn = false;

    sb::OutLight out[3];
    sb::build_stage_lights(in, out);
    // dir = -(3,0,4)/5 = (-0.6, 0, -0.8); magnitude 1.
    CHECK(feq(out[2].pos[0],-0.6f) && feq(out[2].pos[1],0.f) && feq(out[2].pos[2],-0.8f),
          "specular dir = -normalize(lgPos)");
    float m = std::sqrt(out[2].pos[0]*out[2].pos[0] + out[2].pos[1]*out[2].pos[1] +
                        out[2].pos[2]*out[2].pos[2]);
    CHECK(feq(m,1.f), "specular dir is unit length");
}

// ── 4. View transform applies to the positional light (PSMTXMultVec). ──
static void test_positional_view_transform() {
    sb::SetLightIn in{};
    in.lgColor[0]=in.lgColor[1]=in.lgColor[2]=in.lgColor[3]=1;
    in.lgPos[0]=10; in.lgPos[1]=20; in.lgPos[2]=30;
    // View = translate (+1,+2,+3) on top of identity rotation.
    identity_view(in.view);
    in.view[3]=1; in.view[7]=2; in.view[11]=3;
    in.effectOn = false;

    sb::OutLight out[3];
    sb::build_stage_lights(in, out);
    CHECK(feq(out[0].pos[0],11) && feq(out[0].pos[1],22) && feq(out[0].pos[2],33),
          "GX_LIGHT0 pos = M*(lgPos,1)");
    // Specular dir is normalized from WORLD pos, unaffected by view translation.
    float exp = 1.f / std::sqrt(10.f*10+20*20+30*30);
    CHECK(feq(out[2].pos[0], -10*exp) && feq(out[2].pos[1], -20*exp) && feq(out[2].pos[2], -30*exp),
          "specular dir uses world pos (pre-view)");
}

int main() {
    test_effect_on_three_lights();
    test_effect_off_two_lights();
    test_directional_is_negated_unit();
    test_positional_view_transform();
    if (g_fail) { std::fprintf(stderr, "setlight_test: %d FAILED\n", g_fail); return 1; }
    std::printf("setlight_test: all passed\n");
    return 0;
}
