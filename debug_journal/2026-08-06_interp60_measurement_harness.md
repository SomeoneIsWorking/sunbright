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
