// populations.cpp — LABEL-ONLY seams for the interpolation audit.
//
// These hooks give their draws no identity and change no rendering. They exist so that every draw
// in a frame is attributed to a named system, because the audit's value is entirely in the
// distinction between "this population snaps, and that is correct" and "this population snaps and
// nobody has looked at it". Those two are the same number in any global percentage.
//
// Each seam here is a population the attribution pass (SBR_TAGGAP=1) already identified by its
// caller address, promoted from a one-off measurement into a permanent row in the report.
//
// Both J2DPicture entries are deliberately absent: J2DPicture::draw is already hooked by
// overrides/diag_2d.cpp (2D class census) and drawTexCoord by overrides/hud.cpp (the widescreen
// quad emitter). One address gets exactly one override — the registry REFUSES a second rather than
// silently replacing the first, and it caught both attempts here. Their labels are applied from
// those hooks instead, which is the correct outcome: a label is a line of code, not a hook.

#include "../overrides/overrides.h"
#include "populations.h"

#include <intrinsics.h>

extern "C" void func_801dc34c(CPUState&);   // TMapObjFlag::draw
extern "C" void func_801dd21c(CPUState&);   // TMapObjWave::draw
extern "C" void func_80225d00(CPUState&);   // SMS_DrawCube
extern "C" void func_802f1b00(CPUState&);   // JUTResFont::drawChar_scale
bool sbr_lerp_enabled();

namespace {

// Scoped so a nested draw cannot leave the label set on whatever follows it — the same discipline
// the tagging seams follow, and for the same reason: an inherited label files another system's
// draws under this one, which is a wrong answer that reads exactly like a right one.
struct Scope {
    bool on;
    explicit Scope(u8 pop) : on(sbr_lerp_enabled()) {
        if (on) sbr_gxfifo_draw_pop(pop);
    }
    ~Scope() {
        if (on) sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
    }
};

void ov_flag(CPUState& cpu)      { Scope s(SB_POP_FLAG);      func_801dc34c(cpu); }
void ov_wave(CPUState& cpu)      { Scope s(SB_POP_WAVE);      func_801dd21c(cpu); }
void ov_cube(CPUState& cpu)      { Scope s(SB_POP_DRAW_CUBE); func_80225d00(cpu); }
void ov_text(CPUState& cpu)      { Scope s(SB_POP_TEXT);      func_802f1b00(cpu); }

} // namespace

SB_OVERRIDE(0x801dc34cu, ov_flag, "TMapObjFlag::draw",
            "60fps audit label only: deforming cloth, immediate mode; no identity is given")
SB_OVERRIDE(0x801dd21cu, ov_wave, "TMapObjWave::draw",
            "60fps audit label only: the sea ripple grid, rebuilt per tick")
SB_OVERRIDE(0x80225d00u, ov_cube, "SMS_DrawCube",
            "60fps audit label only: the shadow pass's alpha-restore cube")
SB_OVERRIDE(0x802f1b00u, ov_text, "JUTResFont::drawChar_scale",
            "60fps audit label only: text glyphs")
