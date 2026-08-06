---
id: I015
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/interp/subframe_position.py — scores WHERE a sub-frame sits between its two main-frame neighbours (asymmetry / lead / off-segment) from ONE run's labelled present series

## Validated by

--selftest forces five cases: a true midpoint scores ~0%, duplicate-of-next +100%, duplicate-of-prev -100%, a static tick REFUSES instead of scoring 0, and a sub-frame wrong in a third direction is caught by off-segment while asymmetry calls it centred. Run by tools/selftest_all.py in the pre-commit gate.

## Known failure modes

(none recorded yet)
