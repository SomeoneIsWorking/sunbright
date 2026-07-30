// lerp60 — see the header, and debug_journal/2026-07-30_aurora_60fps_lerp_design.md.

#include "lerp60.h"

#include <lucent/log.h>

#include <cstdlib>

// Aurora's tag latch and its coverage counters. Declared here rather than pulled in through a
// public aurora header: these are diagnostics for OUR emitter, and the recomp links aurora
// statically so the symbols resolve directly.
namespace aurora::gx::fifo {
extern uint64_t g_pendingDrawTag;
extern long g_taggedDrawCount;
extern long g_untaggedDrawCount;
extern long g_untaggedOrthoDrawCount;
extern long g_untaggedPerspDrawCount;
extern long g_untaggedPerspDirectCount;
extern long g_untaggedPerspIndexedCount;
} // namespace aurora::gx::fifo

bool sbr_lerp_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_LERP60");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        if (v == 1)
            lucent::info("lerp60", "interpolated 60fps ARMED — aurora is no longer bit-identical to "
                                   "the oracle path; A/B numbers taken with this on are not "
                                   "comparable to numbers taken with it off");
    }
    return v == 1;
}

void sbr_lerp_report_tag_coverage() {
    if (!sbr_lerp_enabled()) return;
    const long tagged = aurora::gx::fifo::g_taggedDrawCount;
    const long untagged = aurora::gx::fifo::g_untaggedDrawCount;
    const long total = tagged + untagged;
    // REFUSE rather than report a percentage of nothing. "0 of 0 tagged" and "the emitter is
    // broken" produce the same 0% otherwise, and a 0% that means "no draws happened" reads as a
    // catastrophic tagging failure.
    if (total == 0) {
        lucent::warn("lerp60", "tag coverage: NO DRAWS REACHED AURORA AT ALL (0 tagged, 0 "
                               "untagged). This says nothing about tagging — there was nothing to "
                               "tag. Check the run is actually rendering before reading this.");
        return;
    }
    const long ortho = aurora::gx::fifo::g_untaggedOrthoDrawCount;
    const long persp = aurora::gx::fifo::g_untaggedPerspDrawCount;
    lucent::info("lerp60", "tag coverage: {} of {} draws carried an identity ({:.1f}%); {} did not "
                           "and will SNAP rather than interpolate.",
                 tagged, total, 100.0 * (double)tagged / (double)total, untagged);
    // The split is the part that carries information. An untagged ORTHOGRAPHIC draw snapping is
    // correct — a screen-space element has no meaningful in-between. An untagged PERSPECTIVE draw
    // snapping is world geometry stuttering inside an otherwise smooth frame, and that number is
    // the honest size of the remaining gap rather than a percentage that merely looks plausible.
    lucent::info("lerp60", "  of the untagged: {} orthographic (correct to snap — 2D/HUD) and {} "
                           "PERSPECTIVE ({:.1f}% of all draws). Perspective draws that snap are "
                           "world geometry stuttering in a smooth frame — that count is the gap, "
                           "and it is only acceptable at 0 or for genuinely per-tick geometry "
                           "(particles, immediate-mode effects).",
                 ortho, persp, 100.0 * (double)persp / (double)total);
    // Split again, because "perspective and untagged" still bundles two very different things. Only
    // the INDEXED count is a defect: direct-mode positions are rebuilt by the CPU every tick and
    // have no cross-tick identity to have, so snapping is the only correct behaviour for them.
    const long direct = aurora::gx::fifo::g_untaggedPerspDirectCount;
    const long indexed = aurora::gx::fifo::g_untaggedPerspIndexedCount;
    lucent::info("lerp60", "    of those: {} DIRECT (immediate-mode, rebuilt per tick — correct to "
                           "snap, no identity exists to give them) and {} INDEXED ({:.1f}% of all "
                           "draws). The INDEXED count is the actual defect: display-list geometry "
                           "from a persistent vertex array HAS a stable identity and should be "
                           "interpolating, so each one is a tag seam not yet covered.",
                 direct, indexed, 100.0 * (double)indexed / (double)total);
}
