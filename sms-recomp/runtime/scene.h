#pragma once
// scene — the interpolated scene the native renderer draws.
//
// WHY THIS EXISTS (2026-07-23, user-directed): the previous 60fps attempt drove the RENDER from the
// game's tick — replaying the captured GX stream twice per tick with the matrices patched. That can
// never work: it is 2x the GPU cost for one frame, the "interpolation" is a patch over matrices
// already baked into the stream (paired by sequence position, which smears the moment the scene
// changes), and the present cadence is chained to the guest's retrace counter. It was a hack
// fighting the architecture, and it was abandoned rather than tuned.
//
// The correct shape, which the native renderer gets for free:
//
//   * The GAME ticks at 30 Hz and produces SCENE STATE — one transform per drawable. That is all a
//     tick contributes; the geometry itself does not change from tick to tick.
//   * The RENDERER owns its own loop at display rate and interpolates each drawable's transform
//     between the last two snapshots by a factor taken from the WALL CLOCK, not from a field count.
//   * Geometry is uploaded once and cached; only transforms move per frame. So a 60 Hz render of a
//     30 Hz simulation costs one extra transform lerp, not a second scene submission.
//
// Drawables are matched across snapshots by a STABLE KEY (the guest J3DShape pointer plus its
// element index), so a scene that gains or loses an object interpolates everything else correctly
// instead of pairing mismatched objects — the failure mode of the sequence-paired approach.

#include <cstdint>

#include "native_render.h"

// One thing to draw: a geometry reference plus where it is this tick.
struct SbrDrawable {
    uint64_t key;       // stable identity across ticks: (shape pointer << 16) | element index
    uint32_t geom;      // handle into the geometry cache (0 = not yet decoded)
    float    mtx[12];   // the draw matrix, row-major 3x4 — ALREADY model x view (J3D concats the
                        // view matrix in viewCalc), so only the projection remains
};

// Start recording the tick's scene. Called once per game tick, before the capture hooks run.
void sbr_scene_begin_tick();

// Record one drawable. Called from the J3D capture hook.
void sbr_scene_add(const SbrDrawable& d);

// Finish the tick: rotate current -> previous and stamp the wall clock, so the renderer can work out
// how far between the two snapshots any given display frame falls.
void sbr_scene_end_tick();

// Render one display frame. `now_seconds` is the wall clock; the interpolation factor is derived
// from it and the two snapshots' timestamps — NOT from a field counter — so the render rate is
// genuinely independent of the tick rate. Returns the alpha actually used (clamped to [0,1]).
float sbr_scene_render(double now_seconds, const float proj[16]);

// Diagnostics: how many drawables the last tick recorded, and how many of them had a match in the
// previous snapshot (i.e. how much of the scene is actually interpolating rather than snapping).
int sbr_scene_last_count();
int sbr_scene_matched_count();
