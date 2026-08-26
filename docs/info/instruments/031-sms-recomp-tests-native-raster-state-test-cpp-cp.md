---
id: I031
kind: instrument
status: trusted
created: 2026-08-26
---

## Instrument

sms-recomp/tests/native_raster_state_test.cpp — CPU test for native GX raster admission

## Validated by

Negative control: with the shipping admission seam preserving the former always-submit policy, the GX_CULL_ALL assertion aborted with exit 134. After rejecting cull mode 3, the same test passes while explicit NONE, FRONT, and BACK positive controls remain submit-eligible.

## Known failure modes

This CPU instrument validates admission policy but cannot prove that a selected retail scene emits
GX_CULL_ALL. The guarded stage-1 runtime control reported zero dropped draws, explicitly marking
that run as no live coverage.
