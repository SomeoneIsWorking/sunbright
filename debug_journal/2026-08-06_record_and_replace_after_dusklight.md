# 2026-08-06 — 60fps: record-and-replace, after dusklight; and the drawn matrices are WORLD matrices

## What changed

The sub-frame gains a second, structurally different implementation, selected by
`SBR_INTERP60_REPLACE=1` (`sms-recomp/overrides/interp60_replace.{h,cpp}`). It is modelled on
**dusklight**'s `src/dusk/frame_interpolation.{h,cpp}` — a shipping TP port on the same
decomp+Aurora architecture, CC0, cloned at `~/repo/dusklight`.

The old path SUBSTITUTES: write an interpolated pose into the game's own objects, re-run parts of
the game from it, render, put it back. Every seam it touches has leaked and each leak had to be
chased separately (camera cache, j3dSys view, waist smoothers, 340,509 pixels of actor state, and a
PreEntry re-issue at cue `0x4` that also runs `requestShadow()` and floods the shadow manager).

dusklight's model does not have that failure mode, and not because it enumerated better: the sim
tick runs UNTOUCHED, final matrices are recorded, the presentation frame lerps prev→cur, and draw
sites read the replacement. Guest state is never written, so it cannot leak. That is structural.

Adapted here: dusklight edits ~20 draw call sites (`d_drawlist`, `d_a_midna`, `d_flower`,
`d_grass`) because it owns decomp source. Recompiling retail PPC we cannot, so the replacement is a
save → write → restore around the sub-frame's draw lists. The key is `(model, index)`, not the
matrix address — `J3DModel::viewCalc` begins with `swapDrawMtx()`, so a joint's matrix alternates
between two addresses on consecutive ticks and an address key would never match.

## VERIFIED: the leak is gone

The invariant the substitution path never met. Comparing two runs that differ only in alpha, at
MATCHED guest ticks, main presents must be byte-identical:

| path | main presents differ |
|---|---|
| substitute-and-re-issue (2026-08-06, earlier) | 99.91% of pixels, mad 13.42 |
| record-and-replace | **0.000%**, mad 0.000 |

Measured twice, at guest ticks 1810..1816 and 2610..2616. `CLOBBERED mid-draw 0` across 750k
matched models: nothing in the re-issue recomputes the matrices while the sub-frame is drawing.

## VERIFIED: the write path reaches the screen

`SBR_INTERP60_REPLACE_KICK=<units>` displaces every replaced matrix's translation by a constant —
a control that MUST fire. At 400 units the whole plaza is thrown out of frame
(`scratch/render/kick_sub-t2612.png`). So `mDrawMtxBuf[1][viewNo]` is what the hardware reads for
essentially the entire visible frame, and the save/write/restore is fully connected.

## RETRACTED, then measured properly: the drawn matrices DO carry the view

The first reading of the displacement histogram was wrong and is recorded here because the mistake
is the instructive part. Cumulative over a whole run, 12.2M translation elements with the camera
swinging 49 units/tick:

```
zero 4,466,100 (37%) | <0.01 496,866 | <1 2,291,466 | <10 1,937,900 | <100 1,787,771 | <1e4 1,214,645 | >=1e4 9,273
```

I read "37% move exactly zero, so these are WORLD matrices and the camera is not covered", and took
it as confirming `a338b88`. That inference does not follow: a cumulative histogram averages the
parked-camera phase of the run together with the moving one, and a fraction with no control says
nothing about what the camera contributes.

So the histogram was made PER-WINDOW (it resets every report) and run against BOTH classes — same
scenario, same window (sub-frames 2400–2700), differing only in the pad script:

| bucket | camera moving, 49 u/tick | camera parked, 0.25 u/tick |
|---|---|---|
| zero | 194,607 (29.2%) | 147,877 (29.2%) |
| <100 | 127,914 | 17,902 |
| <1e4 | **123,764** | **479** |

Large displacements appear only when the camera moves — a factor of 258 in the top finite bucket —
and the frozen fraction is IDENTICAL either way. So the view IS reflected in the drawn matrices for
the bulk of the population, and the ~29% frozen entries are a static population (unused matrix
slots, identity entries) that is invariant to the camera and was never evidence about the view.

A discriminator must be run against both classes before it is trusted. Run against one, this one
scored a confident wrong answer.

Still true and still unexplained: seeding the interpolated view into `j3dSys` AND into the seam's
`TGraphics` (`+0xB4`, the field `TSmJ3DScn::perform` copies into j3dSys) changed **zero** pixels,
verified by two runs producing byte-identical output.

## FOUR CLOCKS, and a whole afternoon of runs that sampled the wrong instant

`DUMP_AFTER` is in PRESENTS. The pad script is in PAD READS (one per game tick). The dump label is
the RETRACE counter (+2 per game tick). `camera_apply #N` counts SUB-FRAMES (one per game tick).
So with two presents per game tick:

```
sub-frame N  ==  pad read N  ==  game tick N  ==  retrace 2N  ==  present 2N
```

`DUMP_AFTER=2600` therefore samples game tick ~1305 — while a pad step written `2000:CSTICK=...`
does not take effect until game tick 2000. Several runs in this session set up a camera motion and
then dumped from BEFORE it started. The tell was that a "camera moving" run and a "camera parked"
run produced BYTE-IDENTICAL main frames (`d8064db3…`), which is impossible unless the sampled
instant is the same in both. Always confirm the condition holds AT THE DUMPED MOMENT — the runner's
CAMTRACE block exists for that and it must not be skipped.

## The camera probes are blind outside the intro — do not read "parked" from them

Three instruments reported the camera still at gameplay moments. All three are looking at the wrong
thing, and none of them says so:

- `camera_apply` / CAMTRACE follow `g_camObj` = `0x81588cd0` ("camera 1"). Scanned across a whole
  run with `SBR_INTERP60_VIEWSEQ_AT=1`, its eye moves only in the first ~90 presents (19.66
  units/tick, the boot/logo phase) and reads 0.000 for the entire remainder — while the filmstrip
  plainly shows the viewpoint changing during gameplay. So this object is not the active gameplay
  camera, and every "the camera did not move this tick" line it prints is really "the object I watch
  did not move".
- `SBR_INTERP60_MTXTRACE` auto-pins the first model viewCalc'd, which prints
  `j3dSys view t=(0.56,-1177.13,-5547.42)`, constant over all 40 lines. y = -1177 is the MIRROR
  camera's view — the same trap the census already documented. It is not watching the scene view.

Finding the object the gameplay camera actually is, and pointing these probes at it, is the
prerequisite for grading camera interpolation at all. Until then, any camera-motion claim in this
arc — including the 49 units/tick used in the table above, which IS supported by `camera_apply`'s
own log for that run — should be checked against the dumped moment rather than assumed.

## Measurement conditions — read before taking any reading here

Three separate moments in the standard `interp60_run.sh` scenario were sampled before one of them
was usable, and two of them silently measured nothing:

- **present 1500 and 1800 are the plaza intro CUTSCENE.** Camera parked (`|eye cur-prev|=0.000`),
  and the dominant per-tick change is the scrolling 2D news ticker at the bottom of the frame —
  `frame_regions.py` puts 72% of the total difference in the bottom two tile rows. J2D/ortho is
  explicitly NOT covered by this path, so asymmetry there saturates near +100% no matter how well
  interpolation works.
- **the walk-forward pad script leaves the camera nearly parked anyway** (0.25 units/tick at
  present 2700).

So `native_pad.cpp` gained `CSTICK=<x>/<y>`, which drives the C-stick and therefore rotates the
camera. `PAD="400:STICK=0/100,1400:STICK=90/0,2000:STICK=0/0+CSTICK=110/0"` with `DUMP_AFTER=2600`
gives 49 units/tick of pure camera motion with Mario standing still — the only sampled condition in
which camera interpolation is gradeable at all.

## Not covered by this path (stated so a 0 cannot read as coverage)

Projection matrix (fovy/zoom), texture and bump matrices, J2D/ortho HUD, JPA particles,
immediate-mode geometry, and any model whose joint count changes between ticks. The report line
prints this list every time it prints a number.

One model (`0x81391d1c`) carries finite-but-absurd garbage in its matrices (max element delta
1.7e37). It swamped the first version of the displacement statistic, which is why that statistic is
a histogram and not a mean. Whether that model is a real defect is not investigated here.
