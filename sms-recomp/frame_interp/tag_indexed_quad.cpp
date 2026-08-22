// tag_indexed_quad.cpp — interpolate SMS's two persistent indexed quad batches.
//
// Retail TDLTexQuad::draw and TDLColorTexQuad::draw bind a double-buffered XYZ-f32 position array
// and issue one display-list draw. Question marks, splash droplets and water-spray refraction
// rebuild those eye-space vertices once per 30 Hz tick while drawing with an identity position
// matrix. Pairing identity matrices cannot move them; their indexed arrays must be interpolated.

#include "../overrides/overrides.h"
#include "populations.h"

#include <dolphin/gx/GXAuroraControl.h>
#include <intrinsics.h>

extern "C" void func_80224f0c(CPUState&); // TDLColorTexQuad::draw
extern "C" void func_80225408(CPUState&); // TDLTexQuad::draw

bool sbr_lerp_enabled();
void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
u8 sbr_gxfifo_pending_pop();

namespace {
constexpr uint64_t kColorKind = 0x54444c43u; // "TDLC"
constexpr uint64_t kTexKind = 0x54444c54u;   // "TDLT"

void draw_indexed_quad(CPUState& cpu, void (*retail)(CPUState&), uint64_t kind) {
    const u32 self = cpu.gpr[3];
    // unk8 is the active quad count. A marker with no following draw would leak onto unrelated
    // geometry, so the same retail precondition gates the bracket.
    if (!sbr_lerp_enabled() || self == 0 || MEM_R16(self + 8) == 0) {
        retail(cpu);
        return;
    }

    const uint64_t previousTag = sbr_gxfifo_pending_tag();
    const u8 previousPopulation = sbr_gxfifo_pending_pop();
    const uint64_t tag = static_cast<uint64_t>(self) << 32 | kind;
    sbr_gxfifo_draw_pop(SB_POP_TDL_QUAD);
    // A reserved tag payload is an ordered one-shot control, not an identity. The immediately
    // following real tag remains the stable key seen by the draw.
    sbr_gxfifo_draw_tag(GX_AURORA_DRAW_TAG_INDEXED_DEFORM);
    sbr_gxfifo_draw_tag(tag);
    retail(cpu);
    sbr_gxfifo_draw_tag(previousTag);
    sbr_gxfifo_draw_pop(previousPopulation);
}

void ov_color(CPUState& cpu) {
    draw_indexed_quad(cpu, func_80224f0c, kColorKind);
}
void ov_tex(CPUState& cpu) {
    draw_indexed_quad(cpu, func_80225408, kTexKind);
}
} // namespace

SB_OVERRIDE(0x80224f0cu, ov_color, "TDLColorTexQuad::draw",
            "interpolate its persistent indexed XYZ-f32 position array between simulation ticks")
SB_OVERRIDE(0x80225408u, ov_tex, "TDLTexQuad::draw",
            "interpolate its persistent indexed XYZ-f32 position array between simulation ticks")
