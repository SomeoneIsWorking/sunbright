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

- Native 60 executes a complete game/FIFO/render tick at 60 Hz. A controlled Delfino run measured
  about 14.6 ms guest/FIFO plus 7.7 ms render work per tick and fell to 45–52 Hz in heavy intervals,
  over the 16.67 ms total budget. The slowdown is renderer/FIFO throughput, not a missing BSE
  timing adjustment.
- `TDLColorTexQuad::draw` (0x80224f0c) and `TDLTexQuad::draw` (0x80225408) rebuild persistent indexed
  XYZ-f32 arrays each 30 Hz tick while using an identity position matrix. Matrix pairing therefore
  cannot interpolate their motion.


## What was tried / dead ends

- Treating the native symptom as another frame-rate formula is rejected: the measured tick is
  already over budget, so a constant can only alter simulation behavior or pacing.
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

Native 60 remains performance-limited. Its proper follow-up is measured FIFO/render optimization
until the complete tick fits 16.67 ms; this issue remains investigating rather than pretending the
native symptom is fixed.

### Note (2026-08-22)
2026-08-22: Root causes separated. Indexed-array interpolation seam implemented with a passing known-motion control, but the stage-1 run reached zero active TDL batches, so live coverage remains unverified. Native 60 remains over its 16.67 ms budget and needs measured FIFO/render optimization.
