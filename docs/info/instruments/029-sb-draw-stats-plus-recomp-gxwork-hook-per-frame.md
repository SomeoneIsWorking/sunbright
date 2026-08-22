---
id: I029
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

SB_DRAW_STATS plus recomp gxwork hook — per-frame FIFO input/output and exact auto-array loop work

## Validated by

Zero-work boot frames report auto_scan_draws=0; settled gameplay reports nonzero scan work and exactly one synchronous replay; gx_fifo_input_test includes a little-endian known-difference control and verifies stats reset/compaction; changed-index Aurora control must alter the max result.

## Known failure modes

(none recorded yet)
