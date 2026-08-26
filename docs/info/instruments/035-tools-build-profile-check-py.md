---
id: I035
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

tools/build/profile_check.py

## Validated by

Synthetic positive profile plus four controls: unoptimized guest, missing GPU diagnostics, NDEBUG in
Debug, and missing representative compile command. Real decomp and recomp Debug commands plus a
recomp Release control were checked on 2026-08-27.

## Known failure modes

This proves the emitted compile and diagnostic policy, not end-to-end frame rate. Runtime throughput
still requires a guarded run on an uncontended host and must not be attributed from wall time alone.
