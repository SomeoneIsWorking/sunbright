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
//   SBR_PAD_SCRIPT="600:START,640:-"
//
// keys on the PAD read count (one per frame): from read 600 hold START, from 640 hold
// nothing. Buttons are named (A B X Y Z L R START UP DOWN LEFT RIGHT) or "-" for none.

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

struct Step { long frame; u16 buttons; };
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
                if (!one.empty()) st.buttons |= button_bit(one);
                if (plus == std::string::npos) break;
                bp = plus + 1;
            }
            g_script.push_back(st);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    for (const auto& st : g_script)
        lucent::info("pad", "script: from read {} hold 0x{:04x}", st.frame, st.buttons);
}

u16 scripted_buttons() {
    u16 held = 0;
    for (const auto& st : g_script)
        if (g_reads >= st.frame) held = st.buttons;   // last matching step wins
    return held;
}

// PADRead(PADStatus status[4]) -> u32 mask of ports that reported a read error.
void pad_read(CPUState& cpu) {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        parse_script();
        // Let the keyboard drive port 0, the same arrangement the decomp runtime uses.
        ::PADSetKeyboardActive(0, true);
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
            sb_w8 (p + PS_STICK_X,  (u8)host[0].stickX);
            sb_w8 (p + PS_STICK_Y,  (u8)host[0].stickY);
            sb_w8 (p + PS_SUB_X,    (u8)host[0].substickX);
            sb_w8 (p + PS_SUB_Y,    (u8)host[0].substickY);
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
