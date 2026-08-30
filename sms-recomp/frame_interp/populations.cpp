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

#include "populations.h"
#include "../overrides/overrides.h"

#include <intrinsics.h>

extern "C" void func_801dc34c(CPUState&); // TMapObjFlag::draw
extern "C" void func_801dd21c(CPUState&); // TMapObjWave::draw
extern "C" void func_80225d00(CPUState&); // SMS_DrawCube
bool sbr_lerp_enabled();
void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
uint64_t sbr_shadow_cube_tag(const CPUState& cpu);

namespace {

// Scoped so a nested draw cannot leave the label set on whatever follows it — the same discipline
// the tagging seams follow, and for the same reason: an inherited label files another system's
// draws under this one, which is a wrong answer that reads exactly like a right one.
struct Scope {
    bool on;
    explicit Scope(u8 pop) : on(sbr_lerp_enabled()) {
        if (on)
            sbr_gxfifo_draw_pop(pop);
    }
    ~Scope() {
        if (on)
            sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
    }
};

// A DEFORMING object gets an identity as well as a label. Its mesh is rebuilt every tick, so the
// vertex path — not any matrix — is what interpolates it, and that path needs a stable key to find
// the previous tick's vertices. `this` (r3) is a real per-instance object: a flag and a wave grid
// are persistent actors, not pooled per-tick records, so this is the sound kind of key rather than
// the ordinals this project has had to withdraw twice.
//
// The vertex-count gate lives in patch_vertices: a mesh rebuilt at a different resolution has no
// correspondence and snaps rather than smearing between two unrelated shapes.
struct Deforming {
    bool on;
    Deforming(u8 pop, u32 self)
        : on(sbr_lerp_enabled() && self != 0 && sbr_gxfifo_pending_tag() == 0) {
        if (on) {
            sbr_gxfifo_draw_pop(pop);
            sbr_gxfifo_draw_tag((uint64_t)self << 32 | 1u);
        }
    }
    ~Deforming() {
        if (on) {
            sbr_gxfifo_draw_tag(0);
            sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
        }
    }
};

void ov_flag(CPUState& cpu) {
    Deforming d(SB_POP_FLAG, (u32)cpu.gpr[3]);
    func_801dc34c(cpu);
}
void ov_wave(CPUState& cpu) {
    Deforming d(SB_POP_WAVE, (u32)cpu.gpr[3]);
    func_801dd21c(cpu);
}
// The alpha-restore cube is no longer label-only: tag_shadow can give it the identity of the shadow
// GROUP it bounds (sbr_shadow_cube_tag — the key is the group's MEMBERSHIP, so a re-clustered group
// snaps instead of pairing with a different set of actors).
void ov_cube(CPUState& cpu) {
    Scope s(SB_POP_DRAW_CUBE);
    const uint64_t tag = sbr_gxfifo_pending_tag() == 0 ? sbr_shadow_cube_tag(cpu) : 0;
    if (tag != 0)
        sbr_gxfifo_draw_tag(tag);
    func_80225d00(cpu);
    if (tag != 0)
        sbr_gxfifo_draw_tag(0);
}

} // namespace

SB_OVERRIDE(0x801dc34cu, ov_flag, "TMapObjFlag::draw",
            "60fps audit label only: deforming cloth, immediate mode; no identity is given")
SB_OVERRIDE(0x801dd21cu, ov_wave, "TMapObjWave::draw",
            "60fps audit label only: the sea ripple grid, rebuilt per tick")
SB_OVERRIDE(0x80225d00u, ov_cube, "SMS_DrawCube",
            "60fps audit label only: the shadow pass's alpha-restore cube")
