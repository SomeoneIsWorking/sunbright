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

extern "C" void func_8033025c(CPUState&);   // JPADrawExecBillBoard::exec(const JPADrawContext*, JPABaseParticle*)
extern "C" void func_80330434(CPUState&);   // JPADrawExecRotBillBoard::exec(...)
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

void ov_billboard(CPUState& cpu) { tag_and_run(cpu, func_8033025c); }
void ov_rot_billboard(CPUState& cpu) { tag_and_run(cpu, func_80330434); }

} // namespace

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

SB_OVERRIDE(0x8033025cu, ov_billboard, "JPADrawExecBillBoard::exec",
            "60fps: identity + world position for a billboard particle, so its own motion "
            "interpolates as a translation (its position is baked into the vertex stream)")
SB_OVERRIDE(0x80330434u, ov_rot_billboard, "JPADrawExecRotBillBoard::exec",
            "60fps: identity + world position for a rotating billboard particle")
