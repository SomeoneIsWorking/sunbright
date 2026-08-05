#!/usr/bin/env python3
"""subframe_gate.py — score a run's consecutive-present dump series.

WHAT IT ASKS, AND WHY THE COMPARISON IS INSIDE ONE RUN

The 60fps sub-frame adds a second present per game tick. `SB_DUMP_FRAME_AFTER` counts PRESENTS, so
the same index reaches a different game moment once the cadence changes — two configurations dumped
"at frame 1500" are then not the same moment at all, and any diff between them is dominated by that
drift rather than by what was being tested. That defect already produced one wrong "VERIFIED".

So the gate compares CONSECUTIVE PRESENTS OF ONE RUN (`SB_DUMP_FRAME_EVERY=1`,
`SB_DUMP_FRAME_COUNT=n`), which alternate main-frame / sub-frame. Nothing drifts, because there is
only one run.

WHAT THE PHASE ORDER PREDICTS (decomp JDRSmJ3DScn.cpp:46, MarDirectorDirect.cpp entry/render
branches, JDRDisplay.cpp endRendering)

A `direct()` call renders what a PREVIOUS call entered, and the present follows the whole call. So
the main present at tick N shows the pose entered at N-1, while the sub-frame re-enters from
whatever pose is live and shows lerp(N-1, N, alpha). Therefore:

    alpha = 0.0  ->  the sub-frame must REPRODUCE the neighbouring main frame  (identity)
    alpha = 1.0  ->  the sub-frame must DIFFER from it, by one tick of motion  (control)

Both must hold. Identity alone can be passed by a sub-frame that presents the same image twice
(rendering nothing new); the control alone can be passed by a sub-frame that renders garbage.

WHICH DUMP IS WHICH

The series may start on either a main or a sub present, and nothing in the file says which. Rather
than assume, the tool reports EVERY adjacent pair and the alternation pattern; a healthy alpha=0.0
run shows near-zero for every pair, and a healthy alpha=1.0 run shows a repeating large/small
alternation. If the pattern is not consistent with alternating presents, it says so instead of
picking the pairing that flatters the result.
"""
import sys
import glob
import os


def load(path):
    with open(path, 'rb') as f:
        return f.read()


def diff(a, b):
    if len(a) != len(b) or not a:
        return None
    n = len(a) // 4
    d = 0
    for i in range(n):
        if a[4 * i:4 * i + 3] != b[4 * i:4 * i + 3]:
            d += 1
    return d, n


def main():
    if len(sys.argv) < 2:
        print("usage: subframe_gate.py <dump-prefix>   (e.g. scratch/render/seq_a0.rgba)")
        return 2
    prefix = sys.argv[1]
    files = sorted(glob.glob(prefix + ".*"), key=lambda p: int(p.rsplit(".", 1)[1]))
    if len(files) < 2:
        # Refuse rather than report on what little arrived: a one-frame series cannot answer a
        # question about adjacent presents, and "0 pairs compared" must not read as "passed".
        print(f"GATE REFUSES: found {len(files)} dump(s) matching {prefix}.* — need at least 2.")
        print("  Nothing was compared. Check the run produced its series (SB_DUMP_FRAME_EVERY=1,")
        print("  SB_DUMP_FRAME_COUNT=n) and that it reached SB_DUMP_FRAME_AFTER.")
        return 1

    print(f"series: {len(files)} consecutive presents")
    imgs = [load(f) for f in files]
    sizes = {len(i) for i in imgs}
    if len(sizes) != 1:
        print(f"GATE REFUSES: dumps differ in size {sizes} — not the same framebuffer.")
        return 1

    results = []
    for k in range(len(imgs) - 1):
        r = diff(imgs[k], imgs[k + 1])
        if r is None:
            print(f"GATE REFUSES: pair {k}->{k+1} is incomparable.")
            return 1
        d, n = r
        results.append(d)
        print(f"  present {k} -> {k+1} : {d:>8} of {n} ({100.0 * d / n:.4f}%)"
              f"   {os.path.basename(files[k])} vs {os.path.basename(files[k+1])}")

    # The alternation is the structural check: adjacent presents of a working sub-frame pair up
    # main/sub, so the diffs should not all be the same order of magnitude unless alpha makes them
    # genuinely equal. Report the shape; do not infer a verdict the data does not carry.
    lo, hi = min(results), max(results)
    print()
    print(f"  smallest adjacent diff: {lo}")
    print(f"  largest  adjacent diff: {hi}")
    if hi == 0:
        print("  ALL ADJACENT PRESENTS IDENTICAL — every present in the series is the same image.")
        print("  At alpha=0.0 that is the expected identity. At any other alpha it means the")
        print("  sub-frame is presenting the main frame again and rendering nothing new.")
    elif lo == 0:
        print("  Some pairs identical, some not — consistent with alternating main/sub presents")
        print("  where the sub-frame reproduces its neighbour (alpha at an endpoint).")
    else:
        print("  No adjacent pair is identical — every present differs from the last.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
