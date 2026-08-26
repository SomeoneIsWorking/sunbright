---
id: I034
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

tools/render/radv_hang_trace.py

## Validated by

CPU fixtures prove exact-child known-positive Trace ID reach/not-reach parsing and byte preservation; no-new, stale, wrong-PID, missing, permission-denied, unavailable, bounds, and atomic-publish-failure controls disagree. This validates collection/parsing only; real RADV_DEBUG=hang activation and hardware trace production remain unverified.

## Known failure modes

- Real `RADV_DEBUG=hang` activation and hardware trace production have not yet been exercised; the
  trusted claim is limited to collector/parser behavior on controlled directories.
- RADV inserts synchronization and can mask the timing/lifetime defect under investigation.
- A missing, partial, denied, stale, or wrong-PID directory is `UNKNOWN`, never evidence that the
  GPU completed or that no hang occurred.
