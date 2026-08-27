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

// Reserve the exact Aurora packet selected by the frame sink for this native render. Returns zero
// when this present is outside the comparison cadence. Query before aurora_end_frame closes the
// packet; delayed callbacks later rendezvous on this ID, never on arrival order.
uint64_t sbr_compare_capture_frame_id();

// Capture the current native frame, optional round-robin ablation, and seal both under the exact
// Aurora packet selected by the sink. This owns the renderer-specific producer transaction so the
// frame seam only orchestrates subsystems.
void sbr_compare_capture_current_native_frame();

// Hand over the native image for the reserved Aurora packet (RGBA8, top-left origin) plus the exact
// clear colour it was rendered over.
void sbr_compare_submit_native(uint64_t frameId, const uint8_t* rgba, int w, int h, uint8_t clearR,
                               uint8_t clearG, uint8_t clearB);

// True if SBR_AB is on.
bool sbr_compare_enabled();

// ---- OPERATION ATTRIBUTION ----------------------------------------------------------------
// The scalar score above can say the frame is wrong; it can never say WHICH GX operation is wrong.
// Answering that by toggling an env per run and comparing means across runs does not work: the
// mean drifts several points with the frame COUNT alone, so two runs of different length are not
// comparable, and that trap has already produced one wrong conclusion in this arc.
//
// So each variant is scored against the exact Aurora frame and native baseline carrying the same
// packet ID. Each variant replaces one operation with a neutral reference (texgen -> raw uv,
// texture fetch -> white, ras -> channel 0, and so on). This makes each row's delta valid; the
// round-robin still samples different variants on different scene frames, so cross-row ranking is
// exploratory until every variant is exercised on the same frozen frame population.
//
//   SBR_ABLATE=1   run the sweep on every scored frame and report a ranked table.

// Submit one labelled variant of the current native frame. Scored against the same aurora frame as
// the baseline; accumulated per variant across frames.
void sbr_compare_submit_variant(uint64_t frameId, int id, const char* name, const uint8_t* rgba,
                                int w, int h);
// Seal the native producer side after its optional variant is submitted. An Aurora callback that
// arrived early remains retained until this call; a later callback completes the same rendezvous.
void sbr_compare_finish_frame(uint64_t frameId);

// True if the ablation sweep is on.
bool sbr_compare_ablate_enabled();

// WHICH ablation to render on this frame, when sweeping one variant per frame. The rotation is
// advanced by the SCORER, not by the caller: the sweep runs once per present while scoring runs
// once per A/B frame, and a caller-side counter aliases against that ratio — with 60 presents per
// scored frame and 15 variants it stuck on a single id and gave it every sample, leaving the other
// fourteen at n=0 for the whole run. Advancing on the event that actually consumes a variant
// cannot alias, whatever the two cadences are.
int sbr_compare_ablation_to_render();

// Attribution table: every variant's mean score and paired delta from its exact-frame baseline.
void sbr_compare_report_attribution();
