---
id: I001
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

sms-recomp/tests/tev_eval_test.cpp — GX TEV pipeline unit tests

## Validated by

NEGATIVE CONTROL RUN: disabling compare-mode handling in tev_eval.cpp fails 7 checks with specific got/want values; restoring passes. Expectations are hand-derived from decomp/sms SDK sources (GXSetTevColorOp packing, GXTevOp enum, konst ramp, GXCompare), not from a run of the implementation.

## Known failure modes

(none recorded yet)

## Path corrected 2026-08-12

Recorded as `tests/tev_eval_test.cpp`, which has not existed for some time — the tests live under `sms-recomp/tests/tev_eval_test.cpp`. A registry entry naming a path that is not there is not a small error: this file is what a later session consults INSTEAD of searching, so a wrong path reads as 'the instrument is gone' and the check gets rebuilt or skipped.

Re-run and still passing on 2026-08-12 (CPU-only, no GPU involved): `./build-sms-recomp/tev_eval_test` -> "all checks passed", exit 0.
