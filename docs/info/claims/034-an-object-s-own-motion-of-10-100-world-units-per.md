---
id: C034
kind: claim
status: holds
created: 2026-08-11
tags: 60fps
depends: sms-recomp/frame_interp/motion_truth.cpp
---

## Claim

An object's own motion of 10-100 world units per SIMULATION TICK is ORDINARY, not evidence of interpolation mispairing. gpMario measured walking/running spends 90 of 593 ticks in [10,100) with a max of 58.5 units/tick. The interpolation audit's long-standing wording — 'anything from [10,100) up is a pose no object reaches in 1/30 s, so those counts are the mispairings' — is FALSE and was condemning 84,507 legitimate draws (12.5% of world geometry) on a plaza run. The measured boundary is ~100, but NOT because of a clean gap: the same run has 36 paired draws in [100,1k), 73 in [1k,10k) and 73 in [10k,inf). What justifies the boundary is (a) the independent ground truth sitting far below it and (b) an asymmetry of failure — snapping a genuinely fast object costs one frame of interpolation on ~36 draws in 670,000, while interpolating a mispair sweeps a whole model across the screen.

## Evidence

sms-recomp/frame_interp/motion_truth.cpp reads gpMario (0x8040E10C) once per tick and buckets |dPos| with the SAME edges as aurora's paired-draw histogram. Run: SBR_FASTBOOT=1 SBR_LERP60=1 SBR_PAD_SCRIPT='60:STICK=0/-90,400:STICK=90/0,700:STICK=0/90' SBR_QUIT_AFTER=900. Ground truth: mean 7.657 max 58.479, [0,0.1) 249 | [0.1,1) 1 | [1,10) 253 | [10,100) 90. Paired-draw histogram same run: [10,100) 84507 | [100,1k) 36 | [1k,10k) 73 | [10k,inf) 73.

## What would falsify it

a stage where an ordinary object routinely exceeds 100 units/tick (a fast vehicle, a launched Mario, a scripted mover) would move the boundary; and Mario is ONE object, so this bounds a player character's speed, not every object's
