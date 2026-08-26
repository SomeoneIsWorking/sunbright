---
id: C072
kind: claim
status: holds
created: 2026-08-26
tags: gpu,recomp,aurora,interpolation,incident
depends: sms-recomp/runtime/gpu_incident_recorder.cpp, sms-recomp/tools/gpu_flight_dump_main.cpp
---

## Claim

The first kernel fault in the 2026-08-26 reset occurred while Aurora submit 1608 was the only
recorded submit outstanding: submit 1608 returned before the illegal-command-stream event and
submit 1609 began after it. Submits 1609–1614 are aftermath and must not be named as the origin.

## Evidence

The surviving v1 flight records wall-clock nanoseconds. `gpu_flight_dump` with kernel realtime
`1787773714689976000` selects submit 1608: BEGIN `1787773714685944512`, RETURN
`1787773714686864771`, first kernel event `1787773714689976000`, next BEGIN
`1787773714699069230`. Submit 1607's COMPLETE callback predates the event. The causal submit's
pipeline topology and draw shape match a known-success earlier capture; only aggregate dynamic
command hashes differ. Full evidence and limitations:
`debug_journal/2026-08-26_gpu_illegal_command_stream_incident.md`.

## What would falsify it

A more authoritative timestamp source (driver coredump, kernel trace, or corrected flight-clock
calibration) placing another submit in the fault window, evidence that the recorder's realtime clock
was discontinuous across these records, or a parser control showing the stored nanoseconds are
decoded incorrectly.
