---
id: I002
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

sms-recomp/tests/gx_light_test.cpp — GX colour-channel unit tests

## Validated by

NEGATIVE CONTROL RUN: reintroducing the historical attnFn bit9/bit10 swap AND clamping DF_SIGN fails 6 checks with specific values; restoring passes. attnFn decode asserted directly against GXSetChanCtrl's encoding.

## Known failure modes

(none recorded yet)

## Path corrected 2026-08-12

Recorded as `tests/gx_light_test.cpp`, which has not existed for some time — the tests live under `sms-recomp/tests/gx_light_test.cpp`. A registry entry naming a path that is not there is not a small error: this file is what a later session consults INSTEAD of searching, so a wrong path reads as 'the instrument is gone' and the check gets rebuilt or skipped.

Re-run and still passing on 2026-08-12 (CPU-only, no GPU involved): `./build-sms-recomp/gx_light_test` -> "all checks passed", exit 0.
