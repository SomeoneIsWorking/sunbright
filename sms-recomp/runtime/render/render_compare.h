#pragma once
// render_compare — score the native SDL3-GPU render against the aurora oracle IN PROCESS.
//
// WHY IN-PROCESS (2026-07-23, user-directed). The file-based harness
// (tools/render/compare_native.py) has three defects that no amount of care at the call site
// fixes:
//
//   * It INFERS the background as the most common colour. On the native side that is a large
//     PLANE, not the clear, so it scores geometry as background and reports a number that moves
//     for the wrong reasons. Here the native clear colour is KNOWN exactly.
//   * The two dumps come from different frame indices, hand-aligned by counting presents. Any
//     mismatch is silently attributed to the renderer.
//   * One sample per run. A single frame cannot show whether a change helped in general or on
//     that frame only.
//
// In-process, both renderers are scored on the same frame with an exact background, every N
// frames, as a time series.
//
// The metrics deliberately avoid colour parity, which does not exist yet (the native path draws
// flat per-object colours while aurora draws the textured, TEV-shaded game):
//
//   geom%     fraction of native pixels differing from the KNOWN clear colour — "is geometry
//             being drawn at all", exact rather than inferred.
//   edgeIoU   intersection-over-union of the two EDGE masks (luma gradient above each image's own
//             percentile). Edges are where shape lives, and a percentile threshold is invariant to
//             both images' overall brightness and contrast — so this measures STRUCTURE without
//             requiring either a background or matching colours.
//   lumaCorr  Pearson correlation of luma over the common grid. Rewards getting light/dark in the
//             same places; unlike mean|d| it is unaffected by a constant offset or scale.
//
// SBR_AB=1 enables; SBR_AB_EVERY=N sets the cadence (default 60).

#include <cstdint>

// Register the aurora frame sink and start scoring. Safe to call every frame; acts once.
void sbr_compare_init();

// Hand over the native frame for this frame (RGBA8, top-left origin) plus the exact clear colour
// it was rendered over. Stored until aurora's asynchronous readback for the same frame arrives.
void sbr_compare_submit_native(const uint8_t* rgba, int w, int h, uint8_t clearR, uint8_t clearG,
                               uint8_t clearB);

// True if SBR_AB is on.
bool sbr_compare_enabled();

// ---- OPERATION ATTRIBUTION ----------------------------------------------------------------
// The scalar score above can say the frame is wrong; it can never say WHICH GX operation is wrong.
// Answering that by toggling an env per run and comparing means across runs does not work: the
// mean drifts several points with the frame COUNT alone, so two runs of different length are not
// comparable, and that trap has already produced one wrong conclusion in this arc.
//
// So variants are scored INSIDE one run, against THE SAME aurora frame. Each variant replaces
// exactly one operation with a neutral reference (texgen -> raw uv, texture fetch -> white,
// ras -> channel 0, and so on). Equal-N holds by construction, and the ranked delta names the
// operation: the ablation that RECOVERS the most score is the operation this port gets wrong.
//
//   SBR_ABLATE=1   run the sweep on every scored frame and report a ranked table.

// Submit one labelled variant of the current native frame. Scored against the same aurora frame as
// the baseline; accumulated per variant across frames.
void sbr_compare_submit_variant(int id, const char* name, const uint8_t* rgba, int w, int h);

// True if the ablation sweep is on.
bool sbr_compare_ablate_enabled();

// Ranked attribution table: every variant's mean score and its delta from the baseline.
void sbr_compare_report_attribution();
