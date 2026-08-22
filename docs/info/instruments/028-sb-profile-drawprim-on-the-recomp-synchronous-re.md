---
id: I028
kind: instrument
status: DISTRUSTED
created: 2026-08-22
distrusted_on: 2026-08-22
---

## Instrument

Retired draw-primitive elapsed profiler on the recomp synchronous-replay path

## Validated by

Aurora drain now reports replay counters even with an empty live FIFO; a profiled 180-present run produced 31,291-31,965 calls/frame, and the independent path partition control merged+unmerged+early-return equalled calls on every report while the run-safe GPU check remained clean.

## Known failure modes

- It profiles Aurora's `draw_prim` body and retained-array accounting, not the guest/FIFO phase or
  the complete present tick. A lower reported draw cost cannot by itself establish 60 FPS.
- Any future submission path that bypasses both `aurora_fifo_replay()` and the frame-end
  `fifo::drain()` boundary will not be represented.

## DISTRUSTED 2026-08-22

Same underlying elapsed/TSC draw_prim profiler as I011. Correct replay plumbing does not make host-time attribution a stable optimization criterion.

The runtime switch and elapsed profiler were removed. Deterministic replay/work accounting now
lives in I029; this entry remains only to invalidate conclusions that cited the clock instrument.

> Every result this instrument produced is suspect until it is re-validated.
