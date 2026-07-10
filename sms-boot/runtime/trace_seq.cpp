// trace_seq.cpp — single shared, totally-ordered event sequence counter.
//
// Problem: the four boot-order diagnostics (SB_PLIST_ORDER_DBG, SB_PROJ_DBG,
// SB_DBHEAD_DBG, plus the frame_seam present boundary) each print to stderr
// with their OWN private counter and/or the VIGetRetraceCount() stamp. Two
// lines that print in the same present can carry DIFFERENT retrace numbers
// (VIGetRetraceCount only advances inside sb_frame_present's retrace loop,
// AFTER the frame's draws/projections were already emitted this dispatch),
// so retrace stamps alone cannot interleave the families into one order —
// that's the "camera-1 write stamped 1432, the draws it covers stamped 1434"
// conflict this instrument was built to resolve.
//
// Fix: one process-wide atomic counter, sb_trace_seq(), that every gated
// instrument calls exactly once per printed line and prefixes into the line
// as `seq=N`. Sorting all four logs' lines by `seq=` gives the exact
// happens-before order across present/perform/projection/drawbuf-flush
// events, independent of which retrace bucket each falls in. Single game
// thread makes plain fetch_add adequate; kept atomic anyway since aurora's
// SDL/Dawn worker threads exist in the same process (they don't currently
// call this, but a relaxed atomic costs nothing and removes the question).
//
// Gated by SB_TRACE_SEQ=1 (checked at each call site, not here) so the
// counter itself is always cheap to link but the callsites stay silent by
// default. Declared weak at every call site (same pattern as
// VIGetRetraceCount) so aurora's standalone unit tests still link without
// the game.

#include <atomic>
#include <cstdint>

extern "C" uint64_t sb_trace_seq(void) {
    static std::atomic<uint64_t> s_seq{0};
    return s_seq.fetch_add(1, std::memory_order_relaxed);
}
