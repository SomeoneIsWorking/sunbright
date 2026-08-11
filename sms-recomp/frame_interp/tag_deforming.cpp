// tag_deforming.cpp — interpolate CPU-rebuilt geometry that emits SEVERAL primitives per call.
//
// Two seams live here, found by the graphics registry as camera-only draws and fixed the same way,
// because they are the same problem: TMapWire (the rope Mario swings on) and
// TModelWaterManager::drawMirror (the water-mirror mask fan around Mario).
//
// WHAT THEY ARE. TMapWire is the rope Mario hangs from and swings on. Its geometry is a triangle strip
// built by the CPU every tick from `mMapWirePoints[i].mPosition` (decomp/sms src/Map/MapWire.cpp),
// with the whole rope sagging and swinging while he moves along it — so this is DEFORMING
// immediate-mode geometry, in exactly the sense that flags and the sea ripple are. No matrix
// carries its motion, which is why it took the camera delta alone and snapped in object space
// inside an otherwise smooth frame. The vertex path is the only thing that reaches it.
//
// THREE STRIPS, NOT ONE, and this is the part that needed care. The wire draws through two
// functions: drawUpper emits ONE strip, drawLower emits TWO (the rope's under-face and its far
// face). All three have the same vertex count, (mNumActiveMapWirePoints + 2) * 2, so a tag keyed on
// the wire object alone would let this tick's strip pair against the PREVIOUS tick's *other* strip —
// same count, so the count gate would not catch it — and smear the rope between two of its own
// faces. That is the marukage teleport again, and it is worse than snapping.
//
// TWO COUNTERS, TWO POPULATIONS — read the report with this in mind. The counts below are GUEST
// PRIMITIVES (one per GXBegin); the interpolation audit counts GPU DRAWS, and aurora merges
// consecutive primitives that share state into one. Measured on two independent seams: the wire
// emits 1,779 primitives and the audit sees 1,184 draws (drawLower's two strips merge), and the
// cone beam emits 7,008 fans for 3,492 draws (each Aux call's two fans merge). Neither is a loss;
// they are different quantities, and this project has paid six times for comparing two of those.
//
// It also means the strip index is usually not load-bearing: merged primitives are ONE draw with
// one tag, so they cannot pair against each other anyway. It stays because the merge is a property
// of the state they happen to share — a future state change between two strips splits them again,
// and then the index is the only thing standing between a rope and its own other face.
//
// So the tag carries a STRIP INDEX as well as the object. The index is an ordinal, which this
// project has had to withdraw twice, and the difference here is what makes it sound rather than
// convenient: both of drawLower's strips are UNCONDITIONAL and emitted in a fixed order in straight-
// line code with no branch between them (MapWire.cpp:47-85). The ordinal is over a population of
// exactly two, fixed by the compiler, not over a set that varies with what the scene is doing. The
// ordinals that failed before were over draw counts and shadow slots, which vary per frame.
//
// drawLower is ABSENT from reference/sms_gmse01_funcs.txt (it is a weak const method and the list
// omits those). Its address comes from the recompiler's own function table, which is what the
// graphics registry symbolizes against for the same reason — the registry reported it as
// `sub_801983a8 (in drawUpper__8TMapWireCFv)`, which is how this seam was found at all.
//
// THE MIRROR IS THE SAME SHAPE OF PROBLEM. drawMirror ends with two unconditional 10-vertex
// TRIANGLEFANs (ModelWaterManager.cpp:1303,1337) built around SMS_GetMarioPos() in world space, so
// they move with Mario and are rebuilt every tick — deforming geometry with two same-sized
// primitives per call, which is exactly the wire's situation. It gets the same treatment from the
// same code rather than a second copy of it.
//
// AND THE PARTICLE STRIPES, for the same structural reason. JPADrawExecStripe and
// JPADrawExecStripeCross are EMITTER-level visitors: they draw a whole particle chain as one strip,
// so no single particle's position can displace them and the per-particle seam in tag_particle.cpp
// cannot cover them (which is why the registry still showed StripeCross camera-only after every
// per-particle visitor was hooked). The chain is rebuilt every tick — deforming geometry again.
//
// THE IDENTITY IS THE EMITTER, and its one weakness is stated rather than hidden: emitters are
// heap objects and an address can be reused, exactly the hazard that made the particle seam key on
// (address, generation) with mAge as the reuse signal. JPABaseEmitter has no equivalent monotonic
// field, so there is no generation to add. What bounds it instead is the pairing gate itself:
// patch_vertices requires the two samples to be from CONSECUTIVE ticks, so an alias would need one
// emitter to die and another to take the same address WITHIN one tick and draw a chain of exactly
// the same length — the vertex-count gate covers the rest. That is a much narrower window than the
// particle pool's, which recycles addresses continuously by design.
//
// TWO MORE, FOUND THE SAME WAY — by playing a stage the registry had never seen. Running Gelato
// (stage 4) and stage 6 added seven rows, two of them camera-only:
//
//   TConeBeam::drawConeBeam    the light-shaft cone. drawConeBeamAux emits TWO triangle fans of
//                              mVtxCount+2 from a vertex array calcVertices rebuilds every tick,
//                              and drawConeBeam calls it three times unconditionally plus a fourth
//                              time when unk1C is set (decomp/sms src/Enemy/beam.cpp). Every fan
//                              draws the SAME vertices under different render state, so fan k this
//                              tick and fan k last tick are the same geometry — and because the
//                              conditional call is LAST, indices 0..5 are a stable prefix and the
//                              optional 6,7 simply have nothing to pair with on a tick where they
//                              did not draw.
//
//   TSwingBoard::drawOneRope   the swinging platform's ropes. It emits exactly ONE primitive
//                              (verified by disassembly: a single GXBegin in the whole function),
//                              and TSwingBoard::draw calls it TWICE from two fixed call sites. So
//                              the rope's identity is (board, call site) — two structural keys, no
//                              ordinal — and it is self-checked the same way Mario's cubes are: a
//                              site seen drawing twice for one board in one tick has its premise
//                              broken and loses its tag for the run, loudly.
//
//   TMapObjGrassGroup::drawNear  the grass. One GX_TRIANGLES primitive of unk68*3 vertices, rebuilt
//                              every tick: each blade's middle vertex carries a sway offset from
//                              the manager's 10-entry animation table, and the outer two are
//                              displaced along a camera-facing width vector (MapObjGrass.cpp:30).
//                              Deforming, so the vertex path is the only thing that reaches it.
//
//                              ONLY drawNear is hooked, and that is deliberate twice over. It is
//                              the f32 variant — drawFar emits GXPosition3s16, which the vertex
//                              path does not handle (it requires direct f32 positions), so tagging
//                              it would buy nothing. And drawFar's address is not written here
//                              because no run has yet seen it draw; when one does, the registry
//                              gets a row and the row is what adds the entry.
//
//                              The LOD switch needs no special handling: a group that goes far
//                              stops being tagged, and patch_vertices refuses a pair whose ticks
//                              are not consecutive, so coming back near cannot pair across the gap.
//
//                              GRASS IS THE EXTREME CASE OF THE MERGE described above: every group
//                              draws with identical state, so a measured 3,516 primitives become
//                              292 draws — twelve groups per tick collapsing into one. The merged
//                              draw carries the LAST group's tag and the whole merged vertex range
//                              is lerped as a unit, which is correct only while the merged SET is
//                              the same from tick to tick. What guards that is the vertex-count
//                              gate: a group joining or leaving changes the total and the draw
//                              snaps. It is not a complete guard — one group leaving as another of
//                              the same blade count joins would slip through — and that residual is
//                              recorded here rather than left for someone to rediscover.
//
//   SBR_TAGWIRE=0   disable all of them (they revert to the camera delta alone)

#include "../overrides/overrides.h"
#include "frame_interp.h"
#include "graphics_db.h"
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <unordered_map>

extern "C" void func_80198278(CPUState&);   // TMapWire::drawUpper() const
extern "C" void func_801983a8(CPUState&);   // TMapWire::drawLower() const — unnamed in funcs.txt
extern "C" void func_8027cc2c(CPUState&);   // TModelWaterManager::drawMirror(MtxPtr)
extern "C" void func_80332c34(CPUState&);   // JPADrawExecStripe::exec(const JPADrawContext*)
extern "C" void func_803330a4(CPUState&);   // JPADrawExecStripeCross::exec(const JPADrawContext*)
extern "C" void func_800def6c(CPUState&);   // TConeBeam::drawConeBeam(const GXColor&)
extern "C" void func_801f383c(CPUState&);   // TSwingBoard::drawOneRope(const TVec3&, const TVec3&)
extern "C" void func_801e99a8(CPUState&);   // TMapObjGrassGroup::drawNear() const — unnamed in funcs.txt

void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
void sbr_gxfifo_draw_pop(u8 pop);
bool sbr_lerp_enabled();
void (*sbr_gxbegin_set_hook(void (*fn)()))();

namespace {

bool enabled() {
    static const bool v = [] {
        const char* e = std::getenv("SBR_TAGWIRE");
        return e == nullptr || e[0] != '0';
    }();
    return v;
}

u32 g_self = 0;        // the object being drawn, 0 when no such draw is in progress
unsigned g_strip = 0;  // which primitive of that object is about to be emitted
unsigned long g_strips = 0, g_upperCalls = 0, g_lowerCalls = 0, g_mirrorCalls = 0;
unsigned long g_mirrorStrips = 0;
unsigned long g_stripeCalls = 0, g_stripeStrips = 0, g_stripeNoEmitter = 0;
unsigned long g_beamCalls = 0, g_beamFans = 0;
unsigned long g_grassCalls = 0, g_grassPrims = 0;
unsigned long g_ropeTagged = 0, g_ropeWithdrawn = 0;

uint64_t tag_for(u32 self, unsigned strip) {
    // Strip index in the low bits, object in the high: the same shape the flag and the sea ripple
    // use, extended by the one thing they do not need.
    return ((uint64_t)self << 32) | (uint64_t)(strip + 1);
}

// Called by the GXBegin seam (tag_gap.cpp) for every immediate-mode primitive while a wire draw is
// in progress. This is the only point that can tell drawLower's two strips apart: they are two
// primitives inside one guest call, so no function-level hook can see the boundary between them.
void (*g_stripCount)() = nullptr;

void on_gx_begin() {
    if (g_self == 0) return;
    sbr_gxfifo_draw_tag(tag_for(g_self, g_strip));
    ++g_strip;
    if (g_stripCount != nullptr) g_stripCount();
}

void count_wire() { ++g_strips; }
void count_mirror() { ++g_mirrorStrips; }
void count_stripe() { ++g_stripeStrips; }
void count_beam() { ++g_beamFans; }
void count_grass() { ++g_grassPrims; }

// JPADrawContext::mBaseEmitter is the first member (JPADrawVisitor.hpp:30 — `pcb` is static and
// takes no space). r4 is the context: these are virtual methods, so r3 is the visitor singleton,
// which is the same object for every emitter and would key every stripe in the scene alike.
u32 emitter_of(const CPUState& cpu) {
    const u32 dc = (u32)cpu.gpr[4];
    if (dc == 0 || !sb_ram_fast(dc)) return 0;
    return sb_r32(dc);
}

// One scope for both seams. It gives every primitive of one guest call a tag of (object, primitive
// index) and labels them all with the caller's population.
struct Scope {
    bool on;
    void (*prevHook)() = nullptr;
    Scope(u8 pop, u32 self, unsigned long& calls, void (*counter)())
        : on(enabled() && sbr_lerp_enabled() && self != 0 && sbr_gxfifo_pending_tag() == 0) {
        if (on) {
            ++calls;
            g_self = self;
            g_strip = 0;
            g_stripCount = counter;
            sbr_gxfifo_draw_pop(pop);
            prevHook = sbr_gxbegin_set_hook(&on_gx_begin);
        }
    }
    ~Scope() {
        if (on) {
            sbr_gxbegin_set_hook(prevHook);
            g_stripCount = nullptr;
            g_self = 0;
            sbr_gxfifo_draw_tag(0);
            sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
        }
    }
};

void ov_draw_upper(CPUState& cpu) {
    Scope s(SB_POP_WIRE, (u32)cpu.gpr[3], g_upperCalls, &count_wire);
    func_80198278(cpu);
}

void ov_draw_lower(CPUState& cpu) {
    Scope s(SB_POP_WIRE, (u32)cpu.gpr[3], g_lowerCalls, &count_wire);
    func_801983a8(cpu);
}

void ov_draw_mirror(CPUState& cpu) {
    Scope s(SB_POP_MIRROR, (u32)cpu.gpr[3], g_mirrorCalls, &count_mirror);
    func_8027cc2c(cpu);
}

void ov_grass_near(CPUState& cpu) {
    Scope s(SB_POP_GRASS, (u32)cpu.gpr[3], g_grassCalls, &count_grass);
    func_801e99a8(cpu);
}

void ov_cone_beam(CPUState& cpu) {
    Scope s(SB_POP_CONEBEAM, (u32)cpu.gpr[3], g_beamCalls, &count_beam);
    func_800def6c(cpu);
}

// ONE ROPE, ONE PRIMITIVE, so no strip counter — the identity is the board and the call site that
// asked for this rope. Self-checking: the premise is that a site draws one rope per board per tick,
// and a second draw for the same pair in one tick means the premise is false.
struct RopeKey {
    uint64_t lastTick = 0;
    unsigned drawsThisTick = 0;
    bool trusted = true;
};
std::unordered_map<uint64_t, RopeKey> g_ropes;

void ov_one_rope(CPUState& cpu) {
    const u32 self = (u32)cpu.gpr[3];
    const u32 site = (u32)cpu.lr;
    uint64_t tag = 0;
    const bool eligible = enabled() && sbr_lerp_enabled() && self != 0 &&
                          sbr_gxfifo_pending_tag() == 0;
    if (eligible) {
        // Both halves are structural: the board instance and the site in TSwingBoard::draw that
        // draws this particular rope. Folded rather than concatenated because both are 32-bit and
        // the tag is 64 — the fold keeps every bit of the object and enough of the site to separate
        // two call sites eight instructions apart.
        const uint64_t key = ((uint64_t)self << 32) ^ ((uint64_t)site << 3);
        RopeKey& rk = g_ropes[key];
        const uint64_t tick = sb::frame_interp::sim_tick_seq();
        if (tick != rk.lastTick) {
            rk.lastTick = tick;
            rk.drawsThisTick = 0;
        }
        if (++rk.drawsThisTick > 1 && rk.trusted) {
            rk.trusted = false;
            ++g_ropeWithdrawn;
            lucent::warn("taggap", "TSwingBoard rope key (board 0x{:08x}, site 0x{:08x}) drew twice "
                                   "in one tick, so it is not the one-rope-per-site identity it "
                                   "assumed. Withdrawn for the rest of the run — those ropes take "
                                   "the camera delta rather than pairing with each other.",
                         self, site);
        }
        if (rk.trusted) {
            tag = key | 1u;
            sbr_gxfifo_draw_pop(SB_POP_ROPE);
            sbr_gxfifo_draw_tag(tag);
            ++g_ropeTagged;
        }
    }
    func_801f383c(cpu);
    if (tag != 0) {
        sbr_gxfifo_draw_tag(0);
        sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
    }
}

void ov_stripe(CPUState& cpu) {
    const u32 emitter = emitter_of(cpu);
    if (emitter == 0) ++g_stripeNoEmitter;
    Scope s(SB_POP_STRIPE, emitter, g_stripeCalls, &count_stripe);
    func_80332c34(cpu);
}

void ov_stripe_cross(CPUState& cpu) {
    const u32 emitter = emitter_of(cpu);
    if (emitter == 0) ++g_stripeNoEmitter;
    Scope s(SB_POP_STRIPE, emitter, g_stripeCalls, &count_stripe);
    func_803330a4(cpu);
}

} // namespace

void sbr_tag_wire_report() {
    if (!enabled() || !sbr_lerp_enabled()) return;
    // THE INVARIANT, stated exactly rather than approximately. drawUpper emits ONE strip and
    // drawLower emits TWO, both unconditionally, so the strip count is determined by the call
    // counts — and a mismatch means the GXBegin hook missed a primitive, which would leave strips
    // pairing on a tag that belongs to another strip of the same wire.
    //
    // The first version of this check asserted strips >= calls*2, which is simply not the shape of
    // the code, and it fired on a perfectly healthy run (879 strips over 586 calls, where 293
    // upper + 293 lower gives exactly 879). A check that flags correct behaviour trains its reader
    // to ignore it, so it is worth more than a comment to get it right.
    const unsigned long expect = g_upperCalls + g_lowerCalls * 2;
    lucent::info("taggap", "wires: {} strip(s) tagged over {} drawUpper + {} drawLower call(s){}",
                 g_strips, g_upperCalls, g_lowerCalls,
                 (g_upperCalls + g_lowerCalls) == 0
                     ? "   <-- NONE. No wire drew in this scene, or the hook never fired; this line "
                       "cannot tell those apart, so do not read it as 'the plaza has no wires'."
                 : g_strips != expect
                     ? "   <-- MISMATCH: the calls imply exactly that many strips. A shortfall means "
                       "the GXBegin hook did not see every primitive, and the strips it missed are "
                       "carrying another strip's tag."
                     : "");
    lucent::info("taggap", "water mirror: {} fan(s) tagged over {} drawMirror call(s){}",
                 g_mirrorStrips, g_mirrorCalls,
                 g_mirrorCalls == 0
                     ? "   <-- NONE, which for a run with Mario on screen means the hook never "
                       "fired rather than that the mirror does not draw."
                 : g_mirrorStrips != g_mirrorCalls * 2
                     ? "   <-- MISMATCH: drawMirror ends with exactly two fans, so anything else "
                       "means the hook missed primitives (or the function took an early return "
                       "this build does not know about)."
                     : "");
    lucent::info("taggap",
                 "grass: {} primitive(s) tagged over {} drawNear call(s){}",
                 g_grassPrims, g_grassCalls,
                 g_grassCalls == 0
                     ? " (no near grass drew — stage-dependent; drawFar is s16 and deliberately not "
                       "hooked, so a stage of distant grass legitimately reports zero here)"
                     : "");
    lucent::info("taggap",
                 "cone beams: {} fan(s) tagged over {} drawConeBeam call(s){}; swing-board ropes: "
                 "{} tagged, {} key(s) withdrawn for drawing twice in a tick",
                 g_beamFans, g_beamCalls,
                 g_beamCalls == 0 ? " (none drew — stage-dependent, not evidence the seam works)"
                                  : "",
                 g_ropeTagged, g_ropeWithdrawn);
    lucent::info("taggap",
                 "particle stripes: {} strip(s) tagged over {} chain draw(s); {} call(s) had no "
                 "readable emitter and were left alone{}",
                 g_stripeStrips, g_stripeCalls, g_stripeNoEmitter,
                 g_stripeCalls == 0
                     ? "   <-- NONE drew this run, which is scene-dependent (stripes are trails and "
                       "chains); it is not evidence the seam works."
                     : "");
}

SB_OVERRIDE(0x80198278u, ov_draw_upper, "TMapWire::drawUpper",
            "60fps: identity for the wire's upper strip so its DEFORMING vertices interpolate; the "
            "rope's motion is in its vertices, not in any matrix")
SB_OVERRIDE(0x801983a8u, ov_draw_lower, "TMapWire::drawLower",
            "60fps: identity per strip for the wire's two lower strips (same vertex count, so they "
            "must not be allowed to pair against each other)")
SB_OVERRIDE(0x801e99a8u, ov_grass_near, "TMapObjGrassGroup::drawNear",
            "60fps: identity for a grass group's swaying blades, whose vertices are rebuilt every "
            "tick from the manager's sway table")
SB_OVERRIDE(0x800def6cu, ov_cone_beam, "TConeBeam::drawConeBeam",
            "60fps: identity per fan for the light-shaft cone, whose vertices calcVertices rebuilds "
            "every tick")
SB_OVERRIDE(0x801f383cu, ov_one_rope, "TSwingBoard::drawOneRope",
            "60fps: identity for one swing-board rope, keyed by (board, call site) because the "
            "board draws exactly one rope from each of two fixed sites")
SB_OVERRIDE(0x80332c34u, ov_stripe, "JPADrawExecStripe::exec",
            "60fps: identity per strip for a particle CHAIN, which is emitter-level geometry no "
            "per-particle position can displace")
SB_OVERRIDE(0x803330a4u, ov_stripe_cross, "JPADrawExecStripeCross::exec",
            "60fps: identity per strip for a crossed particle chain, same reason as Stripe")
SB_OVERRIDE(0x8027cc2cu, ov_draw_mirror, "TModelWaterManager::drawMirror",
            "60fps: identity per fan for the water-mirror mask, which is rebuilt every tick around "
            "Mario's position and so moves with him")
