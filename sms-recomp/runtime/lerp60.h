#pragma once
// lerp60 — interpolated 60fps: two presents per 30 Hz game tick, the first with model matrices
// lerped halfway toward the previous tick's.
//
// See debug_journal/2026-07-30_aurora_60fps_lerp_design.md for the design and for the two premises
// that decided it (the uniform block IS patchable in place; a recorded frame is NOT re-executable).
//
// The short version: a tick contributes MATRICES, not geometry — model-space vertices do not change
// between ticks, animation moves the transforms. So a 60 Hz render of a 30 Hz simulation is the
// same geometry drawn twice with different transforms, not two scene submissions.
//
// GATED OFF BY DEFAULT (SBR_LERP60=1). With the flag off aurora records and presents exactly as it
// did before, which matters because aurora is also the parity oracle the native SDL3-GPU renderer
// is scored against — an interpolation that quietly altered the oracle would corrupt every A/B
// number in the project.
//
// Verify with SBR_SMOOTH=1 (runtime/frame_smoothness.h): it distinguishes a genuine interpolation
// from the same picture presented twice, per screen cell, and is validated against both classes.

#include <cstdint>

// True if SBR_LERP60 is set. Checked at every seam that changes behaviour, so the off path is the
// untouched path.
bool sbr_lerp_enabled();

// True if the GAME warped the camera during the tick just ended, clearing the flag as it reads.
// A warp (CPolarSubCamera::warpPosAndAt) sets position and target outright and zeroes the game's own
// inbetween frame counter, so it is a discontinuity DECLARED by the code that caused it — not one
// inferred from how far the eye moved, which the measured distribution cannot support (see
// sms-recomp/overrides/camera_cut.cpp).
bool sbr_camera_cut_take();

// Sample the gameplay camera's mode this tick, reporting any change with the tick number. MEASURES
// a candidate cut signal; nothing branches on it until it has been shown to fire on the cut ticks
// and stay quiet on the rest.
void sbr_camera_mode_tick(long tick);

// Warp-call and snapped-tick counts, with their denominator. Reported on the same cadence as the
// tag coverage.
void sbr_camera_cut_report();

// Report how many of aurora's draws actually carried an identity tag this run.
//
// This exists because the failure it guards against is silent: if the emitter stops tagging — a
// scene drawn through a path that does not go via J3DShape::draw, a shape whose draws are emitted
// before the tag reaches the stream — interpolation does not break loudly, it just quietly stops
// interpolating those objects, and the frame still looks like a frame. A coverage number turns that
// into something visible. It also carries the DENOMINATOR: "N tagged" alone cannot be told apart
// from "N tagged out of a million".
void sbr_lerp_report_tag_coverage();
