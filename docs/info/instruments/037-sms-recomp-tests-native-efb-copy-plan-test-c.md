---
id: I037
kind: instrument
status: trusted
created: 2026-08-28
---

## Instrument

`sms-recomp/tests/native_efb_copy_plan_test.cpp` — native EFB copy clipping, clear decoding, and
ordered synthetic-clear contract.

## Validated by

Known-positive controls cover an in-bounds partial copy whose clear must be produced and the
pre-copy/post-copy epoch transition that must reject merging. Other-answer controls cover
clear-disabled, no enabled write mask, and the `Hx_Test5` source beginning exactly at y=448; each
must produce no clear draw. Asymmetric channel bytes and colour-only/depth-only state make swapped
or coupled masks visible.

## Known failure modes

This CPU instrument proves the shipping typed plan and draw state, not that a particular live scene
executes the copy or that SDL3-GPU pixels match Aurora. Whole-frame attribution remains invalid
unless the exact-frame A/B control passes.
