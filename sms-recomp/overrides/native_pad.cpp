// native_pad.cpp — controller input.
//
// SI is modelled as a transport but nothing is attached to it, so the real PADRead reports
// every port as disconnected (err = -1) and the game sees no input. Input on a PC port comes
// from the host, not from an SI response packet, so this is an override rather than a device.
//
// For now the only source is a SCRIPT, which is what automated runs need: reaching
// file-select requires pressing START on the title, and an agent run has no keyboard. The
// decomp runtime has the same facility (SB_PAD_SCRIPT); this is its recomp counterpart.
//
//   SBR_PAD_SCRIPT="600:START,640:-,900:STICK=0/-90,1000:STICK=0/0+A"
//
// keys on the PAD read count (one per frame): from read 600 hold START, from 640 hold
// nothing. Buttons are named (A B X Y Z L R START UP DOWN LEFT RIGHT) or "-" for none.
//
// CSTICK=<x>/<y> sets the C-STICK, which rotates the CAMERA. It exists for the 60fps measurement
// harness: interpolation quality can only be graded on a moment whose motion is GEOMETRY, and a
// walk-forward script leaves the plaza camera nearly parked (measured at 0.25 units/tick) while the
// frame's per-tick change is dominated by the 2D news ticker and graffiti particles -- content no
// matrix interpolation covers. Rotating the camera moves the whole scene and nothing else.
//
// STICK=<x>/<y> sets the ANALOG stick (-128..127; '/' because ',' already separates steps, and
// no leading '+' because '+' combines tokens: "STICK=0/-90+A"). Buttons alone cannot drive this
// game: file-select is chosen by walking Mario into a file block and head-butting it, and Mario
// moves on the analog stick — a button-only script can never leave the menu.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

// Aurora's pad service. Declared here rather than including <dolphin/pad.h>: that header pulls
// in aurora's dolphin prelude, which redefines the PPC intrinsics this translation unit already
// has. Only the layout of PADStatus and these two entry points matter.
struct PADStatus {
    unsigned short button;
    signed char stickX, stickY, substickX, substickY;
    unsigned char triggerLeft, triggerRight, analogA, analogB;
    signed char err;
};
extern "C" unsigned int PADRead(PADStatus* status);
extern "C" int PADInit(void);
// {scancode, padButton} pairs; scancode <= 0 (PAD_KEY_INVALID) means "unbound".
struct PADKeyButtonBinding { int scancode; unsigned int padButton; };
extern "C" PADKeyButtonBinding* PADGetKeyButtonBindings(unsigned port, unsigned* count);
extern "C" void PADSetKeyboardActive(unsigned int port, int active);

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// PADStatus, 12 bytes each (decomp/sms/include/dolphin/pad.h):
//   +0x00 u16 button, +0x02 stickX, +0x03 stickY, +0x04 substickX, +0x05 substickY,
//   +0x06 triggerLeft, +0x07 triggerRight, +0x08 analogA, +0x09 analogB, +0x0A s8 err
constexpr u32 PAD_STATUS_SIZE = 12;
// PADStatus: u16 button, s8 stickX/stickY, s8 substickX/substickY, u8 triggerL/R, u8 analogA/B,
// s8 err. Matching the SDK layout matters because the game reads the sticks, not just buttons.
constexpr u32 PS_BUTTON = 0x00, PS_STICK_X = 0x02, PS_STICK_Y = 0x03;
constexpr u32 PS_SUB_X = 0x04, PS_SUB_Y = 0x05, PS_TRIG_L = 0x06, PS_TRIG_R = 0x07;
constexpr u32 PS_ANALOG_A = 0x08, PS_ANALOG_B = 0x09, PS_ERR = 0x0A;

constexpr s8 PAD_ERR_NONE      = 0;
constexpr s8 PAD_ERR_NO_CTRLR  = -1;

struct Step {
    long frame;
    u16 buttons;
    // 0x8000 = "this step does not touch the stick", so a later button-only step does not
    // silently re-centre a stick an earlier step set.
    int stickX = 0x8000;
    int stickY = 0x8000;
    // The C-STICK, which in this game rotates the CAMERA. Kept separate from the analog stick for
    // the same reason as above: a later step that only touches one must not re-centre the other.
    int subX = 0x8000;
    int subY = 0x8000;
};
std::vector<Step> g_script;
long g_reads = 0;

u16 button_bit(const std::string& name) {
    if (name == "LEFT")  return 0x0001;
    if (name == "RIGHT") return 0x0002;
    if (name == "DOWN")  return 0x0004;
    if (name == "UP")    return 0x0008;
    if (name == "Z")     return 0x0010;
    if (name == "R")     return 0x0020;
    if (name == "L")     return 0x0040;
    if (name == "A")     return 0x0100;
    if (name == "B")     return 0x0200;
    if (name == "X")     return 0x0400;
    if (name == "Y")     return 0x0800;
    if (name == "START") return 0x1000;
    if (name == "-")     return 0;
    lucent::error("pad", "unknown button '{}' in SBR_PAD_SCRIPT", name);
    std::abort();
}

void parse_script() {
    const char* env = std::getenv("SBR_PAD_SCRIPT");
    if (!env || !*env) return;
    std::string s(env), item;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) {
            const size_t colon = item.find(':');
            if (colon == std::string::npos) {
                lucent::error("pad", "SBR_PAD_SCRIPT entry '{}' is not <frame>:<button>", item);
                std::abort();
            }
            Step st{std::strtol(item.substr(0, colon).c_str(), nullptr, 10), 0};
            // A step may combine buttons with '+', e.g. 600:A+START
            std::string btns = item.substr(colon + 1);
            size_t bp = 0;
            while (bp <= btns.size()) {
                const size_t plus = btns.find('+', bp);
                const std::string one =
                    btns.substr(bp, plus == std::string::npos ? std::string::npos : plus - bp);
                if (!one.empty()) {
                    const bool isC = one.rfind("CSTICK=", 0) == 0;
                    if (isC || one.rfind("STICK=", 0) == 0) {
                        const char* what = isC ? "CSTICK" : "STICK";
                        const std::string v = one.substr(isC ? 7 : 6);
                        const size_t slash = v.find('/');
                        if (slash == std::string::npos) {
                            lucent::error("pad", "SBR_PAD_SCRIPT '{}' must be {}=<x>/<y>", one, what);
                            std::abort();
                        }
                        const int x = (int)std::strtol(v.substr(0, slash).c_str(), nullptr, 10);
                        const int y = (int)std::strtol(v.substr(slash + 1).c_str(), nullptr, 10);
                        if (x < -128 || x > 127 || y < -128 || y > 127) {
                            lucent::error("pad", "SBR_PAD_SCRIPT {} {}/{} out of range -128..127",
                                          what, x, y);
                            std::abort();
                        }
                        if (isC) { st.subX = x; st.subY = y; }
                        else     { st.stickX = x; st.stickY = y; }
                    } else {
                        st.buttons |= button_bit(one);
                    }
                }
                if (plus == std::string::npos) break;
                bp = plus + 1;
            }
            g_script.push_back(st);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    for (const auto& st : g_script)
        lucent::info("pad", "script: from read {} hold 0x{:04x} stick {}/{} cstick {}/{}", st.frame,
                     st.buttons, st.stickX == 0x8000 ? 999 : st.stickX,
                     st.stickY == 0x8000 ? 999 : st.stickY,
                     st.subX == 0x8000 ? 999 : st.subX, st.subY == 0x8000 ? 999 : st.subY);
}

u16 scripted_buttons() {
    u16 held = 0;
    for (const auto& st : g_script)
        if (g_reads >= st.frame) held = st.buttons;   // last matching step wins
    return held;
}

// Latest step at or before now that actually SET a stick value; 0x8000 if none has.
void scripted_stick(int& x, int& y) {
    x = 0x8000;
    y = 0x8000;
    for (const auto& st : g_script) {
        if (g_reads < st.frame) continue;
        if (st.stickX != 0x8000) x = st.stickX;
        if (st.stickY != 0x8000) y = st.stickY;
    }
}

void scripted_substick(int& x, int& y) {
    x = 0x8000;
    y = 0x8000;
    for (const auto& st : g_script) {
        if (g_reads < st.frame) continue;
        if (st.subX != 0x8000) x = st.subX;
        if (st.subY != 0x8000) y = st.subY;
    }
}

// PADRead(PADStatus status[4]) -> u32 mask of ports that reported a read error.
void pad_read(CPUState& cpu) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        parse_script();
        // aurora's PADInit is what POPULATES the default key bindings; without it the binding
        // table is empty and an "active" keyboard pad reports no button ever pressed. The decomp
        // runtime gets this for free because the game's PAD SDK *is* aurora's, so its PADInit
        // call lands here. This runtime recompiles the guest's own PADInit instead, which knows
        // nothing about aurora — so aurora's must be called explicitly or the keyboard is dead.
        ::PADInit();
        // Let the keyboard drive port 0, the same arrangement the decomp runtime uses.
        ::PADSetKeyboardActive(0, true);
        // Report how many keys are actually bound. "Keyboard active" is not the same as
        // "keyboard usable": with an unpopulated binding table the pad is active and reports
        // nothing, which is indistinguishable from a player pressing no keys. Counting them
        // makes a dead keyboard say so instead of looking like idleness.
        {
            unsigned count = 0;
            const PADKeyButtonBinding* b = ::PADGetKeyButtonBindings(0, &count);
            unsigned bound = 0;
            for (unsigned i = 0; b != nullptr && i < count; ++i)
                if (b[i].scancode > 0) ++bound;
            if (bound == 0)
                lucent::error("pad", "keyboard is active but NO keys are bound — input is dead");
            else
                lucent::info("pad", "keyboard: {} of {} keys bound", bound, count);
        }
    }

    const u32 out = cpu.gpr[3];
    ++g_reads;

    // REAL input from aurora (keyboard and any attached controller), OR-ed with the script so
    // an automated run and a human at the keyboard both work — and so a scripted run can still
    // be nudged by hand. Aurora owns the window's input, exactly as it does for the decomp
    // runtime; without this the recomp could only ever replay SBR_PAD_SCRIPT and a window was
    // useless to a person.
    PADStatus host[4] = {};
    ::PADRead(host);

    const u16 buttons = (u16)(scripted_buttons() | host[0].button);
    static u16 last = 0xFFFF;
    if (buttons != last) {
        last = buttons;
        lucent::info("pad", "read {}: buttons 0x{:04x}", g_reads, buttons);
    }

    for (u32 i = 0; i < 4; i++) {
        const u32 p = out + i * PAD_STATUS_SIZE;
        for (u32 b = 0; b < PAD_STATUS_SIZE; b++) sb_w8(p + b, 0);
        if (i == 0) {
            sb_w16(p + PS_BUTTON, buttons);
            // A script value overrides the host stick; where the script is silent the
            // keyboard still drives, so a scripted run can be nudged by hand.
            int sx = 0x8000, sy = 0x8000;
            scripted_stick(sx, sy);
            sb_w8 (p + PS_STICK_X,  (u8)(s8)(sx == 0x8000 ? host[0].stickX : sx));
            sb_w8 (p + PS_STICK_Y,  (u8)(s8)(sy == 0x8000 ? host[0].stickY : sy));
            int cx = 0x8000, cy = 0x8000;
            scripted_substick(cx, cy);
            sb_w8 (p + PS_SUB_X,    (u8)(s8)(cx == 0x8000 ? host[0].substickX : cx));
            sb_w8 (p + PS_SUB_Y,    (u8)(s8)(cy == 0x8000 ? host[0].substickY : cy));
            sb_w8 (p + PS_TRIG_L,   host[0].triggerLeft);
            sb_w8 (p + PS_TRIG_R,   host[0].triggerRight);
            sb_w8 (p + PS_ANALOG_A, host[0].analogA);
            sb_w8 (p + PS_ANALOG_B, host[0].analogB);
            sb_w8 (p + PS_ERR, (u8)PAD_ERR_NONE);
        } else {
            // Ports 1-3 genuinely have nothing attached; say so rather than reporting a
            // connected pad that never presses anything.
            sb_w8(p + PS_ERR, (u8)PAD_ERR_NO_CTRLR);
        }
    }
    cpu.gpr[3] = 0;   // no port reported a read error
}

} // namespace

SB_OVERRIDE(0x80351600u, pad_read, "PADRead",
            "host-side input; SI has no attached controller to answer a real read")
