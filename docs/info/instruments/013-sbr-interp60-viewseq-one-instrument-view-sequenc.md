---
id: I013
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

SBR_INTERP60_VIEWSEQ (one-instrument view sequence for a 60fps sub-frame)

## Validated by

Replaces two probes whose readings were joined by order across separate runs. Emits gfx mViewMtx writes and j3dSys view sets INTERLEAVED in dispatch order from ONE pass, with per-pass GX bytes and row 1 of the matrix (where a reflection inverts) for both sides, since the row-0 translation alone cannot separate a reflected view from the main camera's. Arms on camera MOTION (SBR_INTERP60_VIEWSEQ_MIN), not a present index — armed on an index it printed identical views at every alpha twice because the camera was parked, which read as 'alpha reaches nothing'. Reports its denominator and says explicitly when no writer or no scene set was seen.

## Known failure modes

(none recorded yet)
