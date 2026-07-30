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
    lucent::info("lerp60", "tag coverage: {} of {} draws carried an identity ({:.1f}%); {} did not "
                           "and will SNAP rather than interpolate. Untagged is correct for 2D/HUD, "
                           "particles and immediate geometry — it is a defect for anything drawn "
                           "through J3DShape::draw.",
                 tagged, total, 100.0 * (double)tagged / (double)total, untagged);
}
