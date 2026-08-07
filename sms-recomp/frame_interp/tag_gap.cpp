// tag_gap.cpp — WHICH guest code draws geometry that interpolation cannot pair.
//
// THE GAP THIS EXISTS TO CLOSE. A draw interpolates only if it carries a cross-tick IDENTITY, which
// j3d_capture.cpp emits at J3DShape::draw as (shape, instance). Measured over a plaza run, 69.9% of
// draws carry one. Of the rest: 831k orthographic (correct to snap — 2D has no in-between), 1.18M
// perspective DIRECT (immediate-mode, rebuilt by the CPU every tick, so no identity exists to give
// them), and 929k perspective INDEXED — 9.5% of all draws.
//
// That last group is the defect. Indexed means the positions come from a persistent vertex array
// through a display list, so the geometry HAS a stable identity across ticks; it simply is not being
// given one. Those draws fall through to patch_camera_only and receive the camera delta alone, which
// is right for static scenery and wrong for anything that moves in the world: such an object follows
// the camera but not its own motion, so it snaps in object space inside an otherwise smooth frame.
// Mario's shadow and the dash-trail ghost are both reported juddery and both are candidates.
//
// WHY ATTRIBUTION HAS TO HAPPEN HERE. aurora classifies a draw when it PARSES the recorded stream,
// which happens at frame-send time — long after the guest code that emitted it has returned, so the
// host stack no longer names the emitter and no amount of unwinding there can recover it. The
// identity has to be captured while the guest is still executing. GXCallDisplayList is the narrow
// waist: indexed geometry reaches the FIFO through it, and its caller's return address names the
// system responsible.
//
// This is OBSERVE-ONLY and costs a histogram insert on display-list calls that are already untagged.
// It changes no rendering.
//
//   SBR_TAGGAP=1   report the untagged display-list callers, worst first

#include "../overrides/overrides.h"
#include "populations.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <vector>

extern "C" void func_80362a50(CPUState&);   // GXCallDisplayList
extern "C" void func_802dfe88(CPUState&);   // J3DShapeDraw::draw() const
extern "C" void func_8035df88(CPUState&);   // GXBegin(GXPrimitive, GXVtxFmt, u16)
uint64_t sbr_gxfifo_pending_tag();
void sbr_gxfifo_draw_tag(uint64_t tag);
void gxfifo_stats(u64& draws, u64& verts, u64& bytes);
void gxfifo_drain_pending();
u64 sbr_shine_shadow_next_tag();

namespace {

bool enabled() {
    static const bool v = std::getenv("SBR_TAGGAP") != nullptr;
    return v;
}

// Keyed by the CALLER's return address, which is what names the system. Keyed by nothing else on
// purpose: a per-call-site count is directly actionable (each one is a seam to tag or a seam to
// prove static), where a per-object count would only say how much geometry is affected.
//
// DRAWS, NOT CALLS — and the difference is not pedantry. The first version of this counted
// GXCallDisplayList CALLS and its headline was that J3DDisplayListObj::callDL accounted for 61.9%
// of the gap. But callDL is what J3DMaterial uses to replay a MATERIAL display list
// (J3DMaterial.cpp:841) — register writes, zero primitives. Counting calls and reasoning about the
// 9.5%-of-DRAWS gap is comparing two populations that are not the same quantity, which is the exact
// instrument failure this project has paid for six times. So each site records both, and the draw
// column is the one that answers the question.
//
// The count is exact at the call boundary because the fifo is DRAINED either side: the recomp's
// parser batches (it only re-parses at 4 KB) so without the drain a call's primitives would be
// credited to whichever site happened to trip the threshold.
struct Site {
    unsigned long calls = 0;
    unsigned long draws = 0;
};
std::unordered_map<u32, Site> g_bySite;
// SECOND LEVEL. The site histogram names the function that called GXCallDisplayList, which for the
// dominant entry is J3DShapeDraw::draw itself — true, and not yet actionable: it says the geometry
// came through J3D's shape-draw object without saying which system was drawing. This records one
// frame further up, so the answer is a system rather than a J3D internal.
std::unordered_map<u32, unsigned long> g_shapeDrawCallers;
unsigned long g_shapeDrawUntagged = 0, g_shapeDrawTagged = 0;

// ── IMMEDIATE-MODE (DIRECT) DRAWS ───────────────────────────────────────────────────────────────
//
// The display-list gap is closed, and the remaining untagged population is 1,056,734 DIRECT draws
// per run, which the coverage line calls "rebuilt per tick — correct to snap, no identity exists to
// give them". That sentence is an ASSUMPTION and it was never checked. A user watching the plaza
// fountain jitter is the counter-example: whatever draws it is in this population, it plainly has a
// stable identity tick to tick (it is the same fountain), and "no identity exists" was a statement
// about how the geometry is SUBMITTED, not about whether the object persists.
//
// GXBegin is the waist for immediate geometry the way GXCallDisplayList is for indexed, so the same
// attribution works: the caller names the system. Whether those draws can be interpolated is a
// separate question — their vertices are rebuilt each tick, so a matrix lerp does nothing and the
// VERTEX DATA would have to be paired and lerped — but naming them is the prerequisite for asking.
std::unordered_map<u32, unsigned long> g_directSites;
unsigned long g_directUntagged = 0, g_directTagged = 0;
unsigned long g_untagged = 0, g_tagged = 0;
unsigned long g_untaggedDraws = 0, g_taggedDraws = 0;

void report() {
    const unsigned long total = g_tagged + g_untagged;
    // REFUSE rather than print a share of nothing: "0% untagged" from a run that drew no display
    // lists and "0% untagged" from full coverage are the same number with opposite meanings.
    if (total == 0) {
        lucent::warn("taggap", "NO display list was called at all this run, so this says nothing "
                               "about tag coverage. Check the run is rendering before reading it.");
        return;
    }
    std::vector<std::pair<u32, Site>> v(g_bySite.begin(), g_bySite.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.draws > b.second.draws; });
    lucent::info("taggap",
                 "display lists with NO draw tag in force: {} of {} calls ({:.1f}%), and {} of {} "
                 "DRAWS ({:.1f}%). The DRAW column is the one that matters — a material display "
                 "list is a call that emits no primitive at all. Untagged draws cannot pair across "
                 "ticks, so they take the camera delta alone: correct for static scenery, and "
                 "object-space judder for anything that moves. {} distinct call site(s), worst by "
                 "DRAWS first:",
                 g_untagged, total, 100.0 * (double)g_untagged / (double)total, g_untaggedDraws,
                 g_untaggedDraws + g_taggedDraws,
                 (g_untaggedDraws + g_taggedDraws)
                     ? 100.0 * (double)g_untaggedDraws / (double)(g_untaggedDraws + g_taggedDraws)
                     : 0.0,
                 v.size());
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        lucent::info("taggap",
                     "    caller 0x{:08x}  {:>9} draw(s) ({:.1f}% of untagged draws)  from {} call(s)",
                     v[i].first, v[i].second.draws,
                     g_untaggedDraws ? 100.0 * (double)v[i].second.draws / (double)g_untaggedDraws : 0.0,
                     v[i].second.calls);
    }
    // One frame further up for the dominant site, with its own denominator.
    std::vector<std::pair<u32, unsigned long>> sd(g_shapeDrawCallers.begin(), g_shapeDrawCallers.end());
    std::sort(sd.begin(), sd.end(), [](auto& a, auto& b) { return a.second > b.second; });
    lucent::info("taggap",
                 "  who calls J3DShapeDraw::draw WITHOUT a tag: {} of {} calls untagged; {} distinct "
                 "caller(s):",
                 g_shapeDrawUntagged, g_shapeDrawUntagged + g_shapeDrawTagged, sd.size());
    for (size_t i = 0; i < sd.size() && i < 10; ++i) {
        lucent::info("taggap", "      0x{:08x}  {:>9} call(s)  {:.1f}%", sd[i].first, sd[i].second,
                     g_shapeDrawUntagged ? 100.0 * (double)sd[i].second / (double)g_shapeDrawUntagged
                                         : 0.0);
    }
    std::vector<std::pair<u32, unsigned long>> dv(g_directSites.begin(), g_directSites.end());
    std::sort(dv.begin(), dv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    lucent::info("taggap",
                 "  IMMEDIATE-MODE (GXBegin) with no tag: {} of {} calls; {} distinct site(s). "
                 "These are the draws the coverage line calls 'correct to snap' — an assumption, "
                 "not a measurement:",
                 g_directUntagged, g_directUntagged + g_directTagged, dv.size());
    for (size_t i = 0; i < dv.size() && i < 12; ++i) {
        lucent::info("taggap", "      0x{:08x}  {:>9} call(s)  {:.1f}%", dv[i].first, dv[i].second,
                     g_directUntagged ? 100.0 * (double)dv[i].second / (double)g_directUntagged : 0.0);
    }
    lucent::info("taggap", "  Resolve these with: python3 tools/re/addr2sym.py <addr>...");
}

void ov_call_display_list(CPUState& cpu) {
    // SHINE-SHADOW SLICE TAGGING. Runs whether or not the attribution diagnostic is on, because it
    // is a behaviour fix rather than a measurement: this is the one seam that sees each individual
    // replay of the volume's sphere display list, and tag_shadow.cpp cannot tag from its own frame
    // because the function is entered once and draws many slices.
    if (sbr_gxfifo_pending_tag() == 0) {
        const u64 shine = sbr_shine_shadow_next_tag();
        if (shine != 0) {
            sbr_gxfifo_draw_pop(SB_POP_SHADOW_SHINE);
            sbr_gxfifo_draw_tag(shine);
            func_80362a50(cpu);
            sbr_gxfifo_draw_tag(0);
    sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
            return;
        }
    }
    if (!enabled()) {
        func_80362a50(cpu);
        return;
    }
    const bool untagged = sbr_gxfifo_pending_tag() == 0;
    u64 d0 = 0, v0 = 0, b0 = 0, d1 = 0, v1 = 0, b1 = 0;
    gxfifo_drain_pending();
    gxfifo_stats(d0, v0, b0);

    func_80362a50(cpu);

    gxfifo_drain_pending();
    gxfifo_stats(d1, v1, b1);
    const unsigned long drew = (unsigned long)(d1 - d0);

    if (untagged) {
        ++g_untagged;
        g_untaggedDraws += drew;
        auto& site = g_bySite[(u32)cpu.lr];
        ++site.calls;
        site.draws += drew;
    } else {
        ++g_tagged;
        g_taggedDraws += drew;
    }
    static unsigned long n = 0;
    if ((++n % 20000) == 0) report();
}

void ov_shape_draw_draw(CPUState& cpu) {
    if (enabled()) {
        if (sbr_gxfifo_pending_tag() == 0) {
            ++g_shapeDrawUntagged;
            ++g_shapeDrawCallers[(u32)cpu.lr];
        } else {
            ++g_shapeDrawTagged;
        }
    }
    func_802dfe88(cpu);
}

void ov_gx_begin(CPUState& cpu) {
    if (enabled()) {
        if (sbr_gxfifo_pending_tag() == 0) {
            ++g_directUntagged;
            ++g_directSites[(u32)cpu.lr];
        } else {
            ++g_directTagged;
        }
    }
    func_8035df88(cpu);
}

} // namespace

SB_OVERRIDE(0x8035df88u, ov_gx_begin, "GXBegin",
            "60fps (SBR_TAGGAP): attribute IMMEDIATE-MODE draws that carry no interpolation "
            "identity; observe-only, always runs the real body")

SB_OVERRIDE(0x802dfe88u, ov_shape_draw_draw, "J3DShapeDraw::draw",
            "60fps (SBR_TAGGAP): name the SYSTEM behind the dominant untagged-draw site; "
            "observe-only, always runs the real body")

SB_OVERRIDE(0x80362a50u, ov_call_display_list, "GXCallDisplayList",
            "60fps (SBR_TAGGAP): attribute display-list draws that carry no interpolation identity "
            "to the guest function that emitted them; observe-only, always runs the real body")
