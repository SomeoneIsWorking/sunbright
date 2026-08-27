---
id: I034
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

tools/render/radv_hang_trace.py

## Validated by

CPU fixtures prove exact-child known-positive Trace ID reach/not-reach parsing and byte preservation;
no-new, stale, wrong-PID, missing, permission-denied, unavailable, bounds, and
atomic-publish-failure controls disagree. On Mesa RADV 26.1.8, the installed driver binary contains
the expected dump-path and trace-marker formats, and a real guarded stage-1 launch printed RADV's
own costly-mode warning plus `Enabled debug options: syncshaders, hang`. This validates real driver
activation as well as collection/parsing. A clean activation does not validate hang-only report
production.

## Known failure modes

- Hardware trace production during a real fault has not yet been exercised.
- RADV inserts synchronization and can mask the timing/lifetime defect under investigation.
- A missing, partial, denied, stale, or wrong-PID directory is `UNKNOWN`, never evidence that the
  GPU completed or that no hang occurred.
- The kernel watcher stops the game process immediately at the first fault, but RADV's report writer
  lives inside that process. A missing or partial report may therefore mean the safety kill won the
  race, not that the lane was inactive.

## Guard integration controls

The live guard has separate planted controls for a complete exact-child dump on clean exit
(`CAPTURED` plus byte-preserved artifact), absent dumps on nonzero exit and wall timeout (`UNKNOWN`),
and a kernel fault visible only at the timeout path's final cursor barrier (fault exit plus durable
incident/stamp). This proves every terminal path reports its answer; it does not remove the
in-process writer race during a real kernel fault.

The real 600-second RADV integration control reached submit 5,403, ended at the wall cap, crossed a
clean final kernel barrier, and persisted `UNKNOWN` with zero eligible exact-child dumps. This
validates that a clean real driver run reports absence rather than manufacturing success. It does
not validate the `CAPTURED` answer against real hang output.
