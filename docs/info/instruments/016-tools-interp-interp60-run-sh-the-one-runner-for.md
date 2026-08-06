---
id: I016
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/interp/interp60_run.sh — the one runner for a 60fps sub-frame present series; carries SBR_INTERP60_COPY + SBR_PRESENT_AFTER_COPY (without either, every sub present is bit-identical to the main frame before it) and prints camera liveness at the dumped moment before any score

## Validated by

Validated by its own failure: a series taken without the copy switches scored a confident -100% asymmetry while showing nothing, and a series at present 1600 scored byte-identical across alpha because the camera is parked there. Positive control: SBR_INTERP60_STREAMHASH gives same-size different-hash sub-frame streams across alpha at a moving-camera moment.

## Known failure modes

(none recorded yet)
