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

### Note (2026-08-12)
MEASURED 2026-08-12, and one number in the original entry is CORRECTED.

FREQUENCY WAS OVERSTATED. The entry said '~1 run in 3'. Actual: 2 occurrences in ~20 dumps, and 7 CONSECUTIVE runs since have produced only the common frame. Call it uncommon and unquantified; the two sightings are md5 d85d48db7426 from builds on either side of a convergence batch.

WHAT THE DIFFERENCE IS. Not missing geometry and not a shift. Rendering the diff as a mask shows it follows EDGES throughout the scene — plaza tile grout, building trim, awning edges, the sea horizon bands — while flat areas are untouched. A shift is ruled out by correlation: mean |ref(x) - variant(x+s)| is minimised at s=0 (1.72) and rises either side (4.53 at -1, 4.66 at +1). The variant is BLURRIER, measured over the 7302 differing pixels sampled: mean local gradient 21.24 vs the reference's 30.65.

WHICH ONE IS RIGHT IS NOT OBVIOUS, and matters. Blurrier-at-distance is what CORRECT mipmapping looks like, so the common frame may be the defective one — textures sampling level 0 where they should minify. Do not assume the frequent frame is the good frame.

CANDIDATE MECHANISM, NOT CONFIRMED. aurora bakes the level count at texture CREATION (gx.cpp resolve_static_texture -> new_static_texture_2d(..., obj.mip_count(), ...)) and caches on texObjId + texDataVersion ONLY. mip_count() reads has_mips() (mode0's min filter, or flags bit 0) and max_lod (mode1). Neither is part of the cache key, so whichever register state happens to be in force at a texture's FIRST bind decides its mip chain for the rest of the run, and that ordering could vary. That is a real property of the code and would explain the symptom.

WHY IT IS STILL A HYPOTHESIS: the variant did not occur in any instrumented run, so the mechanism was never observed firing. Worse for it, the instrumented runs are perfectly stable — LUCENT_DEBUG=texresolve reports exactly 131 textures created, 113 single-level and 18 mipped, IDENTICAL across all 7 runs. That is evidence of no variation in texture creation among runs producing the common frame; it says nothing about the variant, which was never captured with the channel on. Enabling the channel may itself perturb the timing.

NEXT STEP that would settle it: capture the variant with texresolve on. Since it resists reproduction, the practical route is to log the per-texture mip decision to a file every run and compare the two frames' logs when a variant finally appears, rather than trying to provoke it.
