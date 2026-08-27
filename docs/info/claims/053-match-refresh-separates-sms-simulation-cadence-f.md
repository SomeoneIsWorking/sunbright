---
id: C053
kind: claim
status: holds
created: 2026-08-20
tags: interpolation
depends: sms-recomp/app/frame_rate.cpp#presentation_count_for_tick, sms-recomp/frame_interp/frame_interp.cpp#present_interpolated_frame, extern/aurora
reconfirmed: 2026-08-27
verified_at: 2026-08-27 22:45:05
---

## Claim

Match Refresh separates SMS simulation cadence from display presentation cadence: at a forced 120 Hz display, interpolated mode advances 30 SMS ticks while emitting 90 additional samples for 120 total presentations, and read-only .25/.75 samples do not advance interpolation history.

## Evidence

2026-08-20 windowless Vulkan run via run-safe.sh with SBR_FRAME_RATE=interpolated-unlocked, SBR_DISPLAY_HZ=120, SBR_QUIT_AFTER=120 exited 0 with zero amdgpu faults and reported 30 simulation ticks plus 90 in-between frames. SBR_INTERP_SELFTEST separately produced positions 10 and 30 at alpha .25/.75 while tick and pair counters remained unchanged.

## What would falsify it

A repeat 120 Hz run does not report a 1:3 simulation-to-in-between count, the .25/.75 self-test advances tick/history counters, or cadence/replay code changes.

## Re-confirmed 2026-08-27

2026-08-27 ./run.sh diagnostic Match Refresh control at forced 120 Hz exited 0 after 120 presentations and reported exactly 30 simulation ticks plus 90 in-between frames; the v3 GPU flight retained 120/120 successful completion callbacks with zero corrupt/torn/bounds records. Aurora ReplayLineage retained-sample and read-only-resampling controls passed in the 247-test Clang ctest run.
