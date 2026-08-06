#!/usr/bin/env python3
"""subframe_position.py — WHERE does the sub-frame sit between its two main-frame neighbours?

THE NUMBER THIS ARC IS STEERED BY HAD NO TOOL.

"asymmetry 74.5% -> 25.6% -> 20.4%" is the headline of three commits in the 60fps arc, and it was
computed by hand each time, in the session, from an ad-hoc diff loop that was never committed. So
the project's steering metric could not be re-run, could not be compared across sessions, and
carried none of the refusals its own doctrine demands. That is the defect this file closes; the
formula below is the one those numbers were taken with, so historical readings stay comparable.

WHAT IT MEASURES, AND WHY THE COMPARISON IS INSIDE ONE RUN

A working sub-frame renders lerp(N-1, N, alpha), so in a run's present series each sub-frame sits
between the main frame before it and the main frame after it:

    prev_main ---- sub ---- next_main

Everything below is computed from ONE run's consecutive presents (SB_DUMP_FRAME_EVERY=1). Nothing
is compared across runs, so nothing drifts: SB_DUMP_FRAME_AFTER counts PRESENTS, and the same index
reaches a different game moment as soon as the cadence changes, which has already produced one
wrong "VERIFIED" in this project.

    a = d(prev, sub)      how far the sub-frame is from the preceding main frame
    b = d(sub, next)      ...and from the following one
    c = d(prev, next)     the whole tick's motion — the denominator everything is scaled by

    asymmetry   = (a - b) / c     0 = centred. +1 = the sub-frame IS the next main frame
                                  (it duplicates its follower and adds no intermediate image);
                                  -1 = it duplicates its predecessor.
    lead        = a / (a + b)     the same thing as a fraction: 0.5 = centred.
    off-segment = (a + b) / c - 1 how much of the sub-frame lies on NEITHER neighbour's path.
                                  A centred asymmetry with a large off-segment means the
                                  sub-frame is equidistant from both because it is wrong in a
                                  third direction, not because it is halfway along.

WHAT THESE METRICS ARE NOT. Neither "share of pixels that differ" nor "mean absolute channel
difference" is LINEAR in displacement: a half-step does not produce half the differing pixels. So
`ideal half` (c/2) is a REFERENCE POINT, not a target, and a raw a or b must never be read as "the
sub-frame moved x% of the way". What survives the non-linearity is the SIGN and the ORDERING —
which neighbour is nearer, and whether that changes with alpha — which is why both metrics are
printed side by side: a conclusion that holds under only one of them is a conclusion about the
metric.

THE NEGATIVES, WHICH ARE THE WHOLE POINT

  * c == 0 (the tick did not move): REFUSED, per triple. a/0 and b/0 are not "perfect symmetry",
    and a still scene scoring 0.0 asymmetry is the failure this arc has already paid for once —
    an earlier placement scored a confident 98.6% purely because every actor had stopped.
  * unlabelled series: REFUSED. Roles come from the runtime's aurora_set_dump_tag stamp. Inferring
    main/sub from which adjacent pair happens to be zero was done twice in one session and was
    wrong both times.
  * no main/sub/main triple in the series: REFUSED, with the role sequence printed, so "0 triples"
    can never be read as "0 asymmetry".

Usage:
    tools/interp/subframe_position.py scratch/render/seq_a05.rgba
    tools/interp/subframe_position.py --selftest        # must print PASS; wired into the suite
"""
import sys
import glob
import os

import numpy as np


def load(path):
    return np.frombuffer(open(path, 'rb').read(), dtype=np.uint8)


def metrics(a, b):
    """(share of pixels differing in RGB, mean absolute channel difference) — both as percentages
    of their own full scale, so they are readable side by side."""
    ra = a.reshape(-1, 4)[:, :3].astype(np.int16)
    rb = b.reshape(-1, 4)[:, :3].astype(np.int16)
    d = np.abs(ra - rb)
    px = float((d.any(axis=1)).mean() * 100.0)
    mad = float(d.mean() / 255.0 * 100.0)
    return px, mad


def role_of(path):
    tail = path.rsplit(".", 1)[-1]
    return tail if tail in ("main", "sub") else None


def seq_of(p):
    for tok in reversed(p.split(".")):
        if tok.isdigit():
            return int(tok)
    return -1


def score(a, b, c):
    """asymmetry, lead, off-segment — or None where the denominator is zero."""
    if c == 0.0:
        return None
    lead = a / (a + b) if (a + b) > 0.0 else None
    return (a - b) / c, lead, (a + b) / c - 1.0


def report(files, imgs):
    roles = [role_of(f) for f in files]
    if any(r is None for r in roles):
        print("REFUSED: this series is UNLABELLED — the runtime did not stamp main/sub roles.")
        print(f"  files: {[os.path.basename(f) for f in files]}")
        print("  Nothing is scored. Which present is a sub-frame is not inferable from the pixels,")
        print("  and inferring it from the pattern of zeros was wrong twice in one session.")
        return 1

    triples = [k for k in range(len(imgs) - 2)
               if roles[k] == 'main' and roles[k + 1] == 'sub' and roles[k + 2] == 'main']
    print(f"series : {len(files)} presents, roles {' '.join(roles)}")
    if not triples:
        print(f"REFUSED: no main->sub->main triple in this series (scanned {max(0, len(imgs)-2)} "
              f"positions). NOTHING was scored — this is not a symmetry of 0.")
        return 1

    print(f"triples: {len(triples)} scored"
          f"   [px = share of pixels differing | mad = mean abs channel difference, both %]")
    print()
    kept = {'px': [], 'mad': []}
    scale = {'px': [], 'mad': []}
    refused = 0
    for k in triples:
        prev, sub, nxt = imgs[k], imgs[k + 1], imgs[k + 2]
        line = {}
        for name, idx in (('px', 0), ('mad', 1)):
            a = metrics(prev, sub)[idx]
            b = metrics(sub, nxt)[idx]
            c = metrics(prev, nxt)[idx]
            line[name] = (a, b, c, score(a, b, c))
        a, b, c, s = line['px']
        if s is None:
            refused += 1
            print(f"  present {k}..{k+2}: REFUSED — the tick did not move (prev == next, c = 0). "
                  f"A sub-frame cannot be 'between' two identical frames; scoring this would "
                  f"report a still scene as perfectly centred.")
            continue
        # THE SIGNATURE OF A RUN THAT MEASURED NOTHING. a == 0 exactly means the sub present is
        # BIT-IDENTICAL to the main frame before it. That is a real failure mode of the
        # interpolation (the sub-frame duplicating its predecessor) and it is also what a run
        # without SBR_INTERP60_COPY / SBR_PRESENT_AFTER_COPY produces for a completely different
        # reason: the sub-frame renders into the EFB, nothing copies it out, and the display keeps
        # showing the previous XFB. The pixels cannot tell those apart, so the score must not be
        # handed over as though they could.
        if line['px'][0] == 0.0:
            print(f"  present {k}..{k+2}: the sub present is BIT-IDENTICAL to the main frame "
                  f"before it (prev->sub = 0 exactly).")
            print("      Before reading this as 'the sub-frame duplicates its predecessor', check "
                  "the run had BOTH")
            print("      SBR_INTERP60_COPY=1 and SBR_PRESENT_AFTER_COPY=1. Without them the "
                  "sub-frame is never")
            print("      copied out of the EFB and this exact reading appears however well the "
                  "interpolation works.")
            print("      tools/interp/interp60_run.sh carries the full set.")
        for name in ('px', 'mad'):
            a, b, c, s = line[name]
            kept[name].append(s)
            scale[name].append(c)
            print(f"  present {k}..{k+2} [{name:>3}]: prev->sub {a:7.3f}  sub->next {b:7.3f}  "
                  f"full tick {c:7.3f}  ideal half {c/2:7.3f}"
                  f"   asymmetry {s[0]*100:+7.2f}%  lead {('%.3f' % s[1]) if s[1] is not None else '  n/a'}"
                  f"  off-segment {s[2]*100:+7.2f}%")
        print()

    print(f"=== SUMMARY over {len(kept['px'])} scored triple(s), {refused} refused ===")
    if not kept['px']:
        print("  NOTHING scored. Every triple was refused (the scene was static). Do not read this")
        print("  as symmetry: the run needs a pad script that MOVES the player.")
        return 1
    for name in ('px', 'mad'):
        asym = [s[0] for s in kept[name]]
        lead = [s[1] for s in kept[name] if s[1] is not None]
        off = [s[2] for s in kept[name]]
        print(f"  [{name:>3}] asymmetry {100*sum(asym)/len(asym):+7.2f}%   "
              f"lead {sum(lead)/len(lead):.3f}   off-segment {100*sum(off)/len(off):+7.2f}%   "
              f"MOMENT SCALE (mean full tick) {sum(scale[name])/len(scale[name]):7.3f}")
    print("  COMPARE ONLY AT EQUAL MOMENT SCALE. asymmetry is a ratio whose denominator is how far")
    print("  the tick moved, and the two are not independent: at a fast moment the sub-frame's")
    print("  fixed content dominates and asymmetry saturates, at a slow one the same sub-frame")
    print("  scores far closer to centred. Two configurations measured at different scales are not")
    print("  comparable, and a table of asymmetries with no scale column cannot be checked for it.")
    print("  asymmetry 0 = the sub-frame is equidistant from its neighbours; +100% = it duplicates")
    print("  the FOLLOWING main frame; -100% = it duplicates the preceding one. Neither metric is")
    print("  linear in displacement, so compare these across configurations, never against a target.")
    return 0


def selftest():
    """Feed cases whose answer is known and that MUST come out differently from each other.

    A gate that only ever sees real frames cannot tell a working scorer from one that prints a
    plausible constant. Each case below has an answer the arithmetic forces.
    """
    h, w = 64, 64

    def shifted(dx):
        x = (np.arange(w)[None, :] + np.zeros((h, 1), dtype=int) - dx) % w
        img = np.zeros((h, w, 4), dtype=np.uint8)
        img[..., 0] = (x * 4) % 256
        img[..., 1] = 40
        img[..., 2] = 200
        img[..., 3] = 255
        return img.reshape(-1)

    fails = []

    def check(name, cond, detail):
        print(f"  {'ok  ' if cond else 'FAIL'}  {name}: {detail}")
        if not cond:
            fails.append(name)

    prev, mid, nxt = shifted(0), shifted(4), shifted(8)
    a, b, c = metrics(prev, mid)[1], metrics(mid, nxt)[1], metrics(prev, nxt)[1]
    s = score(a, b, c)
    check("midpoint is centred", abs(s[0]) < 0.05, f"asymmetry {s[0]*100:+.2f}% (|.|<5% required)")

    # A sub-frame that duplicates its FOLLOWER: the failure this metric exists to name.
    a, b, c = metrics(prev, nxt)[1], metrics(nxt, nxt)[1], metrics(prev, nxt)[1]
    s = score(a, b, c)
    check("duplicate-of-next scores +100%", abs(s[0] - 1.0) < 1e-6, f"asymmetry {s[0]*100:+.2f}%")

    # ...and of its PREDECESSOR, which must score the OTHER sign. A scorer that cannot distinguish
    # these two is useless here: both look like "the sub-frame is not intermediate".
    a, b, c = metrics(prev, prev)[1], metrics(prev, nxt)[1], metrics(prev, nxt)[1]
    s = score(a, b, c)
    check("duplicate-of-prev scores -100%", abs(s[0] + 1.0) < 1e-6, f"asymmetry {s[0]*100:+.2f}%")

    # A static tick must REFUSE, not score 0. This is the negative that matters most.
    check("static tick refuses", score(0.0, 0.0, 0.0) is None,
          "c == 0 returns None instead of a symmetric-looking 0.0")

    # Off-segment: a sub-frame that is wrong in a THIRD direction is equidistant from both
    # neighbours, so asymmetry alone would call it centred. off-segment must catch it.
    other = np.zeros((h, w, 4), dtype=np.uint8)
    other[..., 3] = 255
    other = other.reshape(-1)
    a, b, c = metrics(prev, other)[1], metrics(other, nxt)[1], metrics(prev, nxt)[1]
    s = score(a, b, c)
    check("off-segment catches a third-direction sub-frame",
          abs(s[0]) < 0.2 and s[2] > 1.0,
          f"asymmetry {s[0]*100:+.2f}% (looks centred) but off-segment {s[2]*100:+.1f}%")

    print()
    if fails:
        print(f"SELFTEST FAILED: {', '.join(fails)}")
        return 1
    print("SELFTEST PASS")
    return 0


def compare(pa, pb):
    """--compare A B: does alpha reach the sub-frame AT ALL?

    Two runs at different alphas, same seed, same pad script, same switches: the cadence is
    identical, so present index k is the same game moment in both and a cross-run diff at the same
    index is legitimate here in a way it is NOT across configurations that change the cadence.

    What it answers, which the within-run score cannot: a sub-frame that is 99% like its follower
    might be a correct render of a nearly-static moment, or an alpha that reaches nothing. If the
    two runs' SUB frames are byte-identical while their MAIN frames are too, the substitution
    changed nothing anywhere and every asymmetry taken from either run is a statement about the
    scene, not about the interpolation.
    """
    fa = sorted(glob.glob(pa + ".*"), key=seq_of)
    fb = sorted(glob.glob(pb + ".*"), key=seq_of)
    if not fa or not fb or len(fa) != len(fb):
        print(f"REFUSED: series lengths differ ({len(fa)} vs {len(fb)}) — index k is not the same "
              f"moment in both. Nothing compared.")
        return 1
    ra = [role_of(f) for f in fa]
    rb = [role_of(f) for f in fb]
    if ra != rb or any(r is None for r in ra):
        print(f"REFUSED: role sequences differ or are unlabelled ({ra} vs {rb}). Nothing compared.")
        return 1
    print(f"comparing {len(fa)} presents, roles {' '.join(ra)}")
    tot = {'main': [], 'sub': []}
    for k, (a, b) in enumerate(zip(fa, fb)):
        px, mad = metrics(load(a), load(b))
        tot[ra[k]].append(px)
        print(f"  present {k} [{ra[k]:>4}] : {px:7.3f}% pixels differ, mad {mad:6.3f}%")
    print()
    for role in ('main', 'sub'):
        v = tot[role]
        if not v:
            print(f"  {role}: NO presents of this role in the series — nothing to conclude about it.")
            continue
        print(f"  {role}: mean {sum(v)/len(v):7.3f}% over {len(v)} present(s)")
    if tot['sub'] and max(tot['sub']) == 0.0:
        print("  ALPHA REACHES NOTHING: every sub present is byte-identical across the two runs.")
        print("  Any asymmetry measured from either run describes the scene, not the interpolation.")
    if tot['main'] and max(tot['main']) > 0.0:
        print("  LEAK: a MAIN present differs across alpha. The substitution is not self-cancelling,")
        print("  so the two runs are not the same game and no cross-run reading from them is valid.")
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--selftest':
        return selftest()
    if len(sys.argv) >= 4 and sys.argv[1] == '--compare':
        return compare(sys.argv[2], sys.argv[3])
    if len(sys.argv) < 2:
        print(__doc__.strip().rsplit("Usage:", 1)[-1])
        return 2
    prefix = sys.argv[1]
    files = sorted(glob.glob(prefix + ".*"), key=seq_of)
    if len(files) < 3:
        print(f"REFUSED: found {len(files)} dump(s) matching {prefix}.* — a triple needs 3.")
        print("  Nothing was compared. Check the run produced its series (SB_DUMP_FRAME_EVERY=1,")
        print("  SB_DUMP_FRAME_COUNT=n>=3) and that it reached SB_DUMP_FRAME_AFTER.")
        return 1
    imgs = [load(f) for f in files]
    sizes = {i.size for i in imgs}
    if len(sizes) != 1:
        print(f"REFUSED: dumps differ in size {sizes} — not the same framebuffer.")
        return 1
    return report(files, imgs)


if __name__ == '__main__':
    sys.exit(main())
