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
objects for the same scene and draw count. Recorded here as a lead; **settled below as a dead end**:
the counter plateaus at 1824 and never grows, so it is one-time startup compilation, not per-frame
churn.

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

---

## Where the decomp's render time actually goes: a 2.26 MB/frame FIFO round-trip

Following the "render is the wall" conclusion, the decomp's 19 ms render was broken down.

**The pipeline lead is dead.** `createdPipelines` plateaus at **1824 and never grows** across a
3900-frame run — a one-time startup cost, not per-frame churn. The 4x-vs-recomp difference is in how
many distinct pipelines each scene ends up needing, not in repeated compilation. Nothing to optimise
there.

**The cost is `drain`, steady at ~15.8 ms every frame**, against ~1280 draws:

    drain=15856us draws=1182 pipelines=1824
    drain=15989us draws=1281 pipelines=1824
    drain=15871us draws=1283 pipelines=1824     (stable for the whole run)

Of that, the per-draw build accounts for ~4.3 ms (`arrayUpload` 2.64 ms, `shaderinfo+cfg` 0.62,
`pipeline_ref` 0.39, `build_uniform` 0.29, `bindgroups` 0.20, `resolve_tex` 0.10, `push_cmd` 0.08).
The remaining **~11.5 ms is the command-stream parse itself**.

**What it is parsing is a stream it just generated.** In the decomp runtime the game calls the GX API
directly, and aurora's implementation serialises it: `GXPosition3f32` is three `GX_WRITE_F32` into
the FIFO buffer (`lib/dolphin/gx/GXVert.cpp`), and `gx::fifo::drain()` then decodes the whole buffer
back at end of frame. `SB_DRAW_STATS` sizes it:

    [draw-stats] frame=3895 bytes=2260384 draws=1314 verts=18226

**2.26 MB encoded and decoded per frame.** The encode is cheap — it lands inside the ~3.2 ms game
slice, so under ~0.5 ms — while the decode costs ~11.5 ms, roughly 20x the write. That asymmetry
says the cost is per-command decode dispatch, not memory bandwidth (2.26 MB at 11.5 ms is only
~200 MB/s).

**Why this matters for 60fps.** The recomp has no choice about the FIFO: its guest writes a gather
pipe, and there is nothing else to consume. The decomp is native code and the round-trip buys it
nothing. Removing even half of that 11.5 ms takes the decomp from 22.2 ms to ~16 ms per tick, which
is inside the 16.7 ms that true 60 Hz needs — the difference between "60fps does not fit" and "it
does".

**Not started, and it is not small.** Bypassing the FIFO means giving aurora a direct submission path
for a host that calls the GX API in-process, parallel to the parse path the recomp needs. The
parse path must stay — it is the recomp's only route, and it is also the oracle both runtimes are
compared against. Scoped as its own arc rather than begun here.

Cheaper alternative worth measuring first: the decode dispatch itself. ~5 ns/byte for what is largely
a memcpy-shaped workload suggests the per-command switch, not the data movement, and a faster decode
would help BOTH runtimes rather than only the decomp.
