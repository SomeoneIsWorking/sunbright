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
whatever pose is live and shows lerp(N-1, N, alpha). Each sub-frame therefore sits BETWEEN the main
frame before it and the main frame after it:

    prev_main -> sub  and  sub -> next_main   should BOTH be non-zero at alpha = 0.5
    alpha = 0.0  ->  sub should approach the PRECEDING main frame
    alpha = 1.0  ->  sub should approach the FOLLOWING main frame (it renders pose N)

Note which neighbour each endpoint approaches: an earlier version of this file named only "the
neighbouring main frame", and a zero against the FOLLOWING main was read as identity against the
PRECEDING one. That single ambiguity produced two confident false readings.

WHICH DUMP IS WHICH — ASK THE RUNTIME, NEVER THE PATTERN

Roles come from `aurora_set_dump_tag`, which the runtime stamps onto each dump's filename. Do not
infer them from which adjacent pair happens to be zero: that inference was made twice in one
session and was wrong both times. An unlabelled series is reported AS unlabelled.
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


def role_of(path):
    """The role the RUNTIME stamped on this dump ('main'/'sub'), or None.

    aurora_set_dump_tag appends the label to the filename, so the artifact says what it is. Before
    that existed, roles were inferred from which adjacent pair happened to be zero — and that
    inference was made wrongly twice in one session, each time producing a confident and false
    reading. An unlabelled series is therefore reported as unlabelled, not guessed at.
    """
    tail = path.rsplit(".", 1)[-1]
    return tail if tail in ("main", "sub") else None


def main():
    if len(sys.argv) < 2:
        print("usage: subframe_gate.py <dump-prefix>   (e.g. scratch/render/seq_a0.rgba)")
        return 2
    prefix = sys.argv[1]
    def seq_of(p):
        parts = p.split(".")
        for tok in reversed(parts):
            if tok.isdigit():
                return int(tok)
        return -1

    files = sorted(glob.glob(prefix + ".*"), key=seq_of)
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
        ra, rb = role_of(files[k]), role_of(files[k + 1])
        label = f"{ra or '?'}->{rb or '?'}"
        print(f"  present {k} -> {k+1} : {d:>8} of {n} ({100.0 * d / n:.4f}%)   [{label}]"
              f"   {os.path.basename(files[k])} vs {os.path.basename(files[k+1])}")

    # The alternation is the structural check: adjacent presents of a working sub-frame pair up
    # main/sub, so the diffs should not all be the same order of magnitude unless alpha makes them
    # genuinely equal. Report the shape; do not infer a verdict the data does not carry.
    if any(role_of(f) is None for f in files):
        print()
        print("  NOTE: this series is UNLABELLED — the runtime did not stamp main/sub roles, so")
        print("  which file is a main frame and which is a sub-frame is NOT known here. Do not")
        print("  infer it from the pattern of zeros; re-run with a build that sets the dump tag.")

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
