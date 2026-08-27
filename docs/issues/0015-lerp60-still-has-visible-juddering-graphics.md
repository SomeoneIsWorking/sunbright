---
id: 15
title: Lerp60 still has visible juddering graphics
status: investigating
symptom: user observes graphics stepping or juddering in interpolated 60 FPS despite existing per-draw coverage reports
tags: reported,60fps,interpolation,judder,graphics
created: 2026-08-26
updated: 2026-08-27
---

## Observation

The running product still has visibly juddering graphics in `Interpolated 60 FPS`. This falsifies
using high aggregate tag or population coverage as proof that presentation is fully smooth. The
affected graphic or region is not yet identified.

## Investigation boundary

Compare `interpolated-60` with `native-60` over overlapping guest retrace labels while a scripted
C-stick produces sustained geometric motion. Measure consecutive presented RGBA frames, not draw
counts alone:

- whole-frame and cropped temporal step evenness;
- spatial alternation cells, with Native 60 as the scene-content control;
- duplicate presents and presentation timing separately from object-space snapping.

The comparison must include a forced-snap positive control. A region that alternates only in
lerp60 is a missing interpolation treatment; alternation shared by Native 60 is scene content; a
globally uneven or duplicated lerp60 series points to presentation cadence rather than a target.

## Instrument correction (2026-08-27)

The first comparison is invalid and must not be used for attribution. Native 60 produced 33 frames
from the pre-change binary; lerp60 produced only 8 readable frames from the later binary before an
amdgpu illegal-command-stream reset. `compare_modes.py` accepted the unequal, partial series and
ranked a lower-right cell anyway. That output suggested the animated water gauge, but it did not
prove the gauge caused the reported judder.

The gauge and persistent J2D pane identities remain implementation candidates backed by their
guest draw semantics and live pairing counts. The replacement comparator now binds captures to the
same binary, tool revision, configuration, and complete GPU-clean manifests; requires an exact
Native60 repeatability control; enforces Native60 `main` and Lerp60 alternating `main`/`sub` role
cadence with the expected guest ticks; and refuses unequal samples, unequal guest-time spans,
changed frame bytes, or partial runs before printing a pixel result. Scripted input is exclusive,
so live keyboard/controller state cannot perturb a capture. Each run must also log the requested
effective renderer/frame-rate mode, deterministic virtual clock, exact input script, and exclusive
input policy; this prevents `.env` from silently replacing the comparator's requested contract.

No replacement capture has been attempted after the reset. The comparator remains a spatial
localizer for image-step unevenness, not a scanout-timing probe or an object-identity join. Issue 17
has now completed the replay-range validation gate. The next comparison must still produce a clean
three-run Native60 / Native60-repeat / Lerp60 set before any region is interpreted.
