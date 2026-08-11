// tag_particle.cpp — interpolate JPA particles (the plaza fountain, and every other billboard).
//
// WHY THIS IS NOT "JUST TAG IT". A JPA billboard's draw bakes the particle's position into the
// VERTEX stream (decomp JSystem/JParticle/JPADrawVisitor.cpp):
//
//     particle->getGlobalPosition(pt);
//     MTXMultVec(dc->pcb->mViewMtx, &pt, &pt);          // world -> EYE SPACE, on the CPU
//     GXBegin(GX_QUADS, GX_VTXFMT0, 4);
//     GXPosition3f32(offs[0].x + pt.x, offs[0].y + pt.y, pt.z);
//
// so there is no per-particle position matrix. Giving the draw a cross-tick identity and stopping
// there would be WORSE than leaving it alone: patch_draw would pair it and lerp identity against
// identity, a no-op that also suppresses the camera delta the draw currently gets for free.
//
// What makes it cheap anyway: `offs[]` is the quad's SHAPE and `pt` is one point added to every
// corner, so between two ticks the quad is the same shape DISPLACED. A billboard translates; it
// does not deform. And the position matrix is identity and otherwise unused, so the entire
// correction fits in it — no vertex data is touched, which matters because aurora pushes RAW GX
// FIFO BYTES (command_processor.cpp) and lerping those would mean decoding each draw's vertex
// format, and merged draws would no longer map to one object anyway.
//
// So: tag the draw, record the particle's WORLD position, and let aurora apply
// translate(-(1-alpha) * (P_cur - P_prev)) composed with the camera delta. World rather than eye
// space is deliberate — an eye-space pair lives in two different view transforms and its difference
// would fold camera motion into the particle's own.
//
// THE IDENTITY, and the hazard the shadow work already paid for. JPABaseParticle* is a real object
// address, but particles are POOLED: a dead particle's address is handed to a new one, and pairing
// across that reuse lerps an unrelated particle's position — the marukage teleport again.
//
// FLAG_JUST_BORN (status bit 0x1) was the obvious signal and it is USELESS HERE: measured, 0 bumps
// over 517,119 draws across 268 addresses, because the flag is set at creation and cleared during
// the particle's update, which runs before the draw pass. A zero that means "the flag is always
// clear by the time I look" is indistinguishable from "no particle was ever born" — it was only
// caught because 268 addresses serving half a million draws obviously implies reuse.
//
// mAge is the sound signal: it increases monotonically for a given particle, so for the same
// address a NON-INCREASE means a different particle. That is a definitive reuse test rather than a
// threshold. A particle that skipped a tick still has an increasing age and is handled anyway —
// patch_billboard requires consecutive tick stamps, so a gap does not pair regardless.
//
//   SBR_TAGPARTICLE=0   disable (the draws revert to the camera delta alone)

#include "../overrides/overrides.h"
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>

void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
bool sbr_lerp_enabled();

namespace aurora::gfx::interp {
void set_tag_world_pos(uint64_t tag, float x, float y, float z);
}

namespace {

// JPABaseParticle (decomp JSystem/JParticle/JPAParticle.hpp).
constexpr u32 PART_GLOBAL = 0x2C;   // JGeometry::TVec3<f32> mGlobalPosition
constexpr u32 PART_AGE    = 0x44;   // f32 mAge

bool enabled() {
    static const bool v = [] {
        const char* e = std::getenv("SBR_TAGPARTICLE");
        return e == nullptr || e[0] != '0';
    }();
    return v;
}

// Per-address generation, bumped when the game says the particle at that address is newly born.
// Without it a pooled address pairs two unrelated particles and the new one flies in from wherever
// the old one died — the same defect as the marukage teleport, with the same visible signature.
struct Slot {
    u32 generation = 0;
    float lastAge = -1.0f;
};
std::unordered_map<u32, Slot> g_slots;
unsigned long g_tagged = 0, g_born = 0;

float guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void tag_and_run(CPUState& cpu, void (*body)(CPUState&)) {
    // r5 = JPABaseParticle* (r3 = this, r4 = const JPADrawContext*).
    const u32 particle = (u32)cpu.gpr[5];
    const bool ok = enabled() && sbr_lerp_enabled() && particle != 0 &&
                    sbr_gxfifo_pending_tag() == 0 && sb_ram_fast(particle + PART_AGE);
    uint64_t tag = 0;
    if (ok) {
        Slot& slot = g_slots[particle];
        const float age = guest_f32(particle + PART_AGE);
        if (!(age > slot.lastAge)) {
            ++slot.generation;   // this address is a DIFFERENT particle than the one seen before
            ++g_born;
        }
        slot.lastAge = age;
        tag = ((uint64_t)particle << 32) | (uint64_t)slot.generation;
        aurora::gfx::interp::set_tag_world_pos(tag, guest_f32(particle + PART_GLOBAL),
                                               guest_f32(particle + PART_GLOBAL + 4),
                                               guest_f32(particle + PART_GLOBAL + 8));
        sbr_gxfifo_draw_pop(SB_POP_PARTICLE);
        sbr_gxfifo_draw_tag(tag);
        ++g_tagged;
    }
    body(cpu);
    if (tag != 0) sbr_gxfifo_draw_tag(0);
    sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
}

} // namespace

// ── EVERY PER-PARTICLE DRAW VISITOR, not the two that happened to be in the fountain ────────────
//
// The first version hooked BillBoard and RotBillBoard, and the registry then showed
// JPADrawExecRotDirectional, JPADrawExecDirBillBoard and friends drawing camera-only in the same
// plaza — the same defect, in the same subsystem, missed because nobody enumerated the visitors.
//
// The generalisation is sound rather than convenient, and this is why: JPA has one shape for every
// per-particle visitor. It builds `offs[]` — the quad's (or the cross's, or the point's) SHAPE —
// and adds ONE point to every corner, and that point is `particle->getGlobalPosition(pt)` in all
// nine of them (checked in decomp/sms JPADrawVisitor.cpp, not assumed). So each one is a shape
// DISPLACED by the particle's world position, which is exactly the case patch_billboard exists for.
//
// The variants differ in whether `pt` reaches the vertices in EYE space (BillBoard, DirBillBoard —
// MTXMultVec by the view first) or in WORLD space (RotDirectional, Rotation — the offsets are
// rotated into world space instead). Both are correct under the same patch, and it is worth being
// explicit about why, because "it worked for billboards" is not a reason:
//
//   eye space   the draw matrix is identity, so the patch's composed matrix is the camera delta,
//               and adding the eye-space displacement V_lerp*dw to its translation column
//               translates the already-eye-space vertices by exactly that.
//   world space the draw matrix is the view, so the composed matrix is ~V_lerp, and adding
//               V_lerp*dw to its translation column IS V_lerp * translate_world(dw), since
//               [R|t] * T(dw) = [R | R*dw + t].
//
// Same correction, both spaces, because in both the wanted operation is "rotate the world
// displacement into eye space and translate".
//
// TWO ARE MISSING and are named rather than silently omitted: JPADrawExecLine::exec and
// JPADrawExecRotYBillBoard::exec are absent from reference/sms_gmse01_funcs.txt (it omits weak
// methods), so their addresses are not known here. If either ever draws, the graphics registry will
// show it as an unlabelled site with a `?` symbol inside that gap — which is the detection working,
// not a hole in it.
//
// The emitter-level visitors (Stripe, StripeCross) are deliberately NOT here: they draw a whole
// particle CHAIN as one strip, so there is no single particle whose position could displace it.
// Interpolating those needs the vertex path, and their registry rows say so.
#define SB_JPA_VISITOR(hexaddr, name)                                                          \
    extern "C" void func_##hexaddr(CPUState&);                                                 \
    namespace {                                                                                \
    void ov_jpa_##hexaddr(CPUState& cpu) { tag_and_run(cpu, func_##hexaddr); }                 \
    }                                                                                          \
    SB_OVERRIDE(0x##hexaddr##u, ov_jpa_##hexaddr, name,                                        \
                "60fps: identity + world position for a per-particle JPA draw, so the "        \
                "particle's own motion interpolates as a translation (its position is baked "  \
                "into the vertex stream, not carried in a matrix)")

SB_JPA_VISITOR(8033025c, "JPADrawExecBillBoard::exec")
SB_JPA_VISITOR(80330434, "JPADrawExecRotBillBoard::exec")
SB_JPA_VISITOR(80330650, "JPADrawExecYBillBoard::exec")
SB_JPA_VISITOR(80330d8c, "JPADrawExecDirectional::exec")
SB_JPA_VISITOR(803311a0, "JPADrawExecRotDirectional::exec")
SB_JPA_VISITOR(803315fc, "JPADrawExecDirectionalCross::exec")
SB_JPA_VISITOR(80331b54, "JPADrawExecRotDirectionalCross::exec")
SB_JPA_VISITOR(803320f4, "JPADrawExecDirBillBoard::exec")
SB_JPA_VISITOR(80332444, "JPADrawExecRotation::exec")
SB_JPA_VISITOR(8033266c, "JPADrawExecRotationCross::exec")
SB_JPA_VISITOR(803329d8, "JPADrawExecPoint::exec")

void sbr_tag_particle_report() {
    if (!enabled() || !sbr_lerp_enabled()) return;
    lucent::info("taggap",
                 "particles: {} billboard draw(s) tagged and positioned, {} pooled-address reuse(s) "
                 "detected by a non-increasing age, {} distinct address(es) seen{}",
                 g_tagged, g_born, g_slots.size(),
                 g_tagged == 0
                     ? "   <-- NONE. Either no particle drew in this scene or the hook never fired; "
                       "those are different answers and this line cannot tell them apart."
                     : "");
}


