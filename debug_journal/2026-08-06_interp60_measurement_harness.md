# 2026-08-06 — the 60fps sub-frame: a measurable metric, and what it says once it is trustworthy

The 60fps arc had been steered for several sessions by a number — "asymmetry", how far the
interpolated sub-frame sits from each of its two main-frame neighbours — that no committed tool
produced. It was recomputed by hand in each session from an uncommitted diff loop. This entry is
about closing that, and about the three separate ways the resulting measurement turned out to be
measuring nothing until each was fixed.

## The tools

* `tools/interp/subframe_position.py` — scores a sub-frame's position between its neighbours from
  ONE run's labelled present series. Reports, per `main -> sub -> main` triple:

      a = d(prev, sub)   b = d(sub, next)   c = d(prev, next)
      asymmetry   = (a - b) / c      0 = centred, +1 = duplicates the FOLLOWING main frame
      lead        = a / (a + b)
      off-segment = (a + b) / c - 1  how far the sub-frame lies OFF the path between them

  Both a pixel-count and a mean-absolute-difference metric, because neither is linear in
  displacement. `--compare A B` scores two runs at the same present index. `--selftest` forces five
  cases (true midpoint ≈ 0, duplicate-of-next +100%, duplicate-of-prev −100%, static tick REFUSES,
  and a third-direction sub-frame that asymmetry calls centred while off-segment catches it).
* `tools/interp/interp60_run.sh` — one runner carrying the whole switch set, plus a camera-liveness
  block read before any score.
* `tools/selftest_all.py` — runs every tool's `--selftest`; wired into `.githooks/pre-commit`.
  Three tools already carried one and nothing had ever run them.

## Three ways the measurement measured nothing

**1. The sub-frame was never copied out.** Without `SBR_INTERP60_COPY=1` AND
`SBR_PRESENT_AFTER_COPY=1` the sub-frame renders into the EFB, nothing copies it out, and the
"sub" present shows the previously copied XFB — every sub dump BIT-IDENTICAL to the main frame
before it. That is also a real failure mode of the interpolation, and the pixels cannot tell the
two apart. First reading taken with the new tool: a confident −100% asymmetry, from a run where
nothing was being shown. The runner now carries both switches; the scorer names the signature; and
`interp60_gate.sh` had the same hole and now sets them too.

**2. The canonical dump moment has a PARKED CAMERA.** At present 1600, runs at alpha 0.0 and alpha
1.0 are byte-identical in every present — while the scene moves 42% of the frame per tick. The
camera is the only thing this configuration interpolates, and `camera_apply` reports
|eye cur−prev| = 0.000 there: the 42% is Mario walking. A frame-level motion figure cannot see
this, so the runner now arms `SBR_INTERP60_CAMTRACE` at the dump present and prints the camera's
own per-tick separation before any score, naming a parked camera outright. The journal already
records this exact mistake once (a "fast test moment" chosen for speed moved the measurement into
a pre-gameplay window and produced a wrong root cause); it is now caught mechanically.

**3. The stream-hash probe capped by first-N.** Its first six sub-frames were whatever the run
started with, and a sub-frame where the camera is still MUST hash the same at every alpha — six
agreeing lines read exactly like six lines proving agreement. It now caps by novelty (moving-camera
sub-frames), prints |cam cur−prev| on every line, and says when that is zero.

## The positive control

At presents 60-68, camera provably moving at 19.664 units/tick, `SBR_INTERP60_STREAMHASH` over the
sub-frame's own emitted GX stream:

    sub-frame #1  alpha=0.00  711520 bytes  FNV-1a 3ae947da5fa2dcf2
    sub-frame #1  alpha=1.00  711520 bytes  FNV-1a 973f4a93c419f9a7
    ... #2..#8 likewise, every pair identical in SIZE and different in BYTES

Identical byte counts with different hashes: the substitution reaches the emitted artifact, not
just guest memory.

## What the measurement then says

Same moment, `SBR_INTERP60_PREENTRY_VC=1`, the one usable triple (presents 5..7; the other two are
a near-static tick and a scene cut, both flagged by the tool):

    alpha   prev->sub   sub->next   full tick   asymmetry   off-segment
    0.0        6.381      14.383      10.628      -75.3%       +95.4%
    0.5       13.681      13.590      10.628       +0.9%      +156.6%
    1.0       14.376       5.029      10.628      +87.9%       +82.6%

Cross-run, alpha 0.0 vs 1.0: MAIN presents byte-identical (the no-leak invariant holds exactly),
SUB presents differ by 14.5%.

**Asymmetry is monotone in alpha and crosses zero at 0.5.** The sub-frame moves toward the
preceding main frame as alpha falls and toward the following one as alpha rises, which is the
correct sign in both directions and is the first time that has been shown with a control rather
than argued from one endpoint.

**And centred is not correct.** At alpha = 0.5 the sub-frame is 13.68 from one neighbour and 13.59
from the other while those neighbours are only 10.63 apart: it is further from each of them than
they are from each other. Off-segment +157%. A perfectly centred asymmetry, and an image that is
not on the path at all.

That is the residual, and it is the thing the old metric structurally could not report. The arc
drove asymmetry 74.5% → 25.6% → 20.4% and would have read ~0% as success. Off-segment is large at
every alpha including the endpoints: at alpha = 1.0, where the sub-frame renders the game's own
pose and `sub -> next` should be ~0, 5.03% of pixels still differ.

## Next

The next defect is named and has a number: **at alpha = 1.0, `sub -> next` = 5.03% of pixels
(mad 0.173) where it must be ~0.** That is an endpoint identity failure inside a single run — no
cross-run comparison, no cadence drift, no scene-moment ambiguity — so it is the cheapest thing in
this arc to bisect, and everything off-segment is downstream of it.

Do NOT re-derive the earlier table. "74.5% → 25.6% → 20.4%" was recorded without its run
configuration or its moment scale, and asymmetry is a ratio whose denominator is how far the tick
moved; readings taken at different moments are not comparable. Re-measure with the runner, which
records both.

## OPEN, and measured: the PreEntry view-calc pass is NOT inert with respect to the MAIN frame

Bisecting the endpoint residual by turning `SBR_INTERP60_PREENTRY_VC` off at alpha = 1.0 produced a
result about something else entirely. Comparing the two runs present-by-present
(`subframe_position.py --compare`):

    present 0 [ sub] : 99.666% differ, mad 22.965%
    present 1 [main] :  0.805% differ, mad  0.313%
    present 2 [ sub] : 99.657% differ, mad 22.966%
    present 3 [main] :  0.805% differ, mad  0.314%
    present 4 [ sub] : 99.663% differ, mad 22.958%
    present 5 [main] : 99.663% differ, mad 23.060%      <- a MAIN frame, 99.7% different
    present 7 [main] : 99.656% differ, mad 23.067%

A main frame must not depend on what the sub-frame does. Two of the four main presents differ by
99.7% between view-calc on and off, so the pass changes the frame the GAME renders — it is not a
matrix recompute that cancels itself.

**Why the previous session's leak test could not see this.** That test compares two runs that
differ only in ALPHA, with the pass ON in both. A defect the pass causes at every alpha is common
to both sides and cancels exactly. It reported 0/0 px and was right about what it measured. A leak
gate needs a baseline with the seam OFF, not only a second alpha.

This is recorded as MEASURED and NOT EXPLAINED. The two runs also sit at different moment scales
(full tick 76.5 vs 10.6 at the same present index), which is itself consistent with a state
divergence rather than a render-only difference, and the sub presents differ by 99.7% everywhere
while the first two main presents differ by only 0.8% — a pattern no single story yet accounts for.
Do not treat any of it as a root cause. The next step is the cheap one: run the leak comparison
with the pass on vs off at a FIXED alpha and find the first present where the mains diverge.
