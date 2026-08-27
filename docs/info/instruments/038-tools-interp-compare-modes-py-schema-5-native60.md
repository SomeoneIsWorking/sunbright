---
id: I038
kind: instrument
status: trusted
created: 2026-08-28
---

## Instrument

tools/interp/compare_modes.py schema-5 Native60/Lerp60 spatial comparator: binds ROM, DOL, host binary, tool/launcher sources, exact frame hashes, texture descriptor set, guest-retrace-keyed input, per-tick settled camera matrices, and GPU-clean completion; requires a byte-exact Native60 repeat before scoring. It localizes temporal alternation to screen cells but does not identify the draw occupying a cell.

## Validated by

2026-08-28 self-test plus live water-facing control: Native60 repeat was byte-exact over 33 frames/ticks 1822..1854, all camera and provenance gates passed, and the planted every-other-frame duplicate produced 16/32 exact zero steps and raised whole-ROI mean spatial alternation from 0.291 to 1.000.

## Known failure modes

It localizes temporal unevenness but cannot identify which draw produced a pixel. Different content
can cross a cell during the capture, and a region-wide improvement can conceal one worse subregion.
It does not measure scanout timing. Its texture gate compares stable descriptor populations rather
than texture bytes; ROM and DOL hashes bind the asset source.
