#!/usr/bin/env python3
"""cadence.py — is the presented series SMOOTH, or does it judder?

WHY THIS EXISTS

Every metric this arc built so far needs to know which presents are "main" and which are "sub", and
each one therefore only works on the path it was written for. There are three 60fps paths
(docs/60fps.md) and no way to put their output on one axis. Worse, the thing a player actually
reports — "it's flickery" — is not what those metrics measure. Asymmetry says WHERE a sub-frame sits
between its neighbours; it says nothing about whether the series as a whole steps evenly, and a
series can score a perfect asymmetry while juddering.

Judder is uneven motion. So measure that directly and label nothing: take the difference between
each pair of CONSECUTIVE presents and ask whether those steps are the same size.

    step[i]   = mean |present[i+1] - present[i]|
    JUDDER    = max(step) / min(step)        1.0 = perfectly even, higher = worse
    duplicates = count of step[i] == 0       a present identical to the one before it

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

import numpy as np

WIDTH_DEFAULT = 1280


def load(path, width):
    raw = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
    if raw.size % (width * 4) != 0:
        raise SystemExit(f"REFUSES: {path} is {raw.size} bytes, not a multiple of width*4 "
                         f"({width * 4}). Wrong --width, or not an RGBA dump.")
    h = raw.size // (width * 4)
    return raw.reshape(h, width, 4)[:, :, :3].astype(np.int16)


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


def presents_per_tick(ticks):
    """The cadence itself: how many presents landed on each guest tick.

    An N-times interpolation must present EXACTLY N times per tick. A run that presents 1, 2 and 3
    times on successive ticks is juddering by construction no matter how good each individual
    in-between frame is, and no pixel metric that ignores the labels can see it — the frames are all
    different from each other, so every step is nonzero and the ratio looks respectable.
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
    ppt = presents_per_tick(ticks) if ticks else None
    if ppt is None:
        print("  PRESENTS PER TICK: unknown — these dumps carry no `-t<tick>` label, so this run "
              "cannot say whether the cadence is regular. That is not 'the cadence is fine'.")
        span = None
    else:
        counts, inner = ppt
        span = (min(counts), max(counts))
        uniq = sorted(set(inner))
        print(f"  presents per guest tick (excluding the partial first/last): {inner}")
        if len(uniq) == 1:
            print(f"  ^ REGULAR: exactly {uniq[0]} present(s) per tick.")
        else:
            print(f"  ^ IRREGULAR CADENCE: {uniq} presents per tick within one window. This is "
                  f"judder by construction — the frames are all different from each other so every "
                  f"step is nonzero and the ratio above still looks respectable, but the display is "
                  f"advancing the game by different amounts of time on consecutive refreshes.")
        print(f"  guest ticks covered: {min(counts)}..{max(counts)}")
    print(f"  SCALE: mean step {mean:.3f}. Two runs are comparable ONLY over the SAME GUEST TICKS — "
          f"SB_DUMP_FRAME_AFTER counts presents, so a 60fps run reaches a given present at half the "
          f"tick a 30fps run does, and their step sizes then describe different scenes.")
    return {"judder": judder, "mean": mean, "dups": dups, "n": n, "span": span}


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
    # 3. fast-slow alternation -> judder ~2
    r = score([6.0, 3.0, 6.0, 3.0], "alternating")
    check("fast/slow alternation reads judder ~2", abs(r["judder"] - 2.0) < 1e-6)
    # 4. too short -> REFUSES rather than scoring
    r = score([5.0], "short")
    check("a one-step series REFUSES", r is None)
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="*", help="dump prefix, e.g. scratch/render/i60_a05.rgba")
    ap.add_argument("--width", type=int, default=WIDTH_DEFAULT)
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
        frames = [load(p, a.width) for p, _ in paths]
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
            print(f"  {os.path.basename(prefix):28s} judder {j:>22s}   mean step {r['mean']:6.3f}"
                  f"   {span}")
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
                print(f"  ^ THESE SERIES DO NOT OVERLAP IN GAME TIME AT ALL (no common tick). They "
                      f"are different scenes and the numbers above are not a comparison of "
                      f"anything. Scale DUMP_AFTER by each run's presents-per-tick and re-run.")
            else:
                print(f"  ^ common guest ticks: {lo}..{hi}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
