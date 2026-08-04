#!/usr/bin/env python3
"""present_evenness.py — how much of the picture actually interpolates, measured in PIXELS.

WHAT IT ANSWERS. Interpolated 60fps presents each 30 Hz tick twice: an in-between image then the
tick's own. If everything interpolated, consecutive presents would advance the picture by equal
amounts. Whatever still SNAPS moves only on the true present and not at all on the in-between one,
so it lands entirely in every other step. The size of that imbalance is the share of on-screen
motion that is not yet interpolating.

WHY NOT THE EXISTING METRIC. frame_smoothness (SBR_SMOOTH) scores whole screen CELLS and reports a
per-cell alternation, which is dominated by whichever geometry fills the cell and cannot see a
low-alpha overlay at all. This works on raw pixels over the whole frame, so it weights by SCREEN
AREA — which is what the eye actually integrates — rather than by draw count.

THE CONTROL IS MANDATORY AND CHEAP. Run it once with 60fps OFF, where consecutive presents are
consecutive ticks and the ratio MUST come out ~1.0. A ratio far from 1.0 in that run means the scene
itself pulses (an animation on a 2-frame cycle, a flashing effect) and every number from the 60fps
run is then measuring that instead. This script refuses to interpret a run without telling you so.

USAGE
    # control: presents == ticks, expect ratio ~1.0
    SB_HEADLESS=1 SB_TURBO=1 SBR_FASTBOOT=1 SBR_STAGE=1 SBR_SCENARIO=0 \\
        SBR_PAD_SCRIPT="150:STICK=0/-90" \\
        SB_DUMP_FRAME=scratch/flash/f.rgba SB_DUMP_FRAME_EVERY=1 SB_DUMP_FRAME_AFTER=300 \\
        ./run-recomp.sh
    python3 tools/present_evenness.py scratch/flash 310 350

    # then the same with SBR_60FPS=1 and compare

The scene must be MOVING or there is nothing to measure — drive the stick with SBR_PAD_SCRIPT.
"""
import os
import sys


def frame_diff(a: bytes, b: bytes, stride_px: int = 32) -> float:
    """Mean per-channel absolute difference, sampling every `stride_px` pixels."""
    n = min(len(a), len(b))
    total = 0
    count = 0
    for off in range(0, n - 3, 4 * stride_px):
        total += abs(a[off] - b[off]) + abs(a[off + 1] - b[off + 1]) + abs(a[off + 2] - b[off + 2])
        count += 3
    return total / count if count else 0.0


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    directory, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    if not os.path.isdir(directory):
        # Refuse rather than report "no frames": a missing directory and an empty one are the same
        # silence otherwise, and the missing one is the far more likely mistake.
        print(f"ERROR: {directory} does not exist. Nothing was measured.", file=sys.stderr)
        return 1
    names = [f for f in os.listdir(directory) if f.startswith("f.rgba.")]
    idx = sorted(int(f.rsplit(".", 1)[-1]) for f in names)
    idx = [i for i in idx if lo <= i <= hi]
    if len(idx) < 9:
        print(f"ERROR: only {len(idx)} frames in [{lo},{hi}] of {len(names)} present in "
              f"{directory}; need at least 9 for an even/odd split to mean anything.",
              file=sys.stderr)
        return 1

    diffs = []
    prev = None
    for i in idx:
        cur = open(os.path.join(directory, f"f.rgba.{i}"), "rb").read()
        if prev is not None:
            diffs.append(frame_diff(prev, cur))
        prev = cur

    even = [d for k, d in enumerate(diffs) if k % 2 == 0]
    odd = [d for k, d in enumerate(diffs) if k % 2 == 1]
    mean_even = sum(even) / len(even)
    mean_odd = sum(odd) / len(odd)
    hi_m, lo_m = max(mean_even, mean_odd), min(mean_even, mean_odd)
    ratio = hi_m / lo_m if lo_m > 1e-9 else float("inf")

    print(f"presents {idx[0]}..{idx[-1]}   {len(diffs)} steps")
    print(f"  even-step mean {mean_even:.3f}   odd-step mean {mean_odd:.3f}   ratio {ratio:.2f}")
    print(f"  motion per tick (even+odd) {mean_even + mean_odd:.3f}")
    if mean_even + mean_odd < 0.02:
        print("  NOTHING IS MOVING — every number above is noise. Drive the stick with "
              "SBR_PAD_SCRIPT; a static scene cannot show whether motion interpolates.")
        return 0
    # ratio = (1+s)/(1-s) for a snapping share s of the on-screen motion.
    snap = (ratio - 1.0) / (ratio + 1.0)
    print(f"  => roughly {snap * 100:.0f}% of on-screen motion lands in one step, i.e. still SNAPS "
          f"at 30 Hz")
    print("  Interpret ONLY against the 60fps-off control from the same scene: if that control is "
          "not ~1.00, the scene itself pulses and this number is measuring that.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
