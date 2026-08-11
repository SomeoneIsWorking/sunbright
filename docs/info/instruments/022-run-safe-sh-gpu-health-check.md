---
id: I022
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

run-safe.sh GPU health check

## Validated by

Counts amdgpu ring-timeout / reset / device-wedged / GPUVM-fault lines in the kernel log across a run — an EXTERNAL check on whether our own run disturbed the card, which does not depend on the process being checked. Run against BOTH classes on 2026-08-12: KNOWN-POSITIVE = boot -2 (the session in which the card was reset repeatedly) scores 108; KNOWN-NEGATIVE = an idle 2-minute window scores 0. Third case covered too: if journalctl cannot be read it prints UNKNOWN and exits 2, never 0, so 'nothing happened' and 'I could not look' do not share an output. A run that trips it exits 3 even when the game itself exited cleanly.

## Known failure modes

(none recorded yet)
