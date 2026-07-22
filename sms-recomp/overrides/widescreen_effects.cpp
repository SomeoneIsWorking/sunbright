// widescreen_effects.cpp — the draws that the 2D squeeze gets wrong, and why each one differs.
//
// Ported from the retired Dolphin-era overrides (git 9283f44^: fader_widescreen.cpp,
// fillrect_widescreen.cpp, screenfx_widescreen.cpp, efbtex_widescreen.cpp), where the RE was done.
//
// widescreen.cpp squeezes every 2D ortho so the game's 0..640 plane presents as the CENTRE 4:3 of
// the 16:9 picture. Two families of draw are wrong under that rule, for opposite reasons:
//
//   FULL-SCREEN 2D — fades, wipes, the dash-blur quad. Authored to fill the 4:3 display rect, so
//   after the squeeze they cover only the middle and leave the side thirds untouched: a fade to
//   black that leaves the scene visible in two vertical strips. These are widened.
//
//   OFFSCREEN PASSES — the EFB->texture passes and the mirror pre-render. Their output is consumed
//   through a matrix built from the UNsqueezed camera, or copied back 1:1 into a texture, so the
//   render and the lookup must agree. Squeezing the render but not the lookup shifts the result
//   ~25% toward the texture centre. These are EXEMPTED rather than widened.
//
// The second family is not cosmetic. The graffiti/pollution counting passes render the pollution
// maps and count lit pixels with GXReadPixMetric; squeezed, they count ~25% fewer, so goop coverage
// percentages — and the game events they trigger — are simply wrong.

#include "overrides.h"

#include "../runtime/probe_server.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

extern "C" void func_8013fa54(CPUState&);   // TSMSFader::drawFadeinout
extern "C" void func_8013fc88(CPUState&);   // TSMSFader::draw
extern "C" void func_80140390(CPUState&);   // GC2D fill_rect
extern "C" void func_802f8bac(CPUState&);   // JDrama::TEfbCtrlTex::perform
extern "C" void func_802f8904(CPUState&);   // JDrama::TEfbCtrlDisp::perform
extern "C" void func_80193fbc(CPUState&);   // TMirrorCamera::perform
extern "C" void func_8022d4f8(CPUState&);   // TAfterEffect::perform
extern "C" void func_801aa6cc(CPUState&);   // TBathWaterManager::draw_mist

// widescreen.cpp
extern bool g_ws_2d_suspend;
extern bool g_ws_persp_suspend;
void ws_2d_suspend_begin(CPUState&);
void ws_2d_suspend_end(CPUState&);
int  sbr_ws_pillar();

namespace {

constexpr u32 FADER_DRAW_FADEINOUT = 0x8013fa54u;
constexpr u32 FADER_DRAW           = 0x8013fc88u;
constexpr u32 FILL_RECT            = 0x80140390u;
constexpr u32 EFBCTRLTEX_PERFORM   = 0x802f8bacu;
constexpr u32 EFBCTRLDISP_PERFORM  = 0x802f8904u;
constexpr u32 MIRRORCAM_PERFORM    = 0x80193fbcu;
constexpr u32 AFTEREFFECT_PERFORM  = 0x8022d4f8u;
constexpr u32 BATH_DRAW_MIST       = 0x801aa6ccu;

bool widescreen_on() { return sbr_ws_pillar() != 0; }

// JDrama::TRect (JUTRect) is s32 x1, y1, x2, y2.
constexpr u32 RECT_X1 = 0x0, RECT_X2 = 0x8;

// These wrappers NEST: TSMSFader::draw widens the rect and then calls drawFadeinout, which is also
// hooked, and both eventually reach fill_rect. Widening at each level compounds — measured as the
// same rect arriving at fill_rect as both -107..747 and -250..890. The Dolphin era did not see this
// because JIT block-linking kept the inner calls from reaching their overrides at all; here every
// call goes through call_ppc, so only the OUTERMOST wrapper may widen.
int g_widen_depth = 0;

struct WidenScope {
    WidenScope() { ++g_widen_depth; }
    ~WidenScope() { --g_widen_depth; }
    bool outermost() const { return g_widen_depth == 1; }
};

// A 16:9 picture shows 4/3 the horizontal range of the 4:3 ortho plane, i.e. w/6 more per side.
void with_widened_rect(CPUState& cpu, u32 rectReg, void (*real)(CPUState&)) {
    const u32 rect = rectReg;
    const WidenScope scope;
    if (!widescreen_on() || !scope.outermost() || !sb_ram_fast(rect)) { real(cpu); return; }
    const s32 x1 = (s32)sb_r32(rect + RECT_X1), x2 = (s32)sb_r32(rect + RECT_X2);
    const s32 extra = (x2 - x1) / 6 + 1;
    sb_w32(rect + RECT_X1, (u32)(x1 - extra));
    sb_w32(rect + RECT_X2, (u32)(x2 + extra));
    real(cpu);
    sb_w32(rect + RECT_X1, (u32)x1);   // the caller's rect is its own state; always restore
    sb_w32(rect + RECT_X2, (u32)x2);
}

// TSMSFader fills the caller's TRect with a GX quad. Widening the rect is enough here:
// drawFadeinout draws its own quad rather than going through fill_rect.
void ov_fader_draw_fadeinout(CPUState& cpu) { with_widened_rect(cpu, cpu.gpr[4], func_8013fa54); }

// TSMSFader::draw also carries the hx_wiper circle-wipe curtain, which draws 0..640 geometry with
// the CURRENT ortho — so the rect widening is not enough and the ortho itself must be unsqueezed
// for the duration, or the wipe shows pillars at the sides.
void ov_fader_draw(CPUState& cpu) {
    if (!widescreen_on()) { func_8013fc88(cpu); return; }
    ws_2d_suspend_begin(cpu);
    with_widened_rect(cpu, cpu.gpr[4], func_8013fc88);
    ws_2d_suspend_end(cpu);
}

// Live inventory of the solid bands the game fills (probe /fills). "Which rect is the black strip
// behind the dialogue" is not answerable from a screenshot; this reports every distinct rect
// fill_rect was asked for, so a band can be matched to its authored extents.
std::map<std::string, unsigned long> g_fills;

void note_fill(u32 rect) {
    char key[40];
    std::snprintf(key, sizeof key, "%d,%d,%d,%d", (int)(s32)sb_r32(rect + 0x0), (int)(s32)sb_r32(rect + 0x4),
                  (int)(s32)sb_r32(rect + 0x8), (int)(s32)sb_r32(rect + 0xC));
    ++g_fills[key];
}

const bool g_fill_probe = [] {
    sb_probe_register("/fills", "distinct fill_rect rects seen: x1,y1,x2,y2 -> count", [](const ProbeArgs&) {
        std::string out;
        char buf[96];
        for (const auto& [k, n] : g_fills) {
            std::snprintf(buf, sizeof buf, "%-28s %lu\n", k.c_str(), n);
            out += buf;
        }
        if (out.empty()) out = "no fills recorded yet\n";
        return out;
    });
    return true;
}();

// GC2D's shared fill_rect draws solid bands — including the stage-name telop backdrop and the
// dialogue band. Those are authored full-display (x1 <= 0, x2 >= 640); under the squeeze the band
// stops at the 4:3 edges with the scene visible beside it. PARTIAL fills (wipe boxes, dialog
// boxes that are meant to be inset) must not be touched, hence the full-width test rather than a
// blanket widen.
void ov_fill_rect(CPUState& cpu) {
    const u32 rect = cpu.gpr[3];
    if (!widescreen_on() || !sb_ram_fast(rect)) { func_80140390(cpu); return; }
    note_fill(rect);
    const s32 x1 = (s32)sb_r32(rect + RECT_X1), x2 = (s32)sb_r32(rect + RECT_X2);
    const bool full_width = (x1 <= 0 && x1 > -40 && x2 >= 600 && x2 < 700);
    if (!full_width) { func_80140390(cpu); return; }
    with_widened_rect(cpu, rect, func_80140390);
}

// The scene graph runs offscreen render-to-EFB-then-copy passes bracketed by TEfbCtrlTex::perform:
// flag 0x80 opens the pass, 0x8 closes it with the GXCopyTex. Everything between is drawn in
// TEXTURE pixel space, 1:1 with the copy source rect, so those orthos must reach the GP verbatim.
void ov_efbctrltex_perform(CPUState& cpu) {
    const u32 flags = cpu.gpr[4];
    if (!widescreen_on()) { func_802f8bac(cpu); return; }
    if (flags & 0x80) {
        g_ws_2d_suspend = true;
        lucent::debug("widescreen", "EFB->texture pass open — 2D squeeze suspended");
    }
    func_802f8bac(cpu);
    if (flags & 0x8) g_ws_2d_suspend = false;
}

// The screen display pass beginning is the safety net: a pass that somehow never closes must not
// leak its suspend into visible 2D, which would shift the whole screen.
void ov_efbctrldisp_perform(CPUState& cpu) {
    if (widescreen_on() && (cpu.gpr[4] & 0x80)) {
        g_ws_2d_suspend = false;
        g_ws_persp_suspend = false;
    }
    func_802f8904(cpu);
}

// The mirror pre-render draws the scene into a 256x256 texture with a PERSPECTIVE projection, and
// the main pass samples it through a projective texture matrix built from the same, unsqueezed,
// camera parameters. Squeezing only the render slides the reflection ~25% toward the texture
// centre. Offscreen perspective consumed through camera-derived matrices stays 4:3.
void ov_mirrorcam_perform(CPUState& cpu) {
    if (!widescreen_on()) { func_80193fbc(cpu); return; }
    const bool prev = g_ws_persp_suspend;
    g_ws_persp_suspend = true;
    func_80193fbc(cpu);
    g_ws_persp_suspend = prev;
}

// TAfterEffect is the dash-blur / screen-flash overlay: a fan over the viewport in screen coords,
// textured with a half-res capture of the screen. No rect edit is needed — the quad already covers
// the full viewport, and stretching the capture across the full width is CORRECT, because the
// capture is of the anamorphic 16:9 EFB. It only needs the ortho unsqueezed.
void ov_aftereffect_perform(CPUState& cpu) {
    const u32 self = cpu.gpr[3], flags = cpu.gpr[4];
    // Mirror the game's own early-outs: only the draw pass (0x10), with the effect enabled
    // (unk14 bit 0), emits the quad. Otherwise this would churn projections every frame.
    const bool draws = widescreen_on() && (flags & 0x10) && sb_ram_fast(self) && (sb_r8(self + 0x14) & 1);
    if (!draws) { func_8022d4f8(cpu); return; }
    ws_2d_suspend_begin(cpu);
    func_8022d4f8(cpu);
    ws_2d_suspend_end(cpu);
}

// draw_mist copies the EFB viewport region to a texture and redraws it over the SAME region through
// its own EFB-pixel ortho. The copy source is the real (anamorphic) EFB, so the replay must be the
// identity mapping: squeezing it would shrink the redraw to the centre 3/4 and misalign it against
// the copy. Flag only, no ortho reload — draw_mist issues its own projection.
void ov_bath_draw_mist(CPUState& cpu) {
    if (!widescreen_on()) { func_801aa6cc(cpu); return; }
    const bool prev = g_ws_2d_suspend;
    g_ws_2d_suspend = true;
    func_801aa6cc(cpu);
    g_ws_2d_suspend = prev;
}

} // namespace

SB_OVERRIDE(FADER_DRAW_FADEINOUT, ov_fader_draw_fadeinout, "TSMSFader::drawFadeinout",
            "widescreen: fades must cover the whole picture, not the centre 4:3")
SB_OVERRIDE(FADER_DRAW, ov_fader_draw, "TSMSFader::draw",
            "widescreen: fades and the circle wipe must span the whole picture")
SB_OVERRIDE(FILL_RECT, ov_fill_rect, "GC2D fill_rect",
            "widescreen: stretch full-width bands (telop/dialogue) to the real edges")
SB_OVERRIDE(EFBCTRLTEX_PERFORM, ov_efbctrltex_perform, "JDrama::TEfbCtrlTex::perform",
            "widescreen: EFB->texture passes are texture-space and must NOT be squeezed — "
            "squeezing them miscounts pollution coverage")
SB_OVERRIDE(EFBCTRLDISP_PERFORM, ov_efbctrldisp_perform, "JDrama::TEfbCtrlDisp::perform",
            "widescreen: clear any dangling pass suspend before visible 2D")
SB_OVERRIDE(MIRRORCAM_PERFORM, ov_mirrorcam_perform, "TMirrorCamera::perform",
            "widescreen: the mirror pre-render is sampled through unsqueezed camera matrices")
SB_OVERRIDE(AFTEREFFECT_PERFORM, ov_aftereffect_perform, "TAfterEffect::perform",
            "widescreen: the dash-blur quad must cover the whole picture")
SB_OVERRIDE(BATH_DRAW_MIST, ov_bath_draw_mist, "TBathWaterManager::draw_mist",
            "widescreen: an EFB copy replayed over its own region must stay identity-mapped")
