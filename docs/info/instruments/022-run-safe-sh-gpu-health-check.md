---
id: I022
kind: instrument
status: trusted
created: 2026-08-12
---

## Instrument

run-safe.sh GPU health check

## Validated by

Counts amdgpu ring-timeout / reset / device-wedged / GPUVM-fault lines in the complete boot kernel log before and after a run — an EXTERNAL check on whether our own run disturbed the card, which does not depend on the process being checked. Run against BOTH classes on 2026-08-12: KNOWN-POSITIVE = the reset-heavy boot scores 108; KNOWN-NEGATIVE = an idle interval leaves the boot-wide count unchanged. Third case covered too: if journalctl cannot be read it prints UNKNOWN and exits 2, never 0, so 'nothing happened' and 'I could not look' do not share an output. A run that increases the count exits 3 even when the game itself exited cleanly. Revalidated 2026-08-24 on recomp+Aurora and decomp+Aurora: both returned delta 0 while the historical boot total stayed 42.

## Known failure modes

This boot's wall clock stepped backward, so `journalctl --since` windows returned either stale
history or nothing and could report a false result. I022 must keep using unfiltered before/after
snapshots; a time-window implementation is distrusted on this machine. The count attributes only
that *some* new kernel anomaly appeared during the run; it does not identify which backend submitted
the offending command when two GPU devices share one process.
