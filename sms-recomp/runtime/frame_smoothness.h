#pragma once
// frame_smoothness — does the presented image actually MOVE every present, and WHERE does it not?
//
// WHY THIS EXISTS. The 60fps work has one failure mode that every cheap check misses: presenting
// twice as often while showing the same picture twice. A present COUNT of 60/s reads identical in
// both cases, and so does a frame-time histogram. Only the IMAGE distinguishes them.
//
// And the interesting version of that failure is PARTIAL: world geometry interpolates while some
// full-screen effect (an EFB-copy composite, a fade, the sea pass, the HUD) is regenerated only on
// tick boundaries and so is duplicated on the in-between frames. That is invisible in a whole-frame
// number, because the moving 90% of the screen drowns it. So the measurement is PER CELL, and its
// output names the cells that stand still while the rest of the frame moves.
//
// THE METRIC IS PHASE-FREE, deliberately. The obvious formulation — compare even-indexed frame
// diffs against odd-indexed ones — assumes presents alternate new/duplicate forever. One hitch (a
// long tick, three presents in one tick) permanently breaks that parity, and a genuinely duplicated
// stream would then read as smooth: the instrument would report success precisely when the frame
// pacing is worst. Instead each consecutive-present diff is judged on its own: a duplicate is a
// diff of ~zero, whatever its index.
//
//   dupFrac   share of consecutive presents whose image barely changed. 0.0 = every present is a
//             new image; 0.5 = every other present is a duplicate (i.e. 30fps shown twice).
//   per cell  a cell that reads near-zero on frames when the FRAME is moving is stair-stepped:
//             something in that region is not taking the interpolation into account.
//
// WHAT IT CANNOT SEE — printed with every report, because a negative from an instrument that never
// states its blind spots is indistinguishable from "I did not look":
//   * It measures EVENNESS of motion, not CORRECTNESS. A wrong-but-even interpolation (bad alpha,
//     extrapolation, a mis-paired object sliding between two unrelated poses) scores perfectly.
//     Only a comparison against the oracle can catch that; this catches duplication.
//   * A static cell carries no opinion — nothing moved there in the source either.
//   * A cell larger than the moving object reads as moving when only that object's edge moves.
//
// SELF-TEST (SBR_SMOOTH_SELFTEST=1): every frame is fed to the analyser TWICE, which is exactly the
// defect being hunted. It MUST report dupFrac ~0.5 and classify every moving cell stair-stepped. A
// detector that has never been run against a known positive is a detector that has never been run.

#include <cstdint>

// True if SBR_SMOOTH is set.
bool sbr_smooth_enabled();

// Feed one presented frame (RGBA8, top-left origin). Called for EVERY present when enabled — the
// measurement is about consecutive presents, so any sampling cadence above 1 destroys it.
void sbr_smooth_feed(const uint8_t* rgba, int w, int h);

// Report the classification so far. Called on a cadence and at shutdown.
void sbr_smooth_report();
