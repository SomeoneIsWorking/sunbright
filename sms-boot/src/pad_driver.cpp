// pad_driver.cpp — headless scripted controller input for sms-boot.
//
// The native boot has no window/keyboard, so to drive past idle into gameplay code
// (movement, collision, camera, FLUDD, scenario events) for blocker-hunting, this
// installs a VI tick hook (vi_present.h) that feeds the PAD seam (sb_pad_set_state)
// a scripted button/stick state each retrace. Off unless an env var asks for it, so
// normal runs are unaffected.
//
// Env interface:
//   SB_PAD_SCRIPT="F:TOKENS F:TOKENS ..."  — at retrace count >= F, hold TOKENS until
//       the next entry. TOKENS = '+'-joined of: A B X Y START Z L R (digital) and
//       UP DOWN LEFT RIGHT (full-deflect stick) and CUP/CDOWN/CLEFT/CRIGHT (C-stick).
//       Use '-' or NONE for neutral. Entries separated by space/comma/semicolon.
//       Example: SB_PAD_SCRIPT="60:START 64:- 120:A 124:- 3200:UP 3260:UP+A 3264:UP"
//   SB_PAD_DRIVE=1            — convenience roam: from SB_PAD_DRIVE_FROM (default 3200),
//       hold UP and pulse A (jump) every ~24 frames forever. Ignored if SB_PAD_SCRIPT set.
//   SB_PAD_DRIVE_FROM=N       — first retrace for the roam (default 3200).
//
// Notes: state is on channel 0 only. The game's PADClamp applies the real dead-zone, so
// full-deflect ±72 maps to a clamped octagon exactly like a real stick.

#include <dolphin/pad.h>
#include "../common/vi_present.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>

extern "C" void sb_pad_set_state(int chan, const PADStatus* status);

namespace {

struct PadState { uint16_t button = 0; int8_t sx = 0, sy = 0, cx = 0, cy = 0; };
struct ScriptEntry { uint32_t frame; PadState st; };

std::vector<ScriptEntry> g_script;   // sorted by frame ascending
bool g_drive = false;
uint32_t g_drive_from = 3200;
bool g_enabled = false;

void apply_token(PadState& st, const std::string& tok) {
    if (tok == "A") st.button |= PAD_BUTTON_A;
    else if (tok == "B") st.button |= PAD_BUTTON_B;
    else if (tok == "X") st.button |= PAD_BUTTON_X;
    else if (tok == "Y") st.button |= PAD_BUTTON_Y;
    else if (tok == "START") st.button |= PAD_BUTTON_START;
    else if (tok == "Z") st.button |= PAD_TRIGGER_Z;
    else if (tok == "L") st.button |= PAD_TRIGGER_L;
    else if (tok == "R") st.button |= PAD_TRIGGER_R;
    else if (tok == "UP") st.sy = 72;
    else if (tok == "DOWN") st.sy = -72;
    else if (tok == "LEFT") st.sx = -72;
    else if (tok == "RIGHT") st.sx = 72;
    else if (tok == "CUP") st.cy = 72;
    else if (tok == "CDOWN") st.cy = -72;
    else if (tok == "CLEFT") st.cx = -72;
    else if (tok == "CRIGHT") st.cx = 72;
    // '-' / NONE / unknown -> neutral contribution
}

PadState parse_tokens(const std::string& s) {
    PadState st;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('+', i);
        if (j == std::string::npos) j = s.size();
        apply_token(st, s.substr(i, j - i));
        i = j + 1;
    }
    return st;
}

void parse_script(const char* spec) {
    std::string s(spec);
    // split on space/comma/semicolon
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == ';')) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != ';') ++j;
        std::string item = s.substr(i, j - i);
        i = j;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        ScriptEntry e;
        e.frame = (uint32_t)std::strtoul(item.substr(0, colon).c_str(), nullptr, 10);
        e.st = parse_tokens(item.substr(colon + 1));
        g_script.push_back(e);
    }
    // insertion sort by frame (small lists; keep stable for equal frames)
    for (size_t a = 1; a < g_script.size(); ++a)
        for (size_t b = a; b > 0 && g_script[b - 1].frame > g_script[b].frame; --b)
            std::swap(g_script[b - 1], g_script[b]);
}

PadState current_state(uint32_t frame) {
    if (g_drive) {
        PadState st;
        if (frame >= g_drive_from) {
            st.sy = 72;                       // hold up
            if (((frame - g_drive_from) % 24) < 4) st.button |= PAD_BUTTON_A;  // pulse jump
        }
        return st;
    }
    // script: last entry whose frame <= now
    PadState st;
    for (const auto& e : g_script) {
        if (e.frame <= frame) st = e.st;
        else break;
    }
    return st;
}

void tick(u32 count, void* /*user*/) {
    if (!g_enabled) return;
    PadState s = current_state(count);
    PADStatus st;
    std::memset(&st, 0, sizeof(st));
    st.button = s.button;
    st.stickX = s.sx; st.stickY = s.sy;
    st.substickX = s.cx; st.substickY = s.cy;
    st.err = PAD_ERR_NONE;
    sb_pad_set_state(0, &st);
}

} // namespace

extern "C" void sb_pad_driver_install() {
    if (const char* spec = std::getenv("SB_PAD_SCRIPT")) {
        if (spec[0]) { parse_script(spec); g_enabled = !g_script.empty(); }
    }
    if (!g_enabled) {
        if (const char* d = std::getenv("SB_PAD_DRIVE")) {
            if (d[0] && d[0] != '0') {
                g_drive = true; g_enabled = true;
                if (const char* f = std::getenv("SB_PAD_DRIVE_FROM"))
                    g_drive_from = (uint32_t)std::strtoul(f, nullptr, 10);
            }
        }
    }
    if (g_enabled) {
        std::printf("[pad-driver] enabled (%s)\n",
                    g_drive ? "roam" : "script");
        sb_vi_set_tick_hook(tick, nullptr);
    }
}
