---
id: I001
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

tests/tev_eval_test.cpp — GX TEV pipeline unit tests

## Validated by

NEGATIVE CONTROL RUN: disabling compare-mode handling in tev_eval.cpp fails 7 checks with specific got/want values; restoring passes. Expectations are hand-derived from decomp/sms SDK sources (GXSetTevColorOp packing, GXTevOp enum, konst ramp, GXCompare), not from a run of the implementation.

## Known failure modes

(none recorded yet)
