// hud.cpp — the in-game HUD's widescreen layout (TGCConsole2).
//
// Ported from the retired Dolphin-era runtime/overrides/hud.cpp (git 9283f44^), where the RE below
// was established. Nothing about the game side changed; what changed is that in this runtime EVERY
// call goes through call_ppc, so the overrides are reached without the JIT-block-linking caveats the
// original had to work around.
//
// WHY THIS FILE EXISTS. widescreen.cpp squeezes all 2D toward the screen centre so it survives the
// wide present at the correct aspect. That is right for menus, which are composed as a picture, but
// wrong for the HUD, whose clusters are meant to hug the screen CORNERS: after the squeeze they sit
// ~107px (the pillar) inside each edge, floating. This file pushes them back out — per element, so
// the centred menus are untouched.
//
// WHERE THE POSITION LIVES. Every HUD element (counter digits, icons, lives, the FLUDD gauge, the
// health sun) is drawn as a 2D quad in a 0..640 ortho with an IDENTITY position matrix, so its
// screen position lives entirely in its own transform. Two draw paths reach the hardware:
//
//   1. the 2D quad emitter (0x802cd2ec), called from J2DPicture::drawFullSet — the element's
//      transform is at this+0x84, and its X translation (m03) IS its 640-space screen X (verified:
//      counters 13, lives 223, water gauge 515);
//   2. GXLoadPosMtxImm (0x80362e0c), for the parts drawn through a J2DOrthoGraph rather than the
//      emitter (the FLUDD gauge's blue fill, "WATER", the nozzle icon, the fade curtain).
//
// IDENTITY BY NAME, NEVER A FLAG — twice over, and both matter:
//
//   * Elements are identified by their J2D .blo name (this+0x10, a fourcc). The file-select menu
//     also draws J2DPictures (s_0a/.s_1/n_0a/shn0/yaji/.x_0); the exact HUD role gate rejects their
//     _0<x> roles, so menus are never touched.
//   * Path 2 is keyed on the CALL SITE (cpu.lr), not on a "we are inside drawWater" scope flag.
//     drawWater and friends call sub-functions that leave without returning, so a scope flag would
//     never be cleared and would shift the ENTIRE screen. An lr key has nothing to leak.
//
// Every transform is restored after the draw: these matrices are persistent object state, and
// shifting one without restoring it would accumulate the offset every frame.
//
//   SBR_HUD_OFF=<px>   override the anchor distance (0 disables the anchoring entirely)

#include "overrides.h"

#include "../runtime/probe_server.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

extern "C" void func_802cd2ec(CPUState&);   // the 2D quad emitter
extern "C" void func_80362e0c(CPUState&);   // GXLoadPosMtxImm

int sbr_ws_pillar();   // widescreen.cpp — half the width the 2D squeeze frees at each side

namespace {

constexpr u32 QUAD_EMITTER         = 0x802cd2ecu;
constexpr u32 GX_LOAD_POS_MTX_IMM  = 0x80362e0cu;

// J2DPane transform used by the quad emitter: this+0x84, X translation at +0x0C.
constexpr u32 PANE_MTX   = 0x84;
constexpr u32 MTX_M00    = 0x00;
constexpr u32 MTX_M03    = 0x0C;

enum HudAnchor { A_NONE, A_LEFT, A_CENTER, A_RIGHT };

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

void guest_set_f32(u32 ea, f32 v) {
    u32 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    sb_w32(ea, bits);
}

bool guest_obj(u32 p) { return p >= 0x80000000u && p < 0x81800000u; }

// The pane's .blo name: a fourcc at this+0x10.
void read_fourcc(u32 self, char out[5]) {
    const u32 t = guest_obj(self) ? sb_r32(self + 0x10) : 0;
    for (int i = 0; i < 4; i++) out[i] = (char)((t >> (24 - i * 8)) & 0xff);
    out[4] = '\0';
}

// Which screen edge an element belongs to.
//
//   LEFT    s_* d_* c_*     top-left counters (shine / coin / fruit)
//   CENTER  m_*  go00..02   top-centre lives and the health sun
//   RIGHT   w_t0 nz** xb**  bottom-right FLUDD gauge, nozzle icon, button prompt
HudAnchor hud_anchor(const char n[5]) {
    auto digit = [](char c) { return std::isdigit((unsigned char)c) != 0; };

    if (n[0] == 'g' && n[1] == 'o' && digit(n[2]) && digit(n[3])) return A_CENTER;   // health sun
    if (n[0] == 'n' && n[1] == 'z' && digit(n[2]) && digit(n[3])) return A_RIGHT;    // nozzle icon
    if (n[0] == 'x' && n[1] == 'b' && digit(n[2]) && digit(n[3])) return A_RIGHT;    // button prompt

    char cluster, r0, r1;
    if (n[1] == '_')      { cluster = n[0]; r0 = n[2]; r1 = n[3]; }
    else if (n[2] == '_') { cluster = n[1]; r0 = n[3]; r1 = '\0'; }
    else                  return A_NONE;

    // The role suffix. This is the gate that keeps the file-select menu's own J2DPictures out:
    // their _0<x> roles match none of these.
    const bool hud_role = (r0 == 'b' && r1 == 'a') || (r0 == 'i' && r1 == 'c') ||
                          (r0 == 't' && r1 == 'x') || (r0 == 'x') ||
                          ((r0 == 'n' || r0 == 't') && digit(r1));
    if (!hud_role) return A_NONE;

    switch (cluster) {
    case 's': case 'd': case 'c': return A_LEFT;
    case 'm':                     return A_CENTER;
    case 'w':                     return A_RIGHT;
    default:                      return A_NONE;
    }
}

// Full-width band panes that must STRETCH to the 16:9 edges rather than anchor to one of them: the
// stage-title card's dark masks (TConsoleStr scenario_demo_1.blo 'msk1'/'msk2' — 600x70 strips at
// x=0 behind "DELFINO PLAZA" and the episode text).
bool hud_stretch_band(const char n[5]) {
    return n[0] == 'm' && n[1] == 's' && n[2] == 'k' && (n[3] == '1' || n[3] == '2');
}

// 1/scale — how much to widen something that must span the full screen.
f32 inv_scale() { return 1.0f + (f32)sbr_ws_pillar() / 320.0f; }

// ── Live pane inventory (probe /2d) ──────────────────────────────────────────────────────
// Which 2D panes are on screen right now, and where each one thinks it is. Identifying an
// element by eye from a screenshot is guesswork; this reports the .blo name and the X
// translation the game actually used, so a misplaced element can be matched to its pane
// instead of fixed by nudging a constant.
struct PaneSeen {
    f32 m00 = 0, m03 = 0;
    unsigned long hits = 0;
    HudAnchor anchor = A_NONE;
};
std::map<std::string, PaneSeen> g_panes;

void note_pane(const char nm[5], u32 mtxBase, HudAnchor a) {
    if (nm[0] == '\0') return;
    auto& e = g_panes[nm];
    e.m00 = guest_f32(mtxBase + MTX_M00);
    e.m03 = guest_f32(mtxBase + MTX_M03);
    e.anchor = a;
    ++e.hits;
}

const bool g_pane_probe = [] {
    sb_probe_register("/2d", "2D panes drawn recently: name, transform, anchor", [](const ProbeArgs&) {
        std::string out;
        char buf[160];
        for (const auto& [name, e] : g_panes) {
            static const char* kA[] = {"-", "left", "centre", "right"};
            std::snprintf(buf, sizeof buf, "%-6s m00=%8.3f m03=%8.2f anchor=%-6s hits=%lu\n",
                          name.c_str(), (double)e.m00, (double)e.m03, kA[e.anchor], e.hits);
            out += buf;
        }
        if (out.empty()) out = "no 2D panes recorded yet\n";
        return out;
    });
    return true;
}();

void ov_quad(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    const int pillar = sbr_ws_pillar();
    char nm[5];
    read_fourcc(self, nm);

    if (pillar != 0 && hud_stretch_band(nm) && guest_obj(self)) {
        // The squeeze scales x around the PROJECTION's own centre. TConsoleStr draws under the
        // 600-wide J2D ortho (root 600x480), so that centre is 300 — not 320 like the 640-wide
        // quads below. Un-squeeze around it: x' = (x - 300)/scale + 300. Using the 640-space
        // pillar here instead left the right edge ~7 units short, which was visible as a gap.
        constexpr f32 kCenter = 300.0f;
        const f32 k = inv_scale();
        const f32 m00 = guest_f32(self + PANE_MTX + MTX_M00);
        const f32 m03 = guest_f32(self + PANE_MTX + MTX_M03);
        guest_set_f32(self + PANE_MTX + MTX_M00, m00 * k);
        guest_set_f32(self + PANE_MTX + MTX_M03, (m03 - kCenter) * k + kCenter);
        func_802cd2ec(cpu);
        guest_set_f32(self + PANE_MTX + MTX_M00, m00);
        guest_set_f32(self + PANE_MTX + MTX_M03, m03);
        return;
    }

    // Shifting m03 moves the element to its 16:9 edge. This is a TRANSFORM, not a clip rect, so
    // nothing is cropped by moving it.
    const HudAnchor a = hud_anchor(nm);
    if (guest_obj(self)) note_pane(nm, self + PANE_MTX, a);
    f32 m03 = 0.0f;
    bool moved = false;
    if (pillar != 0 && (a == A_LEFT || a == A_RIGHT) && guest_obj(self)) {
        m03 = guest_f32(self + PANE_MTX + MTX_M03);
        guest_set_f32(self + PANE_MTX + MTX_M03, m03 + (f32)(a == A_LEFT ? -pillar : pillar));
        moved = true;
    }

    func_802cd2ec(cpu);

    if (moved) guest_set_f32(self + PANE_MTX + MTX_M03, m03);
}

// ── The draws that bypass the quad emitter ───────────────────────────────────────────────
// GXLoadPosMtxImm call sites (cpu.lr) inside the TGCConsole2 FLUDD-gauge draws. Each loads an
// identity position matrix and then draws part of the gauge in the 2D ortho, so shifting that
// matrix moves everything that draw emits out to the right edge with the rest of the gauge.
//   0x8014421c  drawWater (the water level)
//   0x8014487c  drawJuice
//   0x8014930c  drawWaterBack (the blue swirl circle behind it)
constexpr u32 kGaugePosMtxLR[] = {0x8014421cu, 0x8014487cu, 0x8014930cu};

// J2DPicture::draw (0x802ccef4) loads each picture's own matrix (this+0x54) at this site. The
// nozzle icon, "WATER" label and the health sun come through here rather than the emitter, so they
// are anchored here — per element, by the name at this+0x10 (= mtx - 0x54).
constexpr u32 J2DPIC_DRAW_POSMTX_LR = 0x802ccfc0u;

// TSunGlass::draw (0x8017d354) draws the full-screen fade/darkening curtain as ONE quad over a 4:3
// rect with an identity matrix. Under the 2D squeeze that quad covers only the centre 75%, leaving
// the 16:9 side strips undarkened during every fade. Un-squeeze it to the whole screen: map
// x in [0,640] to the full width, i.e. m00 = 1/scale and m03 = -pillar. A scale AND a translate,
// unlike the gauge's pure translate.
constexpr u32 TSUNGLASS_POSMTX_LR = 0x8017d3ecu;

void ov_load_pos_mtx_imm(CPUState& cpu) {
    const int pillar = sbr_ws_pillar();
    if (pillar == 0) { func_80362e0c(cpu); return; }   // widescreen off: stay out of a hot path

    const u32 lr = cpu.lr, mtx = cpu.gpr[3];

    if (lr == TSUNGLASS_POSMTX_LR && guest_obj(mtx)) {
        const f32 k = inv_scale();
        const f32 m00 = guest_f32(mtx + MTX_M00), m03 = guest_f32(mtx + MTX_M03);
        guest_set_f32(mtx + MTX_M00, m00 * k);
        guest_set_f32(mtx + MTX_M03, m03 - (f32)pillar);
        func_80362e0c(cpu);
        guest_set_f32(mtx + MTX_M00, m00);
        guest_set_f32(mtx + MTX_M03, m03);
        return;
    }

    int shift = 0;
    for (u32 site : kGaugePosMtxLR)
        if (lr == site) { shift = pillar; break; }

    if (shift == 0 && lr == J2DPIC_DRAW_POSMTX_LR && mtx >= 0x80000054u && mtx < 0x81800000u) {
        char nm[5];
        read_fourcc(mtx - 0x54, nm);
        const HudAnchor a = hud_anchor(nm);
        note_pane(nm, mtx, a);   // this path draws the panes the quad emitter never sees
        if (a == A_LEFT)  shift = -pillar;
        if (a == A_RIGHT) shift =  pillar;
    }

    f32 m03 = 0.0f;
    bool moved = false;
    if (shift != 0 && guest_obj(mtx)) {
        m03 = guest_f32(mtx + MTX_M03);
        guest_set_f32(mtx + MTX_M03, m03 + (f32)shift);
        moved = true;
    }

    func_80362e0c(cpu);

    if (moved) guest_set_f32(mtx + MTX_M03, m03);
}

} // namespace

SB_OVERRIDE(QUAD_EMITTER, ov_quad, "J2D 2D quad emitter",
            "widescreen HUD: anchor each element to its real 16:9 edge by .blo name")
SB_OVERRIDE(GX_LOAD_POS_MTX_IMM, ov_load_pos_mtx_imm, "GXLoadPosMtxImm",
            "widescreen HUD: anchor the gauge draws that bypass the quad emitter, keyed by call "
            "site, and stretch the fade curtain to the full screen")
