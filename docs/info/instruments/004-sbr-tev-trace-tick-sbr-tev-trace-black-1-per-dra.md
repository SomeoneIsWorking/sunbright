---
id: I004
kind: instrument
status: DISTRUSTED
created: 2026-07-28
distrusted_on: 2026-07-29
---

## Instrument

SBR_TEV_TRACE=<tick> / SBR_TEV_TRACE_BLACK=1 — per-drawable pixel explanation (runtime/scene.cpp)

## Validated by

Runs the SAME tested reference (tev_eval / gx_light) the frame uses, on the drawable's real state and real decoded texels, so the explanation cannot drift from the renderer. Found the channel-1 defect in one run after a day of whole-frame scoring found nothing.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-07-29

The CPU TEV reference (tev_eval) decodes GUEST MEMORY, so for any draw sampling an EFB COPY DESTINATION it does NOT mirror what the GPU samples — the GPU reads the rendered copy surface. It reported a confident texel[0,0,0] a1.000 for the compositing quad that was not what was sampled. The trace now flags such units explicitly, but the reference still cannot evaluate those draws.

> Every result this instrument produced is suspect until it is re-validated.
