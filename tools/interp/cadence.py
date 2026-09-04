#!/usr/bin/env python3
"""cadence.py — is the presented series SMOOTH, or does it judder?

WHY THIS EXISTS

Most presentation metrics need to know which presents are "main" and which are "sub". This one
instead puts any consecutive series on the same axis. The thing a player reports — "it's flickery"
— is not what positional asymmetry measures: a series can score a perfect midpoint while juddering.

Judder is uneven motion. So measure that directly and label nothing: take the difference between
each pair of CONSECUTIVE presents and ask whether those steps are the same size.

    step[i]     = mean |present[i+1] - present[i]|
    JUDDER      = max(step) / min(step)      1.0 = perfectly even, higher = worse
    ALTERNATION = mean(odd steps) / mean(even steps)
    duplicates  = count of step[i] == 0      a present identical to the one before it

ALTERNATION is the sharper statistic for a two-presents-per-tick cadence and it absorbs what
tools/interp/cadence.py used to compute separately. With one in-between frame per tick the steps
come in pairs, and anything that does NOT interpolate moves only on the tick's own present and not
at all on the in-between one — so it lands entirely in every other step. The ratio between the two
phases is therefore the share of on-screen MOTION that is still snapping, weighted by screen area,
which is what the eye integrates. max/min can be dragged around by a single outlier step; the
phase means cannot.

WHAT THE NUMBERS MEAN, WITH THE TWO FAILURE MODES NAMED

  judder 1.0            every present advances by the same amount. This is what 60fps should look
                        like, and also what 30fps looks like — see the note on scale below.
  judder ~2, no dups    the in-between frame exists but is not at the midpoint: motion goes
                        fast-slow-fast-slow. This is the visible "shimmer" of a mis-placed sub-frame.
  duplicates > 0        a present that shows the previous image unchanged. At 60fps that is a
                        dropped in-between frame, and it reads as a stutter rather than a shimmer.
                        A run where EVERY OTHER step is 0 is the classic "same frame presented
                        twice" fault, which is indistinguishable from working interpolation in a
                        screenshot and obvious in motion.

SCALE IS NOT QUALITY. A judder of 1.0 on a series whose steps are all 11.0 is 30fps rendered
honestly; a judder of 1.0 on steps of 5.5 is that same motion at 60fps. So the mean step is printed
beside the ratio and two runs are only comparable at the same MEAN STEP — the same trap the
asymmetry metric documents, and the reason `--compare` prints both.

THE CONTROL

`--selftest` forces four synthetic series and asserts the statistic separates them: even motion must
read ~1.0; every-other-frame-duplicated must be caught by the duplicate count and not merely by a
large ratio; a fast-slow alternation must read ~2; and a single-present series must REFUSE rather
than report a ratio of nothing. Exits non-zero on failure; wired into tools/selftest_all.py.
"""
import argparse
import glob
import os
import sys
from pathlib import Path

import numpy as np

WIDTH_DEFAULT = 1280


def fraction_box(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise SystemExit(f"REFUSES: --crop wants x0,y0,x1,y1 as four fractions, got {text!r}.")
    try:
        v = [float(x) for x in parts]
    except ValueError:
        raise SystemExit(f"REFUSES: --crop values must be numbers, got {text!r}.")
    if not all(0.0 <= x <= 1.0 for x in v) or v[0] >= v[2] or v[1] >= v[3]:
        raise SystemExit(f"REFUSES: --crop {text!r} is not a box inside the frame (each value in "
                         f"0..1, x0<x1, y0<y1).")
    return tuple(v)


def load(path, width, crop=None):
    raw = np.frombuffer(Path(path).read_bytes(), dtype=np.uint8)
    if raw.size % (width * 4) != 0:
        raise SystemExit(f"REFUSES: {path} is {raw.size} bytes, not a multiple of width*4 "
                         f"({width * 4}). Wrong --width, or not an RGBA dump.")
    h = raw.size // (width * 4)
    img = raw.reshape(h, width, 4)[:, :, :3].astype(np.int16)
    if crop is None:
        return img
    x0, y0, x1, y1 = crop
    # Fractions of the frame, not pixels: a crop written in pixels silently means a different part
    # of the picture at a different window size, and these dumps are whatever SB_W/SB_H were set to.
    a, b = round(y0 * h), round(y1 * h)
    c, d = round(x0 * width), round(x1 * width)
    if b - a < 8 or d - c < 8:
        raise SystemExit(f"REFUSES: --crop {x0},{y0},{x1},{y1} leaves {d - c}x{b - a} pixels, which "
                         f"is too small to say anything about motion. A crop that measures almost "
                         f"nothing still prints a confident number.")
    return img[a:b, c:d]


def series_paths(prefix):
    """Every dump in the series, in PRESENT order, with the GUEST TICK each was taken at.

    Ordered by the numeric index the dumper writes, never by filename sort: `10` sorts before `2`
    as text, and a series silently reordered produces a judder number that is pure artefact.

    The tick comes from the dump's own label (`...<n>.<role>-t<retrace>`). It is not decoration.
    SB_DUMP_FRAME_AFTER counts PRESENTS, so a run presenting twice per tick reaches present 2400 at
    HALF the guest tick a run presenting once does — measured: an uninterpolated run dumped at
    present 2400 sat at retrace 4802 while a 60fps run dumped at the same present sat at 2402, two
    entirely different moments in the game. Comparing their step sizes compares two scenes.
    """
    files = glob.glob(prefix + ".*")
    out = []
    for f in files:
        tail = f[len(prefix) + 1:]
        idx = tail.split(".")[0]
        if not idx.isdigit():
            continue
        tick = None
        if "-t" in tail:
            t = tail.rsplit("-t", 1)[1]
            if t.isdigit():
                tick = int(t)
        out.append((int(idx), f, tick))
    out.sort()
    return [(f, t) for _, f, t in out]


def steps_of(frames):
    return [float(np.abs(frames[i + 1] - frames[i]).mean()) for i in range(len(frames) - 1)]


def presents_per_label(ticks):
    """How many presents carry each distinct `-t<n>` label.

    THIS IS NOT THE PRESENT CADENCE, and an earlier version of this function said it was. The label
    is the GAME's own retrace counter, which the game advances by however many NTSC fields it asked
    for this frame — usually 2, but not always, and it does not have to change at all. So two
    consecutive ticks can share a label, and a run presenting exactly twice per tick then shows a
    label carrying 4 presents. Reading that as "4 presents in one tick" is reading the counter's
    variable step as a cadence irregularity.

    Measured, on the run this was first drawn from: the labels grouped as [1, 2, 3] while the
    runtime's own counters reported 6000 in-between frames for 6000 simulation ticks — exactly two
    presents per tick, perfectly regular. The verdict was wrong and the numbers it came from were
    right, which is the combination that is hardest to notice.

    THE AUTHORITY FOR CADENCE IS THE RUNTIME, NOT THE FILENAMES: `SBR_LUCENT_DEBUG=interp` prints
    "N simulation tick(s), M in-between frame(s) presented". What this function is still good for is
    checking that a series covers the guest ticks you think it does, which is what makes two runs
    comparable at all.
    """
    if any(t is None for t in ticks):
        return None
    counts = {}
    for t in ticks:
        counts[t] = counts.get(t, 0) + 1
    # The first and last tick in the window are partial by construction (the dump starts and stops
    # mid-tick), so they cannot be evidence of irregularity and are excluded.
    inner = [counts[t] for t in sorted(counts)[1:-1]]
    return counts, inner


def score(steps, label="", ticks=None):
    n = len(steps)
    if n < 2:
        print(f"REFUSES{(' ' + label) if label else ''}: {n + 1} present(s) gives {n} step(s); a "
              f"cadence needs at least 2 steps to compare. Nothing is scored.")
        return None
    dups = sum(1 for s in steps if s == 0.0)
    lo, hi = min(steps), max(steps)
    mean = sum(steps) / n
    judder = float("inf") if lo == 0.0 else hi / lo
    # Phase means. Which phase is the in-between one is not known here and does not matter: the
    # ratio is reported as >= 1 so it reads the same either way.
    even = [s for i, s in enumerate(steps) if i % 2 == 0]
    odd = [s for i, s in enumerate(steps) if i % 2 == 1]
    me = sum(even) / len(even) if even else 0.0
    mo = sum(odd) / len(odd) if odd else 0.0
    altern = float("inf") if min(me, mo) == 0.0 else max(me, mo) / min(me, mo)
    print(f"  steps ({n}): " + " ".join(f"{s:.3f}" for s in steps))
    print(f"  mean step {mean:.3f}   min {lo:.3f}   max {hi:.3f}")
    if dups:
        every_other = dups >= (n // 2) and all(s == 0.0 for s in steps[::2]) or \
                      dups >= (n // 2) and all(s == 0.0 for s in steps[1::2])
        print(f"  DUPLICATE PRESENTS: {dups} of {n} steps are EXACTLY ZERO — those presents show "
              f"the previous image unchanged.")
        if every_other:
            print("  ^ EVERY OTHER step is zero. That is the 'same frame presented twice' fault, "
                  "not interpolation: it looks identical in a screenshot and stutters in motion.")
        print("  JUDDER: undefined (a zero step makes the ratio infinite). Fix the duplicates "
              "first; the ratio says nothing until then.")
    else:
        print(f"  JUDDER {judder:.2f}   (1.00 = every present advances equally; ~2 = the in-between "
              f"frame exists but sits off the midpoint, which is the visible shimmer)")
        print(f"  ALTERNATION {altern:.2f}   phase means {me:.3f} / {mo:.3f}   (1.00 = both presents "
              f"advance the picture equally. Higher = that much of the on-screen motion moves on "
              f"only one of the two presents, i.e. is still SNAPPING rather than interpolating.)")
        if altern >= 1.8:
            print("  ^ RUN THE CONTROL BEFORE READING THIS. With interpolation OFF, consecutive "
                  "presents are consecutive ticks and alternation MUST come out ~1.0. If it does "
                  "not, the scene itself pulses on a two-frame cycle (a flashing effect, a 2-frame "
                  "animation) and this number is measuring that instead.")
            print(f"  ^ AND CHECK THE SCENE IS MOVING. Mean step here is {mean:.3f}. This statistic "
                  f"is the share of on-screen CHANGE landing on one of the two presents, and in a "
                  f"scene with little GEOMETRIC motion that change is dominated by things no "
                  f"geometry interpolation can smooth — an animated texture, an EFB copy, a 2D "
                  f"layer, all of which update once per tick by design. Measured on Delfino: a "
                  f"near-static camera gives alternation 6.24 at mean step 2.16 and 15.30 at mean "
                  f"step 0.88, while the SAME build with the camera rotating "
                  f"(SBR_PAD_SCRIPT=\"400:CSTICK=100/0\") gives 1.19 at mean step 13.34. The "
                  f"first two are not a regression; they are this metric measuring the texture "
                  f"update rate. Drive the camera before concluding anything from a high value.")
    ppt = presents_per_label(ticks) if ticks else None
    if ppt is None:
        print("  RETRACE LABELS: absent — these dumps carry no `-t<n>` label, so this series "
              "cannot be checked for moment overlap against another. That is not 'it overlaps'.")
        span = None
    else:
        counts, inner = ppt
        span = (min(counts), max(counts))
        print(f"  presents per retrace label (excluding the partial first/last): {inner}")
        print("  ^ NOT the present cadence. The label is the game's own retrace counter and it "
              "advances by however many fields the game asked for that frame, so consecutive ticks "
              "can share one. For the cadence, read the runtime: SBR_LUCENT_DEBUG=interp prints "
              "simulation ticks against in-between frames presented.")
        print(f"  guest retrace labels covered: {min(counts)}..{max(counts)}")
    print(f"  SCALE: mean step {mean:.3f}. Two runs are comparable ONLY over the SAME GUEST TICKS — "
          f"SB_DUMP_FRAME_AFTER counts presents, so a 60fps run reaches a given present at half the "
          f"tick a 30fps run does, and their step sizes then describe different scenes.")
    return {"judder": judder, "altern": altern, "mean": mean, "dups": dups, "n": n, "span": span}


def selftest():
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print(f"  {'PASS' if cond else 'FAIL'}  {name}{(' — ' + detail) if detail else ''}")
        ok = ok and cond

    print("cadence.py --selftest")
    # 1. even motion -> judder ~1
    r = score([4.0, 4.0, 4.0, 4.0], "even")
    check("even motion reads judder ~1", abs(r["judder"] - 1.0) < 1e-6)
    # 2. every-other duplicated -> caught as duplicates, NOT merely as a big ratio
    r = score([0.0, 8.0, 0.0, 8.0], "dup")
    check("every-other-duplicate is caught by the duplicate count", r["dups"] == 2)
    # 3. fast-slow alternation -> judder ~2 AND alternation ~2
    r = score([6.0, 3.0, 6.0, 3.0], "alternating")
    check("fast/slow alternation reads judder ~2", abs(r["judder"] - 2.0) < 1e-6)
    check("fast/slow alternation reads ALTERNATION ~2", abs(r["altern"] - 2.0) < 1e-6)
    # 3b. even motion must NOT read as alternating — the statistic has to separate both classes,
    # not merely fire on the positive one.
    r = score([4.0, 4.0, 4.0, 4.0], "even-alt")
    check("even motion reads alternation ~1", abs(r["altern"] - 1.0) < 1e-6)
    # 3c. A single outlier step must move JUDDER strictly more than ALTERNATION — that is the
    # reason alternation exists as a separate number, and it is the claim that can actually be
    # asserted. An earlier version of this case asserted `altern < 1.3` and FAILED at 1.42: over
    # six steps one outlier still shifts a phase mean by 42%, because it is one of only three
    # samples in its phase. The property is the ORDERING, not an absolute threshold, and the
    # threshold version would have quietly encoded a series length into the test.
    r = score([4.0, 4.0, 9.0, 4.0, 4.0, 4.0], "outlier")
    check("one outlier moves judder more than alternation",
          r["judder"] > r["altern"] * 1.4,
          f"judder {r['judder']:.2f} vs alternation {r['altern']:.2f}")
    # ...and over a longer series the outlier's effect on alternation must SHRINK, which is the
    # property that makes it the more robust of the two.
    long_series = [4.0] * 20
    long_series[7] = 9.0
    r2 = score(long_series, "outlier-long")
    check("alternation is more robust the longer the series",
          r2["altern"] < r["altern"], f"{r2['altern']:.2f} < {r['altern']:.2f}")
    # 4. too short -> REFUSES rather than scoring
    r = score([5.0], "short")
    check("a one-step series REFUSES", r is None)
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="*", help="dump prefix, e.g. scratch/render/i60_a05.rgba")
    ap.add_argument("--width", type=int, default=WIDTH_DEFAULT)
    # WHICH PART OF THE PICTURE. The whole-frame numbers are area-weighted, so one full-screen or
    # corner element that snaps can dominate a frame in which everything else interpolates — which
    # is not a hypothetical: measured ALTERNATION 6.26 whole-frame against a scene the per-draw
    # audit scored at 99.7% interpolating. Cropping is how those two are reconciled rather than
    # argued about.
    ap.add_argument("--crop", type=fraction_box, default=None,
                    help="x0,y0,x1,y1 as FRACTIONS of the frame (e.g. 0.15,0.15,0.85,0.8 for the "
                         "world without the HUD corners)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.prefix:
        ap.error("give at least one dump prefix (or --selftest)")

    results = []
    for prefix in a.prefix:
        paths = series_paths(prefix)
        print(f"\n{prefix}  —  {len(paths)} present(s)")
        if len(paths) < 3:
            # REFUSE loudly. A missing series and a static scene both produce "no judder".
            print(f"  REFUSES: found {len(paths)} dump(s) matching '{prefix}.*'. That is not a "
                  f"series. Check the run actually dumped (SB_DUMP_FRAME_EVERY=1, _COUNT>=3).")
            continue
        frames = [load(p, a.width, a.crop) for p, _ in paths]
        for p, _ in paths:
            print(f"    {os.path.basename(p)}")
        r = score(steps_of(frames), prefix, [t for _, t in paths])
        if r:
            results.append((prefix, r))

    if len(results) > 1:
        print("\n=== COMPARE ===")
        for prefix, r in results:
            j = "undefined (duplicates)" if r["dups"] else f"{r['judder']:.2f}"
            span = f"ticks {r['span'][0]}..{r['span'][1]}" if r["span"] else "ticks UNKNOWN"
            a = "undefined" if r["dups"] else f"{r['altern']:.2f}"
            print(f"  {os.path.basename(prefix):26s} judder {j:>10s}  alternation {a:>9s}"
                  f"  mean step {r['mean']:6.3f}   {span}")
        # OVERLAP IS THE PRECONDITION, and it is checked rather than assumed. Two runs dumped at the
        # same PRESENT index are at different guest ticks whenever their presents-per-tick differ,
        # which is exactly the case being compared here.
        spans = [r["span"] for _, r in results]
        if any(s is None for s in spans):
            print("  ^ at least one series carries no tick labels, so this comparison CANNOT be "
                  "checked for moment overlap. Do not read it.")
        else:
            lo = max(s[0] for s in spans)
            hi = min(s[1] for s in spans)
            if lo > hi:
                print("  ^ THESE SERIES DO NOT OVERLAP IN GAME TIME AT ALL (no common tick). They "
                      "are different scenes and the numbers above are not a comparison of "
                      "anything. Scale DUMP_AFTER by each run's presents-per-tick and re-run.")
            else:
                print(f"  ^ common guest ticks: {lo}..{hi}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
