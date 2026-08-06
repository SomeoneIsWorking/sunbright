// frame_interp.h — THE public interface to interpolated 60fps. One entry point for the whole
// subsystem; everything else in this directory is an implementation of some part of it.
//
// The map of what exists, why there were three of these, and which one survives, is docs/60fps.md.
// Read that before changing anything here.
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
//   Unlimited  as many presentation frames as the display can take. NOT IMPLEMENTED HERE: the
//              current mechanism produces exactly one in-between frame per tick, so requesting
//              Unlimited gets Capped and says so once, rather than silently behaving as something
//              the caller did not ask for.
enum class Mode : uint8_t { Off = 0, Capped = 1, Unlimited = 2 };

Mode mode();
bool is_enabled();   // mode() != Off

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
// Registered DURING a simulation tick, called once when that tick's in-between frame is presented.
// The registration is cleared at the start of every tick, so a system that stops registering stops
// being called, and there is no list of effects in this file to go stale.
//
// `is_sim_frame` is false for the in-between frame and true for the tick's own frame, matching
// dusklight, so one callback body can handle both.
using Callback = void (*)(bool is_sim_frame, void* user);
void add_interpolation_callback(Callback cb, void* user);

// ── SEAMS. Called by the frame loop, not by systems. ─────────────────────────────────────────────

// A new simulation tick has begun: clear the callback registration and advance the sequence.
void begin_sim_tick();
uint64_t sim_tick_seq();

// The in-between frame is about to be presented — dispatch the callbacks. Called from
// aurora_replay_midpoint(), which is the one place in the frame loop that is genuinely BETWEEN the
// tick's two presents.
void present_interpolated_frame();

// Per-run summary, with denominators. Prints even when nothing registered, because "no effect asked
// to be interpolated" and "the dispatch never ran" are the same silence otherwise — and of those
// two, the dispatch never running is by far the more likely defect.
void report();

} // namespace sb::frame_interp
