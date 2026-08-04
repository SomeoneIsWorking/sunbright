# decomp vs recomp: where the frame time actually goes, and what that means for true 60fps

The 60fps path decision (lerp vs native 60 Hz, decomp vs recomp) was resting on an assumption:
*native decomp logic is much cheaper than recompiled PPC, so the decomp can afford to run its game
logic at 60 Hz.* The first half is true. The second half does not follow, because logic is not the
bottleneck in either runtime.

## The measurement

Delfino Plaza, stage 1, headless, turbo (unpaced, so these are work costs not pacing).
`SB_PROFILE` for the decomp seam, the frame-time split for the recomp, `SB_PROFILE_GFX` for both.

**Per-tick wall clock** (decomp at load ~10, recomp at load ~6 — see the caveat below):

| | game logic | render | total | fps-equiv |
|---|---|---|---|---|
| decomp (`sms-boot`) | **~3.2 ms** | ~19 ms | 22.2 ms | ~45 |
| recomp (`sms-recomp`) | 11.6 ms | 8.4 ms | 20.0 ms | ~50 |

**Aurora's own view, which is load-independent in its structure:**

| | draws | per-draw build | drain | pipelines created |
|---|---|---|---|---|
| decomp | 1298 | 4.33 ms | **15.08 ms** | **1800** |
| recomp | 1351 | 4.25 ms | 0.00 ms | 458 |

## What it says

**1. Native logic really is much cheaper: ~3.2 ms vs 11.6 ms, about 3.6x.** That part of the
expectation holds, and it is the strongest argument for the decomp as the long-term runtime.

**2. But the totals are comparable, because RENDER dominates both.** The decomp's cheap logic buys
nothing at the frame level: 22.2 ms vs 20.0 ms per tick. Switching runtimes does not create 60fps
headroom.

**3. The recomp's "guest logic" number is not pure logic.** Its aurora drain reads 0.00 ms because
the recomp pre-digests the GX stream incrementally during the frame (`gxfifo_drain_pending`), so the
~4.3 ms of per-draw build lands inside the window my split attributes to guest logic. Corrected, the
recomp is roughly 7.3 ms logic + 12.7 ms render. The decomp pays the same parse in one lump at
`end_frame`. **Same work, charged to different phases** — which is exactly the kind of comparison
that produces a wrong conclusion if the two sides are read as if they measured the same quantity.

**4. Neither runtime has the headroom for true 60 Hz today.** Both sit at ~45-50 fps-equivalent
unpaced, and running the whole tick 60 times a second needs 60. Both are ~20-25% short, and the
shortfall is in rendering.

**5. `createdPipelines` is 1800 for the decomp against 458 for the recomp** — 4x more pipeline
objects for the same scene and draw count. Pipeline creation means shader compilation. This is an
unexplained difference and a concrete lead, not a conclusion: it may be a cumulative counter
plateauing at different points, or the decomp genuinely churning pipeline state. Worth settling
before any render optimisation, since it is the largest structural difference between the two.

## Consequence for the path decision

The earlier recommendation — "go decomp because native logic is cheap enough for 60 Hz" — is **not
supported**. Logic was never the wall. Restated honestly:

* **True 60 Hz needs ~25% more render performance than either runtime has**, regardless of which one
  runs the logic.
* The decomp is still the better long-term host for 60fps (source-level control of the tick rate, no
  interpolation approximations, and 3.6x cheaper logic once render stops dominating) — but it is
  gated on render cost, not on the recompiler.
* That makes the **render path the lever for 60fps**, which is the one axis I previously called
  orthogonal to this decision. It is not orthogonal; it is the deciding factor.

## Caveat on the absolute numbers

This machine carried a load average between 6 and 31 from unrelated work during these runs, and a
back-to-back attempt at equal load produced obviously inflated figures (decomp 48-72 ms/tick, recomp
60 ms/tick) that are not usable. The two rows in the first table were taken at loads ~10 and ~6
respectively, so the decomp is if anything flattered by re-measurement, not the reverse. **Re-take
both on an idle machine before treating any absolute figure as a baseline.** The second table (draw
counts, per-draw build, drain split, pipeline counts) is structural and does not depend on load.
