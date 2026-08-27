// frame_interp.h — THE public interface to interpolated 60fps. One entry point for the whole
// subsystem; everything else in this directory is an implementation of some part of it.
//
// The map of what exists, why there were three of these, and which one survives, is
// docs/60fps/README.md. Read that before changing anything here.
//
// ── SHAPED AFTER DUSKLIGHT ──────────────────────────────────────────────────────────────────────
// TwilitRealm/dusklight is a shipping Twilight Princess PC port on the same decomp+Aurora
// architecture (CC0, cloned at ~/repo/dusklight). It solved this problem once already, and the
// names and the division of responsibility below are deliberately ITS names —
// src/dusk/frame_interpolation.h — so that reading their solution and reading this one do not
// require a translation step. Where this differs, the difference is stated at the declaration and
// the reason is that we recompile retail PPC and cannot edit a draw site, which they can.
//
// ── THE ONE INVERSION THAT MATTERS ──────────────────────────────────────────────────────────────
// A system that needs to do something on a presentation frame REGISTERS for it
// (add_interpolation_callback) rather than being enumerated by the interpolator. That is
// dusklight's design and it is the thing that stops the effect list going stale: an effect added
// later opts itself in at the site that knows it exists, instead of waiting for someone to remember
// to add it to a list in this file.

#pragma once

#include <cstdint>

namespace sb::frame_interp {

// dusklight's enum, and its semantics (src/dusk/settings.h).
//
//   Off        one present per simulation tick. The game as the console ran it.
//   Capped     a fixed number of presents per tick — for us, two.
//   Unlimited  presentation samples track the active display refresh rate while SMS simulation
//              remains at its original 30000/1001 Hz.
enum class Mode : uint8_t { Off = 0, Capped = 1, Unlimited = 2 };

Mode mode();
bool is_enabled(); // mode() != Off

// How far through the tick the frame being presented is: 0.0 = the previous tick's state, 1.0 =
// this tick's. Forced to 1.0 while a presentation sync is active.
float interpolation_step();

// ── PRESENTATION SYNC — dusklight's request_presentation_sync ────────────────────────────────────
//
// "This tick has no meaningful in-between; present it EXACTLY." A camera warp, a scene cut, a
// teleport: the halfway pose is a viewpoint the game never simulated, and interpolating across it
// is not smoothing, it is inventing a frame.
//
// It is a REQUEST FROM THE GAME, never a threshold. The obvious alternative — "the eye moved more
// than N units this tick" — is unfalsifiable here: the measured distribution of camera step has a
// populated middle with no gap to put a threshold in (34 ticks in [10,100), 36 in [100,1k) over an
// 898-tick plaza run, debug_journal/2026-08-04_interp60_pairing_attribution.md), so any N either
// cuts genuinely fast motion or misses genuinely small warps, invisibly. The game already knows:
// CPolarSubCamera::warpPosAndAt sets position and target outright AND zeroes its own inbetween
// frame counter. camera.cpp observes that and calls this.
void request_presentation_sync();
bool presentation_sync_active();

// ── INTERPOLATION CALLBACKS — dusklight's add_interpolation_callback ─────────────────────────────
//
// Registered DURING a simulation tick, sealed by begin_sim_tick() at the frame boundary, then
// called for each in-between presentation sample belonging to that completed tick. The next frame
// boundary replaces the sealed list, so a system that stops registering stops being called and
// there is no permanent list of effects in this file to go stale.
//
// `is_sim_frame` is always false in this retained-replay runtime. It remains in the
// Dusklight-shaped callback signature, but the game-owned simulation frame does not need callback
// dispatch here.
//
// ⚠ WHAT A CALLBACK CAN AND CANNOT DO, because getting this wrong produces a silent no-op.
//
// The dispatch is `aurora_replay_sample()`, which aurora calls from `end_frame()` BEFORE
// `begin_frame()` and `install_replay_snapshot()` — so a callback genuinely runs ahead of each
// in-between present. It does NOT follow that a callback can
// draw into that frame: the in-between image is a SNAPSHOT of the tick's recorded passes, and
// install_replay_snapshot() throws away the pass begin_frame() just created and substitutes the
// snapshot's. GX emitted from a callback lands in the NEXT tick's stream, where it will be drawn
// once, late, and at the wrong pose.
//
// So a callback may do host-side work — read state, update a classifier, decide how the
// interpolator should treat something. It may NOT issue geometry, and it may not re-run a guest
// draw and expect the result to appear. That is dusklight's one structural advantage here: they own
// decomp source and edit the draw site itself; we recompile retail PPC and the in-between frame is
// a replayed stream.
//
// Correcting an effect on the in-between frame therefore means PATCHING THE RECORDED STREAM (see
// docs/60fps/effects.md — the water refraction is identifiable by its texmtx-slot-0x1e load marker
// and needs an eye-space reprojection, not a matrix lerp), not re-issuing the effect here.
using Callback = void (*)(bool is_sim_frame, void* user);
void add_interpolation_callback(Callback cb, void* user);

// ── SEAMS. Called by the frame loop, not by systems. ─────────────────────────────────────────────

// The simulation tick's game work has ended and its presentation is about to begin: seal the
// callbacks registered during that tick, discard the previous tick's sealed list, and advance the
// sequence. The historical name is retained because this is also the boundary that starts the next
// simulation-tick interval.
void begin_sim_tick();
uint64_t sim_tick_seq();

// An in-between sample is about to be presented — dispatch the callbacks with its alpha. Called
// from aurora_replay_sample() before each retained-snapshot replay.
void present_interpolated_frame(float alpha);

// Per-run summary, with denominators. The current retained-replay path has no callback clients, so
// zero registrations is reported as an unused API rather than misreported as missing interpolation
// coverage. Matrix/vertex/effect coverage belongs to the recorded-stream audits.
void report();

} // namespace sb::frame_interp
