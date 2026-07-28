---
id: I004
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

SBR_TEV_TRACE=<tick> / SBR_TEV_TRACE_BLACK=1 — per-drawable pixel explanation (runtime/scene.cpp)

## Validated by

Runs the SAME tested reference (tev_eval / gx_light) the frame uses, on the drawable's real state and real decoded texels, so the explanation cannot drift from the renderer. Found the channel-1 defect in one run after a day of whole-frame scoring found nothing.

## Known failure modes

(none recorded yet)
