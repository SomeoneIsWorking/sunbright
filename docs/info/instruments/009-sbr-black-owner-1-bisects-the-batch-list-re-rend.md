---
id: I009
kind: instrument
status: trusted
created: 2026-07-29
---

## Instrument

SBR_BLACK_OWNER=1 — bisects the batch list, re-rendering the frame with a prefix, to name the batch that paints a given pixel black

## Validated by

Self-proving before it bisects: with ZERO batches the sample pixel must be the clear colour and with ALL batches it must be black; if either fails it prints 'bisect INVALID' and attributes nothing. Found the EFB-copy quad in one run after days of indirect reasoning, and after the fix correctly reports INVALID because nothing is black.

## Known failure modes

(none recorded yet)
