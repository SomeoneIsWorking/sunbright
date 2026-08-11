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
//   SBR_TAGWIRE=0   disable both (they revert to the camera delta alone)

#include "../overrides/overrides.h"
#include "graphics_db.h"
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

extern "C" void func_80198278(CPUState&);   // TMapWire::drawUpper() const
extern "C" void func_801983a8(CPUState&);   // TMapWire::drawLower() const — unnamed in funcs.txt
extern "C" void func_8027cc2c(CPUState&);   // TModelWaterManager::drawMirror(MtxPtr)

void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
void sbr_gxfifo_draw_pop(u8 pop);
bool sbr_lerp_enabled();
void sbr_gxbegin_set_hook(void (*fn)());

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

// One scope for both seams. It gives every primitive of one guest call a tag of (object, primitive
// index) and labels them all with the caller's population.
struct Scope {
    bool on;
    Scope(u8 pop, u32 self, unsigned long& calls, void (*counter)())
        : on(enabled() && sbr_lerp_enabled() && self != 0 && sbr_gxfifo_pending_tag() == 0) {
        if (on) {
            ++calls;
            g_self = self;
            g_strip = 0;
            g_stripCount = counter;
            sbr_gxfifo_draw_pop(pop);
            sbr_gxbegin_set_hook(&on_gx_begin);
        }
    }
    ~Scope() {
        if (on) {
            sbr_gxbegin_set_hook(nullptr);
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
}

SB_OVERRIDE(0x80198278u, ov_draw_upper, "TMapWire::drawUpper",
            "60fps: identity for the wire's upper strip so its DEFORMING vertices interpolate; the "
            "rope's motion is in its vertices, not in any matrix")
SB_OVERRIDE(0x801983a8u, ov_draw_lower, "TMapWire::drawLower",
            "60fps: identity per strip for the wire's two lower strips (same vertex count, so they "
            "must not be allowed to pair against each other)")
SB_OVERRIDE(0x8027cc2cu, ov_draw_mirror, "TModelWaterManager::drawMirror",
            "60fps: identity per fan for the water-mirror mask, which is rebuilt every tick around "
            "Mario's position and so moves with him")
