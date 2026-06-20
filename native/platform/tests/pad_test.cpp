// pad_test.cpp — TDD harness for the PAD seam (native/platform/pad_impl.cpp).
//
// Verifies the host-input feed round-trip (PADRead) + the REAL GC stick/trigger
// clamp (PADClamp), against hand-computed spec values derived from the decomp's
// ClampRegion {minTrigger 30, maxTrigger 180, minStick 15, maxStick 72, xyStick 40}.

#include <dolphin/pad.h>
#include "pad_input.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static void test_read() {
    PADInit();
    PADStatus in;
    std::memset(&in, 0, sizeof(in));
    in.button = PAD_BUTTON_A; in.stickX = 50; in.stickY = -20; in.err = PAD_ERR_NONE;
    sb_pad_set_state(0, &in);
    sb_pad_set_absent(1);

    PADStatus out[4];
    u32 mask = PADRead(out);
    chk(out[0].button == PAD_BUTTON_A, "read button");
    chk(out[0].stickX == 50 && out[0].stickY == -20, "read stick");
    chk(out[0].err == PAD_ERR_NONE, "chan0 connected");
    chk(out[1].err == PAD_ERR_NO_CONTROLLER, "chan1 absent");
    chk((mask & 0x80000000u) != 0, "mask has chan0");
    chk((mask & 0x40000000u) == 0, "mask lacks chan1");
}

static void test_clamp_deadzone() {
    PADStatus s[4];
    std::memset(s, 0, sizeof(s));
    for (int i = 1; i < 4; ++i) s[i].err = PAD_ERR_NO_CONTROLLER;
    // within dead zone (|x| <= minStick 15) -> 0
    s[0].stickX = 10; s[0].stickY = 0; s[0].err = PAD_ERR_NONE;
    PADClamp(s);
    chk(s[0].stickX == 0 && s[0].stickY == 0, "stick deadzone -> 0");
}

static void test_clamp_axis() {
    PADStatus s[4];
    std::memset(s, 0, sizeof(s));
    for (int i = 1; i < 4; ++i) s[i].err = PAD_ERR_NO_CONTROLLER;
    // pure-X at max: 72 -> 72-15 = 57 (no octagon scaling on a pure axis).
    s[0].stickX = 72; s[0].stickY = 0; s[0].err = PAD_ERR_NONE;
    PADClamp(s);
    chk(s[0].stickX == 57, "stickX 72 -> 57");
    chk(s[0].stickY == 0, "stickY stays 0");
    // sign preserved.
    std::memset(s, 0, sizeof(s));
    for (int i = 1; i < 4; ++i) s[i].err = PAD_ERR_NO_CONTROLLER;
    s[0].stickX = -72; s[0].err = PAD_ERR_NONE;
    PADClamp(s);
    chk(s[0].stickX == -57, "stickX -72 -> -57");
}

static void test_clamp_trigger() {
    PADStatus s[4];
    std::memset(s, 0, sizeof(s));
    for (int i = 1; i < 4; ++i) s[i].err = PAD_ERR_NO_CONTROLLER;
    s[0].err = PAD_ERR_NONE;
    s[0].triggerLeft = 20;    // <= minTrigger 30 -> 0
    s[0].triggerRight = 200;  // > maxTrigger 180 -> 180, then -30 = 150
    PADClamp(s);
    chk(s[0].triggerLeft == 0, "trigger below min -> 0");
    chk(s[0].triggerRight == 150, "trigger 200 -> 150");
    // mid value: 100 - 30 = 70
    std::memset(s, 0, sizeof(s));
    for (int i = 1; i < 4; ++i) s[i].err = PAD_ERR_NO_CONTROLLER;
    s[0].err = PAD_ERR_NONE; s[0].triggerLeft = 100;
    PADClamp(s);
    chk(s[0].triggerLeft == 70, "trigger 100 -> 70");
}

int main() {
    std::printf("== PAD seam unit tests ==\n");
    test_read();
    test_clamp_deadzone();
    test_clamp_axis();
    test_clamp_trigger();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
