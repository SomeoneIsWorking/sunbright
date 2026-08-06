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

## THE FINDING: the drawn matrices are WORLD matrices, so this covers OBJECT motion only

Alpha moves only ~0.8% of pixels while a tick moves ~22%. The cause is not plumbing. Per-tick
displacement of translation elements, 12.2M samples, taken with the camera swinging **49
units/tick**:

```
zero 4,466,100 (37%) | <0.01 496,866 | <1 2,291,466 | <10 1,937,900 | <100 1,787,771 | <1e4 1,214,645 | >=1e4 9,273
```

If the view were baked into these matrices every translation would move by tens of units. 37% move
EXACTLY zero — which is what a static prop's WORLD matrix does. This independently confirms commit
`a338b88` ("the drawn matrices are WORLD matrices, alpha-invariant") from a different instrument.

Consequences:

- record-and-replace on `mDrawMtxBuf` interpolates **object** motion. In Delfino that is Mario and a
  few NPCs — a small share of the screen, hence 0.8%.
- the CAMERA needs its own mechanism. dusklight already splits exactly this way: `interp_view()`
  lerps the camera as a POSE (eye/center/up/bank/fovy, bank via `remainderf`) and rebuilds the
  matrices, separately from `record_final_mtx()`. Its `record_final_mtx` covers per-object matrices
  for the same reason ours does.
- seeding the interpolated view into `j3dSys` AND into the seam's `TGraphics` (`+0xB4`, the field
  `TSmJ3DScn::perform` copies into j3dSys) changed **zero** pixels — verified by two runs producing
  byte-identical output. So the sub-frame's draw consumes the view from neither. WHERE the view
  reaches GX during the re-issue is the open question, and it is the next thing to instrument.

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
