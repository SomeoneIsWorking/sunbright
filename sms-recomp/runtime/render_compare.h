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
