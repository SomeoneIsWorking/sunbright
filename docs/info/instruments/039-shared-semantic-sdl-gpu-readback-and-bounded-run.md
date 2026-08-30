---
id: I039
kind: instrument
status: trusted
created: 2026-08-30
---

## Instrument

Shared semantic SDL GPU readback and bounded-runtime summary

## Validated by

The production client encoded an empty frame and measured exactly zero non-clear pixels, then encoded planted picture, resource-font glyph, and solid-rectangle operations through the same ordered path and measured non-clear pixels plus known-different order/content hashes; duplicate consumption was visibly refused. Guarded recomp and decomp runs produced non-clear live samples with completed submissions and no kernel GPU fault. Coverage is limited to collected pane/immediate pictures, resource-font glyphs, and GC2D solid rectangles: it does not measure windows, `J2DGrafContext::fillBox`, 3D, mip chains, particles, lights, effects, appearance correctness, or cross-runtime parity. The watcher control independently plants a kernel-fault line and proves it kills the exact owned process group before a clean GPU result is trusted.

## Known failure modes

(none recorded yet)
