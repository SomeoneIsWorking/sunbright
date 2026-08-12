---
id: 5
title: the decomp runtime renders one of TWO different frames at the same settled game state, at random, ~1 run in 3
status: open
symptom: SB_DUMP_FRAME at a fixed SB_DUMP_FRAME_AFTER on SB_STAGE=1 yields md5 ab0910ca47a1 on most runs and d85d48db7426 on some, from the SAME binary; 5.41% of pixels differ by more than 8 levels
tags: render,decomp,nondeterminism,verification
created: 2026-08-12
updated: 2026-08-12
---

OBSERVED 2026-08-12 while runtime-verifying upstream convergences, and it is NOT caused by them: both hashes were produced by builds from before and after the convergence batches, so it is pre-existing and build-independent.

IT IS NOT DUMP TIMING, which was the obvious explanation and is falsified. If the dump were landing on a different game tick, neighbouring present counts would differ by a similar amount. They do not: presents 199, 200 and 201 are IDENTICAL to each other (0.00% and 0.01% of pixels over the 8-level threshold) because the scene is settled at that point, while the variant differs by 5.41%. Two ticks of animation change nothing here; the variant changes 66,433 pixels.

SHAPE OF THE DIFFERENCE. Scene-wide, not localised to a subsystem: 582 of 960 rows touched, bounding box x[43,1275] y[71,699]. Mean RGB moves only 0.4 levels (144.91/150.27/155.83 vs 145.27/150.51/156.02) but the differing pixels have a median delta of 15, p90 34 and max 223. That combination — small mean shift, large local deltas over a wide area — reads as something present in one frame and absent or displaced in the other, rather than a global tint or exposure change.

WHY IT MATTERS BEYOND ITSELF: byte-identical frame comparison is the verification method used for convergence batches. It stays valid as POSITIVE evidence when frames match, but a mismatch cannot be attributed to the change under test until this is understood — which is how it was nearly misread as a convergence regression today.

NOT YET DONE: no bisect against a known-good older build, and no identification of what geometry the differing region belongs to. The frames are kept at scratch/raw/c4_1.rgba (common) and scratch/raw/c4_2.rgba (variant).
