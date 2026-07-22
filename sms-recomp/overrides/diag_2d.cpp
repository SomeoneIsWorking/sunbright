// diag_2d.cpp — identify WHICH 2D class draws a given on-screen element.
//
// Enabled with SBR_DIAG_2D=1; inert otherwise (the overrides are registered but pass straight
// through, and these are hot J2D entry points).
//
// The widescreen work keeps running into the same question: an element is misplaced on screen, and
// the fix depends entirely on which draw path put it there — the quad emitter, J2DPicture::draw,
// fill_rect, or a text box. Guessing from a screenshot has been wrong twice (the subtitle band is
// not fill_rect; the HUD gauge parts do not all go through the emitter), so this reports the .blo
// name and the pane transform at each of the J2D entry points, and the answer arrives on the probe
// at /2dclass instead of in a rebuild.
//
//   curl 127.0.0.1:17654/2dclass

#include "overrides.h"

#include "../runtime/probe_server.h"

#include <intrinsics.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

extern "C" void func_802cc758(CPUState&);   // J2DPicture::drawSelf
extern "C" void func_802ccef4(CPUState&);   // J2DPicture::draw
extern "C" void func_802d0b28(CPUState&);   // J2DTextBox::draw
extern "C" void func_802d0d70(CPUState&);   // J2DTextBox::drawSelf
extern "C" void func_802d01c8(CPUState&);   // J2DScreen::drawSelf

namespace {

bool diag_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_DIAG_2D");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

struct Seen {
    unsigned long hits = 0;
    float m00 = 0, m03 = 0;
};
std::map<std::string, Seen> g_seen;

bool guest_obj(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

// J2DPane: .blo name fourcc at +0x10, transform at +0x84 (X translation at +0x0C).
void note(const char* cls, u32 self) {
    if (!guest_obj(self)) return;
    const u32 t = sb_r32(self + 0x10);
    char nm[5];
    for (int i = 0; i < 4; i++) {
        const u8 c = (t >> (24 - i * 8)) & 0xff;
        nm[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    nm[4] = '\0';
    char key[64];
    std::snprintf(key, sizeof key, "%-18s %s", cls, nm);
    auto& e = g_seen[key];
    ++e.hits;
    e.m00 = guest_f32(self + 0x84);
    e.m03 = guest_f32(self + 0x84 + 0x0C);
}

const bool g_probe = [] {
    sb_probe_register("/2dclass", "which J2D class drew each pane (needs SBR_DIAG_2D=1)",
                      [](const ProbeArgs&) {
                          if (!diag_on()) return std::string("set SBR_DIAG_2D=1 to record\n");
                          std::string out;
                          char buf[128];
                          for (const auto& [k, e] : g_seen) {
                              std::snprintf(buf, sizeof buf, "%-26s m00=%7.3f m03=%8.2f hits=%lu\n",
                                            k.c_str(), (double)e.m00, (double)e.m03, e.hits);
                              out += buf;
                          }
                          if (out.empty()) out = "nothing recorded yet\n";
                          return out;
                      });
    return true;
}();

void ov_pic_drawself(CPUState& cpu) {
    if (diag_on()) note("J2DPicture::drawSelf", cpu.gpr[3]);
    func_802cc758(cpu);
}
void ov_pic_draw(CPUState& cpu) {
    if (diag_on()) note("J2DPicture::draw", cpu.gpr[3]);
    func_802ccef4(cpu);
}
void ov_text_draw(CPUState& cpu) {
    if (diag_on()) note("J2DTextBox::draw", cpu.gpr[3]);
    func_802d0b28(cpu);
}
void ov_text_drawself(CPUState& cpu) {
    if (diag_on()) note("J2DTextBox::drawSelf", cpu.gpr[3]);
    func_802d0d70(cpu);
}
void ov_screen_drawself(CPUState& cpu) {
    if (diag_on()) note("J2DScreen::drawSelf", cpu.gpr[3]);
    func_802d01c8(cpu);
}

} // namespace

SB_OVERRIDE(0x802cc758u, ov_pic_drawself,    "J2DPicture::drawSelf", "diagnostic: 2D class census")
SB_OVERRIDE(0x802ccef4u, ov_pic_draw,        "J2DPicture::draw",     "diagnostic: 2D class census")
SB_OVERRIDE(0x802d0b28u, ov_text_draw,       "J2DTextBox::draw",     "diagnostic: 2D class census")
SB_OVERRIDE(0x802d0d70u, ov_text_drawself,   "J2DTextBox::drawSelf", "diagnostic: 2D class census")
SB_OVERRIDE(0x802d01c8u, ov_screen_drawself, "J2DScreen::drawSelf",  "diagnostic: 2D class census")
