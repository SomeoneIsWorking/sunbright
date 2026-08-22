---
id: I011
kind: instrument
status: DISTRUSTED
created: 2026-08-05
distrusted_on: 2026-08-22
---

## Instrument

Retired draw-primitive phase attribution — an rdtsc-probed nine-way split of `draw_prim`, formerly
printed per drain by `extern/aurora/lib/gx/fifo.cpp` with probes in `command_processor.cpp`

## Validated by

Five controls, each able to fail: (1) unattributed = whole - sum(phases); the phases are built as a partition so a region with no probe on some path shows up here, reads 1.9%. (2) probe-cost x probes/call vs measured body reads 0.5% and prints 'TOO HIGH: phase split is NOT admissible' past 25% — this is the defect that made the OLD clock_gettime profiler report 'scan=14%' while partly measuring its own probes. (3) merged+unmerged+early-return must equal calls: 44622+1291+0=45913 exact. (4) TRUNCATED flag when the percentile ring clips. (5) an n= denominator and '% of calls' beside every phase — this one CAUGHT A REAL MISREADING: dividing each phase by TOTAL calls made handle_draw_unmerged read as 293ns/call when it runs on 1291 of 45913 calls and really costs 35x that.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-22

TSC elapsed-phase attribution is host-clock performance measurement. Its partition controls catch accounting mistakes but cannot make results invariant to host contention, and it is no longer admissible for choosing optimizations.

The runtime switch and clock probes were removed; this entry remains only to invalidate conclusions
that cited the old instrument.

> Every result this instrument produced is suspect until it is re-validated.
