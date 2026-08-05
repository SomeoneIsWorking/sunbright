// diag_tick_split.cpp — SBR_TICK_SPLIT=1: where does a game tick's wall time actually go?
//
// WHY THIS EXISTS. The 60fps interpolation question is "what does a SECOND draw pass cost?", and
// that cannot be answered from a leaf profile: sample shares tell you which functions are hot, not
// which PHASE of the tick they were serving. A sub-frame re-runs PreEntry + the draw lists and does
// NOT re-run movement, physics or AI, so the marginal cost is the draw-side phases alone. Estimating
// that from percentages is arithmetic on a guess; this measures it.
//
// WHERE IT HOOKS, AND WHY NOT THE FUNNEL. The first version wrapped JDrama::TViewObj::testPerform
// (0x802fcc94), the dispatch funnel. Its own control killed it: with the override built in but
// SBR_TICK_SPLIT unset the frame cost ~30 ms/present, and with it set ~63 ms. testPerform runs 3,623
// times per tick and the wrapper's bookkeeping lands INSIDE the outermost timed window, so the
// instrument roughly doubled frame time and inflated the very shares it was reporting. An instrument
// that perturbs its subject by 2x cannot price a sub-frame.
//
// So it hooks TPerformList::perform (0x802a4e28) instead — the whole-list-per-phase dispatch, about
// ten calls per tick. Each call IS one phase over one list, which is exactly the unit the question is
// asked in ("what do the draw-side lists cost?"), and at ~10 calls/tick two clock_gettime calls are
// unmeasurable.
//
// NESTING. Perform lists can contain perform lists, so naive timing would count the same nanoseconds
// once per level. Only the OUTERMOST call is timed; its duration is inclusive of everything below it,
// and the buckets stay non-overlapping.
//
// NO TAXONOMY GUESS. Calls are bucketed by their EXACT phase mask rather than sorted into
// "movement"/"draw" by a hand-written if-chain. A mask carrying several bits would be silently
// mis-filed by such a chain, and the phase-bit semantics have already misled this project once (a
// hook gated on `(mask & link->unk8) & 1` never fired). The masks that actually occur are reported
// with their own times, so the taxonomy is read off the data.
//
// DECLARES ITS BLIND SPOT. The report always prints what fraction of wall-clock the timed calls
// account for. Work outside the perform lists — movement(), the present itself, TStrategy and the
// manager-driven dispatches that do not go through TPerformList — is NOT covered, and a bucket table
// summing to well under 100% must say so rather than read as a complete account of the tick.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <ctime>

extern "C" void func_802a4e28(CPUState&);      // TPerformList::perform(u32, JDrama::TGraphics*)
extern "C" unsigned VIGetRetraceCount(void);   // this runtime's present counter

namespace {

bool enabled() {
    static const bool on = std::getenv("SBR_TICK_SPLIT") != nullptr;
    return on;
}

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Report cadence, in presents.
constexpr unsigned REPORT_EVERY = 300;

constexpr int MAX_MASKS = 96;
struct Bucket {
    u32    mask  = 0;
    double ms    = 0.0;
    long   calls = 0;
};
Bucket g_bucket[MAX_MASKS];
int    g_nMasks = 0;
bool   g_maskOverflow = false;   // more distinct masks than slots -- say so, never silently drop

int     g_depth      = 0;
long    g_outerCalls = 0;
long    g_totalCalls = 0;
int64_t g_windowStart = 0;
unsigned g_lastReport = 0;

void account(u32 mask, double ms) {
    for (int i = 0; i < g_nMasks; ++i) {
        if (g_bucket[i].mask == mask) {
            g_bucket[i].ms += ms;
            ++g_bucket[i].calls;
            return;
        }
    }
    if (g_nMasks < MAX_MASKS) {
        g_bucket[g_nMasks].mask  = mask;
        g_bucket[g_nMasks].ms    = ms;
        g_bucket[g_nMasks].calls = 1;
        ++g_nMasks;
    } else {
        g_maskOverflow = true;
    }
}

void report() {
    const int64_t nowT = now_ns();
    const double window = (double)(nowT - g_windowStart) / 1e6;
    if (window <= 0.0) return;

    double timed = 0.0;
    for (int i = 0; i < g_nMasks; ++i) timed += g_bucket[i].ms;

    lucent::info("ticksplit",
                 "over {:.0f} ms of wall clock ({} presents): testPerform accounted {:.1f} ms "
                 "({:.1f}% of wall) across {} outer / {} total calls{}",
                 window, REPORT_EVERY, timed, 100.0 * timed / window, g_outerCalls, g_totalCalls,
                 g_maskOverflow ? "  [MASK TABLE OVERFLOWED -- some masks dropped]" : "");
    lucent::info("ticksplit",
                 "  NOT COVERED: direct list->perform() calls, movement(), present, and all guest "
                 "work outside the testPerform funnel -- the buckets below are a share of the "
                 "{:.1f}% above, not of the tick",
                 100.0 * timed / window);

    // Descending by time. Simple selection sort over <=MAX_MASKS entries.
    bool used[MAX_MASKS] = {};
    for (int k = 0; k < g_nMasks; ++k) {
        int best = -1;
        for (int i = 0; i < g_nMasks; ++i)
            if (!used[i] && (best < 0 || g_bucket[i].ms > g_bucket[best].ms)) best = i;
        if (best < 0) break;
        used[best] = true;
        const Bucket& b = g_bucket[best];
        lucent::info("ticksplit", "  mask 0x{:04x}: {:8.2f} ms  ({:5.1f}% of timed)  {} calls",
                     b.mask, b.ms, timed > 0 ? 100.0 * b.ms / timed : 0.0, b.calls);
    }

    g_nMasks = 0;
    g_maskOverflow = false;
    g_outerCalls = 0;
    g_totalCalls = 0;
    g_windowStart = nowT;
}

void tick_split(CPUState& cpu) {
    if (!enabled()) {
        func_802a4e28(cpu);
        return;
    }

    ++g_totalCalls;

    // Nested dispatch: its cost belongs to the outermost ancestor already timing this subtree.
    if (g_depth != 0) {
        ++g_depth;
        func_802a4e28(cpu);
        --g_depth;
        return;
    }

    const u32 mask = (u32)cpu.gpr[4];
    ++g_outerCalls;
    ++g_depth;
    const int64_t t0 = now_ns();
    func_802a4e28(cpu);
    const double ms = (double)(now_ns() - t0) / 1e6;
    --g_depth;

    account(mask, ms);

    if (g_windowStart == 0) g_windowStart = t0;
    const unsigned frame = VIGetRetraceCount();
    if (frame >= g_lastReport + REPORT_EVERY) {
        g_lastReport = frame;
        report();
    }
}

} // namespace

SB_OVERRIDE(0x802a4e28, tick_split, "TPerformList::perform",
            "diagnostic only (SBR_TICK_SPLIT): time each phase dispatch to price a 60fps "
            "sub-frame; always runs the real body")
