---
id: I028
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

SB_PROFILE_DRAWPRIM on the recomp synchronous-replay path

## Validated by

Aurora drain now reports replay counters even with an empty live FIFO; a profiled 180-present run produced 31,291-31,965 calls/frame, and the independent path partition control merged+unmerged+early-return equalled calls on every report while the run-safe GPU check remained clean.

## Known failure modes

- It profiles Aurora's `draw_prim` body and retained-array accounting, not the guest/FIFO phase or
  the complete present tick. A lower reported draw cost cannot by itself establish 60 FPS.
- Any future submission path that bypasses both `aurora_fifo_replay()` and the frame-end
  `fifo::drain()` boundary will not be represented.
