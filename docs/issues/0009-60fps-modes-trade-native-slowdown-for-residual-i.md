---
id: 9
title: 60fps modes trade native slowdown for residual indexed-quad jitter
status: investigating
symptom: Native 60fps slows down under scene load; interpolated 60fps still jitters in TDLColorTexQuad and TDLTexQuad indexed geometry
tags: 60fps,performance,interpolation,indexed-geometry
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The modes fail for different reasons:

- Native 60 executes a complete game/FIFO/render tick at 60 Hz. Deterministic settled-plaza work
  accounting shows roughly 123.5k guest FIFO appends (357 KB), 1.80 MB after display-list expansion,
  30.4k auto-sized primitives / 506k indexed-field visits, and about 1.42k finalized Aurora draws
  per tick. No missing BSE timing adjustment can remove that work; the optimization target is the
  duplicated FIFO/Aurora command path and repeated per-draw state/cache work.
- `TDLColorTexQuad::draw` (0x80224f0c) and `TDLTexQuad::draw` (0x80225408) rebuild persistent indexed
  XYZ-f32 arrays each 30 Hz tick while using an identity position matrix. Matrix pairing therefore
  cannot interpolate their motion.


## What was tried / dead ends

- Treating the native symptom as another frame-rate formula is rejected: a constant can only alter
  simulation behavior or pacing; it cannot reduce the measured internal command/draw workload.
- Ordinary matrix tagging cannot fix the two TDL batches. Identity-to-identity pairing leaves their
  moving array bytes untouched and can produce a false-success count.
- A stage-1 idle run is not a live control for the TDL seam: it produced 0 marked indexed draws. The
  instrument now reports that explicitly instead of allowing zero to read as success.


## Resolution

Partial, 2026-08-22. Added an explicit one-shot indexed-deformation marker at the two retail draw
functions and retained-frame big-endian XYZ interpolation in Aurora. Its synthetic known-motion
control passes and the full interpolated runtime exits clean with 360 simulation plus 360 in-between
presents and no GPU faults. The active stage-1 scene did not exercise either TDL batch, so their
graphics-registry verdict remains `camera-only` pending a live spray/question/splash capture.

Native 60 remains performance-limited. Its proper follow-up is FIFO/render optimization selected by
internal work counts and no-loss CPU sampling; this issue remains investigating rather than
pretending the native symptom is fixed.

### Note (2026-08-22)
2026-08-22: Root causes separated. Indexed-array interpolation seam implemented with a passing known-motion control, but the stage-1 run reached zero active TDL batches, so live coverage remains unverified. Native 60 needs internal-work-driven FIFO/render optimization.

### Note (2026-08-22)
Optimized the proven guest-call and MMIO routing costs without changing cadence: sparse exact-address dispatch reduced call_ppc + override_lookup sampling share from 9.65% to 3.96%, and the retained per-thread MMIO device cache reduced its routing work. Remaining sampled leaders are draw_prim, retained-array XXH3 hashing, Sunbright FIFO parsing, and Aurora command processing. Evidence: debug_journal/2026-08-22_native60_dispatch_optimization.md.

### Dead end (2026-08-22)
The earlier scalar-append/raw-gather experiments were judged only by host elapsed time and are not reusable evidence. The current `GxFifoInput` is instead justified by exact work elimination: it removes one generic range insertion per guest store, preserves byte order under a known-difference control, and reports zero settled-frame compactions/moved bytes.

### Note (2026-08-22)
Compile-time direct-target caching reduced combined guest target-resolution sampling share from 3.96% to 1.22% (84,148 direct sites using per-target slots; 7,409 genuinely indirect sites). The issue remains investigating because FIFO parsing, array scanning and per-draw cache hashing remain sampled leaders. Evidence: debug_journal/2026-08-22_native60_dispatch_optimization.md.

### Dead end (2026-08-22)
A page-version fingerprint for Aurora's unchanged persistent arrays reduced XXH3 sampling share, but
required touching authoritative dirty state on every guest store. The prototype was removed: it
shifted work into a much larger event population and had no cheap authoritative dirty source. Do not
reintroduce guest-store dirty tracking without first proving a lower-work invalidation source.

### Note (2026-08-22)

Internal-work correction: `SB_DRAW_STATS` + `gxwork` reports roughly 123.5k write-gather appends /
357 KB guest input, 1.80 MB after display-list expansion, 30.4k auto-array scans / 506k field visits,
and 1.42k finalized draws per settled plaza frame. `GxFifoInput` removes the per-store generic vector
insertion; its live control reports zero compactions and capacity growth after warmup. A 499 Hz
bounded capture recorded 10,398 samples with zero losses. Wall-clock frame averages are no longer
used to choose work.
