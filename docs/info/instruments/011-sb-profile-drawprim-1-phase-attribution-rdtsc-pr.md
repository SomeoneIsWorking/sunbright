---
id: I011
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

SB_PROFILE_DRAWPRIM=1 phase attribution — rdtsc-probed nine-way split of draw_prim, printed per drain (extern/aurora/lib/gx/fifo.cpp report, DP_PHASE probes in command_processor.cpp)

## Validated by

Five controls, each able to fail: (1) unattributed = whole - sum(phases); the phases are built as a partition so a region with no probe on some path shows up here, reads 1.9%. (2) probe-cost x probes/call vs measured body reads 0.5% and prints 'TOO HIGH: phase split is NOT admissible' past 25% — this is the defect that made the OLD clock_gettime profiler report 'scan=14%' while partly measuring its own probes. (3) merged+unmerged+early-return must equal calls: 44622+1291+0=45913 exact. (4) TRUNCATED flag when the percentile ring clips. (5) an n= denominator and '% of calls' beside every phase — this one CAUGHT A REAL MISREADING: dividing each phase by TOTAL calls made handle_draw_unmerged read as 293ns/call when it runs on 1291 of 45913 calls and really costs 35x that.

## Known failure modes

(none recorded yet)
