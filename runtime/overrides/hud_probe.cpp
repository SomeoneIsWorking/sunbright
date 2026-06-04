// TEMP diagnostic: find which indirect (overridable) draw entry the HUD counters pass through.
// Enable with SUNBRIGHT_HUD_FINDDRAW=1 SUNBRIGHT_RENDERPORT_LOG=1. Delete once the lever is found.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>
static void name_at(u32 self, char out[5]) {
    u32 t = (self >= 0x80000000u && self < 0x81800000u) ? mem_r32(self + 0x10) : 0;
    for (int i = 0; i < 4; i++) { u8 c = (t >> (24 - i*8)) & 0xff; out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
    out[4] = 0;
}
template <u32 A> static void probe(CPUState& cpu) {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) { char nm[5]; name_at(cpu.gpr[3], nm);
        std::fprintf(stderr, "[probe %08x] this=%08x name='%s'\n", A, cpu.gpr[3], nm); }
    if (RecompFunc o = recomp_raw(A)) o(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool reg = [] {
    if (getenv("SUNBRIGHT_HUD_FINDDRAW")) {
        register_override(0x802cc758u, &probe<0x802cc758u>);  // J2DPicture::drawSelf
        // 0x802cc7c0 (drawSelf overload) is owned natively by hud.cpp — do not re-register here.
        register_override(0x802ccef4u, &probe<0x802ccef4u>);  // J2DPicture::draw
        register_override(0x802d0b28u, &probe<0x802d0b28u>);  // J2DTextBox::draw
        register_override(0x802d0d70u, &probe<0x802d0d70u>);  // J2DTextBox::drawSelf
        register_override(0x802d0dd8u, &probe<0x802d0dd8u>);  // J2DTextBox::drawSelf overload
        register_override(0x802d01c8u, &probe<0x802d01c8u>);  // J2DScreen::drawSelf
    }
    return true;
}();
