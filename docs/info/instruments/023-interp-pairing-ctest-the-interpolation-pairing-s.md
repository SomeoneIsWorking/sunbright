---
id: I023
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

interp_pairing ctest — the interpolation pairing self-test, reachable without a GPU

## Validated by

Run against both directions on 2026-08-12. POSITIVE: ctest -R interp_pairing passes, and the self-test's own output states it exercised both classes (a 1000-unit camera move with object delta 0, and a 50-unit object move with object delta 50; the discontinuity gate firing on a teleport; birth flag on a never-seen tag AND on a tag that skipped a tick and returned; a one-tick gap pairing and an over-limit gap refused). NEGATIVE: the result was temporarily inverted in host/main.cpp, rebuilt, and ctest reported 'interp_pairing (Failed)' — so a failing self-test does propagate through the exit code to the test harness, rather than passing silently. Inversion reverted and re-verified passing. Runs with no window, no ROM, no GPU device.

## Known failure modes

(none recorded yet)
