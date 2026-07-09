// pad_script.cpp — headless scripted controller input for sms-boot.
//
// The one-runtime architecture has no independent VI-tick hook anymore (that
// belonged to the retired two-thread native/platform stack, deleted in the
// 2026-07-07 one-runtime consolidation — see CLAUDE.md's ARCHITECTURE
// section). The per-frame boundary now is sb_frame_present() in
// frame_seam.cpp, which itself drives VIWaitForRetrace() `retraces` times to
// advance the SDK's own retrace counter. This driver hooks into that loop
// (sb_pad_script_tick(), called once per VIWaitForRetrace) instead of a
// dedicated tick callback, and feeds the game through Aurora's existing
// virtual-pad seam (PADSetVirtualStatus in extern/aurora/lib/dolphin/pad/pad.cpp)
// rather than a bespoke sb_pad_set_state seam (that seam no longer exists;
// PADSetVirtualStatus already ORs buttons / dominant-picks sticks against
// whatever the keyboard or a real controller reports in PADRead, which is
// exactly the "compose, don't replace" behavior the old driver wanted).
//
// Script format is unchanged from the pre-consolidation implementation
// (native/src/pad_driver.cpp, commit 7082de4) so existing tools/render/*.sh
// scripts stay valid:
//
//   SB_PAD_SCRIPT="F:TOKENS F:TOKENS ..."  — at retrace count >= F, hold
//       TOKENS until the next entry. TOKENS = '+'-joined of: A B X Y START Z
//       L R (digital) and UP DOWN LEFT RIGHT (full-deflect stick) and
//       CUP/CDOWN/CLEFT/CRIGHT (C-stick). Use '-' or NONE for neutral.
//       Entries separated by space/comma/semicolon.
//       Example: SB_PAD_SCRIPT="60:START 64:- 120:A 124:- 3200:UP 3260:UP+A"
//
// Channel 0 only, composed with keyboard/gamepad input via Aurora's virtual
// pad merge (PADSetVirtualStatus), so this is additive and inert when
// SB_PAD_SCRIPT is unset. Game's PADClamp applies the real dead-zone to the
// full-deflect ±72 the same way it would a physical stick.

#include <dolphin/pad.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct PadState { uint16_t button = 0; int8_t sx = 0, sy = 0, cx = 0, cy = 0; };
struct ScriptEntry { uint32_t frame; PadState st; std::string label; };

std::vector<ScriptEntry> g_script;   // sorted by frame ascending
bool g_enabled = false;
int g_lastFired = -1; // index into g_script of the last entry whose action fired

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
        e.label = item.substr(colon + 1);
        e.st = parse_tokens(e.label);
        g_script.push_back(std::move(e));
    }
    // insertion sort by frame (small lists; keep stable for equal frames)
    for (size_t a = 1; a < g_script.size(); ++a)
        for (size_t b = a; b > 0 && g_script[b - 1].frame > g_script[b].frame; --b)
            std::swap(g_script[b - 1], g_script[b]);
}

} // namespace

extern "C" {

// Called once from main() before the frame seam starts pumping retraces.
void sb_pad_script_install(void) {
    const char* spec = std::getenv("SB_PAD_SCRIPT");
    if (!spec || !spec[0]) return;
    parse_script(spec);
    g_enabled = !g_script.empty();
    if (g_enabled) {
        std::fprintf(stdout, "[padscript] loaded %zu event(s) from SB_PAD_SCRIPT\n", g_script.size());
        std::fflush(stdout);
    }
}

// Called once per VIWaitForRetrace() from the frame seam (frame_seam.cpp),
// with the SDK's current retrace count.
void sb_pad_script_tick(uint32_t retrace_count) {
    if (!g_enabled) return;

    int active = -1;
    for (size_t k = 0; k < g_script.size(); ++k) {
        if (g_script[k].frame <= retrace_count) active = (int)k;
        else break;
    }
    if (active < 0) return;

    if (active != g_lastFired) {
        g_lastFired = active;
        std::fprintf(stdout, "[padscript] fire frame=%u entry=%u:%s\n",
                     retrace_count, g_script[active].frame, g_script[active].label.c_str());
        std::fflush(stdout);
    }

    const PadState& s = g_script[active].st;
    PADStatus st;
    std::memset(&st, 0, sizeof(st));
    st.button = s.button;
    st.stickX = s.sx;
    st.stickY = s.sy;
    st.substickX = s.cx;
    st.substickY = s.cy;
    st.err = PAD_ERR_NONE;
    PADSetVirtualStatus(0, &st);
}

} // extern "C"
