# 2026-08-06 — the motion census, and the first trustworthy score for record-and-replace

The previous commit ended by naming the blocker: *every camera probe in this arc is blind, and
identifying the real gameplay camera is the prerequisite for grading camera interpolation at all.*

That framing was wrong, and usefully so. The thing the arc actually needed was not a better camera
probe — it was a liveness instrument that watches **no named object**, because every probe that
watches one can be pointed at the wrong one and will then report a plausible negative forever.

## The instrument: `SBR_INTERP60_CENSUS=1`

`sbr_i60r_census()` (`sms-recomp/overrides/interp60_replace.cpp`) buckets `|cur - prev|` over the
TRANSLATION of every draw matrix recorded that tick, and prints one greppable line per 300 ticks.
It measures the matrices the hardware is about to read, so "nothing moved" from it is a statement
about the drawn geometry rather than about whichever object an instrument happened to follow.

It is available independently of the interpolation (`SBR_INTERP60_CENSUS=1` with no
`SBR_INTERP60`), because the run that interpolates nothing is the baseline every other reading is
compared against. `tools/interp/interp60_run.sh` now prints the census window covering the DUMPED
MOMENT — selected by guest tick, not by taking the end of the run — before it prints any score, and
the CAMTRACE block is demoted to a footnote that says outright that its negatives mean nothing.

**Run against both classes**, same scenario, same 300-tick windows, differing only in the pad script:

| bucket | camera held rotating | no input at all |
|---|---|---|
| `<100` | 128,183 | 11,247 |
| `<1e4` | **115,991** | **0** |
| `>=1e4` | 600 | 23,700 |

The verdict is the `<1e4` bucket. `>=1e4` is excluded and the code says why: it is a constant
garbage population (one model's unused matrix slots) that appears in every window of every run and
runs BACKWARDS between the two classes. The first version of this verdict summed all three buckets
and could therefore never fire — it read "moving" on a dead scene, which is the exact failure the
instrument exists to prevent.

## What the census found immediately: the standing pad script measured nothing

The runner's old default walked Mario forward. Measured:

    400:STICK=0/100              (walk only)   <1e4 bucket: 0 for the whole run
    400:CSTICK=110/0             (camera only) <1e4 bucket: ~130k every window
    400:STICK=0/100+CSTICK=110/0 (both)        <1e4 bucket: ~125k every window   <-- new default

A two-step script that walks first and engages the camera later (the previous session's
`2000:STICK=0/0+CSTICK=110/0`) produces motion for ~300 ticks and then dies: the camera stops
responding once Mario is parked where the walk left him. Held from one step, it does not.

This session's first three runs — alphas 0.0, 0.5 and 1.0 — produced **identical scores to the
digit**, because the moment they sampled was static. Nothing in the harness said so at the time.

## The first trustworthy score for record-and-replace

Camera rotating 65 units/tick, moment scale 89.5% px / 4.45 mad, `DUMP_AFTER=2400`:

| alpha | asymmetry [px] | asymmetry [mad] | lead | off-segment [px] |
|---|---|---|---|---|
| 0.0 | −53.6% | −41.0% | 0.302 | +35.3% |
| 0.5 | +5.2% | +24.0% | 0.514 | +85.8% |
| 1.0 | +96.6% | +99.2% | 0.966 | +3.6% |

Monotone in alpha, correct sign in both directions. The `alpha = 1.0` endpoint is nearly exact:
`sub -> next` is 3.2% of pixels, mad 0.150 — down from the 5.03% the substitution path last scored,
and its residual is now LOCAL (14% tile coverage, 90.7% top-decile) rather than spread.

## The residual is asymmetric, and that names its cause

`alpha = 0.0` should reproduce the preceding main frame exactly. It does not: 36.7% of pixels,
mean 4.41/channel, against a full tick of 11.42. So **~39% of a tick's visible change is carried by
content the matrix path does not move**, and because that content sits at the CURRENT tick's state
whatever alpha is, it contributes zero error at alpha=1 and a full tick's error at alpha=0. That is
the whole of the asymmetry above, and it is also why alpha=0.5 is off-segment by +86%: the frame is
a mixture of two different instants, not a point on the path between them.

`scratch/screenshots/i60_a0_residual.png` shows it directly. The plaza GROUND is black — it returns
to the previous tick exactly. Bright: the buildings and palms, the sea, the distant island, Mario's
silhouette, and the subtitle box.

## FALSIFIED with a control: the frozen matrices are not a hidden population

37% of translation elements have EXACTLY zero delta per tick. The obvious reading — that these are
world matrices whose objects the lerp therefore cannot move — is wrong, and it was tested rather
than argued. `SBR_INTERP60_REPLACE_KICK_ONLY=frozen|moving` displaces a 300-unit kick into only one
of the two populations:

    kick frozen matrices only :  0.00% of pixels move
    kick moving matrices only : 96.33% of pixels move
    kick everything           : 96.33% of pixels move

Exhaustive, and the frozen half is invisible. Every DRAWN matrix moves between ticks and the lerp
reaches all of them. The 2026-08-06 retraction was right about this for a reason it had not yet
tested; it is now tested.

Two further off-by-one theories were killed the same way. Extrapolating the matrix alpha past both
endpoints (`SBR_INTERP60_REPLACE_ALPHA=-1.0` and `2.0`) gives residuals against the previous main
frame of 11.57 and 15.32 against alpha=0.0's 4.73 — alpha=0.0 IS the minimum, so the prev/cur
pairing is one tick apart and correctly aligned.

## Ablation: the camera pose lerp is nearly inert here

At alpha = 0.0, with the other half pinned:

| configuration | asymmetry [mad] |
|---|---|
| matrices at prev, camera pinned at cur | −34.5% |
| matrices pinned at cur, camera at prev | +92.3% |
| both | −41.0% |

The matrix replacement does essentially all the work; the camera pose lerp contributes ~6 points.
That contradicts the comment in `interp60_snapshot.cpp` claiming the camera lerp is load-bearing
because "most of a Delfino frame is static geometry drawn with the view matrix loaded directly" —
that reading came from a measurement taken before the matrix path worked, and the geometry it
describes is in fact reached by `mDrawMtxBuf` like everything else.

## OPEN, and this is the next defect

**Which content carries the ~39%?** It is not the drawn matrices — those are covered, paired and
verified. The candidates the path documents as uncovered are the projection matrix, texture and
bump matrices, J2D/ortho, JPA particles and immediate-mode geometry. The subtitle box in the
residual map is J2D and is accounted for; the buildings, palms, sea and island are NOT explained by
any of them yet, and no theory about them should be recorded here until one is run against both
classes. The instrument to build is the one that attributes a screen region to the draw that
produced it — this arc has now paid twice for attributing a region by elimination.

`lookup_concat_replacement(lhs, rhs)` in dusklight exists because some draw sites resolve a
view ⊗ world pair rather than a single final matrix. If SMS has such sites, that is where to look.

## Reproduce

    DUMP_AFTER=2400 tools/interp/interp60_run.sh <tag> <alpha> SBR_INTERP60_REPLACE=1

Read the MOTION CENSUS line the runner prints before the score. A score taken where it says STATIC
describes the scene, not the interpolation.
