#!/usr/bin/env python3
"""title_track.py — track native's title progression against the oracle's, frame by frame.

For each native dump (a series over the title window), find the closest-content
oracle frame and report the residual. Produces:
  - a residual-over-native-frame curve (spikes = native states the oracle never
    reaches, i.e. broken/divergent animation)
  - the oracle-VI each native frame maps to (monotonic = native tracks oracle's
    progression; jumps = native skips/replays a phase)
  - the worst-residual native frames (candidates for visual inspection)

This is the pixel-grounded counterpart to visual comparison: it can't tell you
WHAT is wrong with a frame, only WHETHER a frame has no oracle counterpart.

Usage:
  tools/render/title_track.py --native 'scratch/screenshots/sweep.rgba.*' \
      --oracle-dir scratch/oracle/frames --oracle-range 1,4454 --step 5 \
      --out scratch/logs/title_track.txt
"""
import argparse, glob, os, re, sys
import numpy as np
from PIL import Image


def load_native(path, ds=640):
    im = Image.frombytes('RGBA', (1280, 960), open(path, 'rb').read()).convert('RGB')
    return np.asarray(im.resize((ds, int(ds*0.75)), Image.LANCZOS), dtype=np.float32)


def load_oracle(path, ds=640):
    return np.asarray(Image.open(path).convert('RGB').resize((ds, int(ds*0.75)), Image.LANCZOS),
                      dtype=np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--native', default='scratch/screenshots/sweep.rgba.*')
    ap.add_argument('--oracle-dir', default='scratch/oracle/frames')
    ap.add_argument('--oracle-range', default='1,4454')
    ap.add_argument('--step', type=int, default=5)
    ap.add_argument('--out', default='scratch/logs/title_track.txt')
    args = ap.parse_args()

    vmin, vmax = [int(x) for x in args.oracle_range.split(',')]
    nat = sorted(glob.glob(args.native), key=lambda p: int(re.search(r'\.(\d+)$', p).group(1)))
    if not nat:
        sys.exit("no native at %r" % args.native)

    # pre-decode oracle at the search stride
    print("[track] indexing oracle (step %d)..." % args.step, file=sys.stderr)
    orc_idx = []
    orc = []
    for vi in range(vmin, vmax + 1, args.step):
        f = os.path.join(args.oracle_dir, 'oracle_vi%08d.png' % vi)
        if os.path.exists(f):
            orc_idx.append(vi)
            orc.append(load_oracle(f).ravel())
    orc = np.array(orc)  # (N, pixels*3)
    print("[track] %d oracle frames indexed" % len(orc_idx), file=sys.stderr)

    rows = []
    for nf in nat:
        nim = load_native(nf).ravel()
        # L1 distance to every oracle frame
        d = np.abs(orc - nim).mean(axis=1)
        bi = int(d.argmin())
        # refine +-step around best
        best_vi, best_d = orc_idx[bi], d[bi]
        rows.append((int(re.search(r'\.(\d+)$', nf).group(1)), best_vi, best_d))

    # report
    with open(args.out, 'w') as f:
        f.write("# native_seq present  oracle_vi  resid   (resid>30 = no good oracle match)\n")
        for seq, vi, d in rows:
            flag = "  <<< HIGH" if d > 30 else ""
            f.write("  %3d  pres=%5d  vi=%5d  resid=%5.1f%s\n" % (rows.index((seq,vi,d)), seq, vi, d, flag))
    print("[track] wrote %s" % args.out, file=sys.stderr)

    # summary curve to stderr
    ds = [r[2] for r in rows]
    vis = [r[1] for r in rows]
    print("[track] resid: min=%.1f med=%.1f max=%.1f  | >30: %d/%d frames" %
          (min(ds), np.median(ds), max(ds), sum(1 for x in ds if x > 30), len(ds)), file=sys.stderr)
    print("[track] oracle-VI trajectory: %s" %
          " ".join(str(r[1]) for r in rows[:30]), file=sys.stderr)
    # monotonicity of VI mapping (jumps = skip/replay)
    jumps = sum(1 for i in range(1, len(vis)) if abs(vis[i] - vis[i-1]) > 200)
    print("[track] VI jumps >200: %d (each = native skipping/replaying an oracle phase)" % jumps,
          file=sys.stderr)


if __name__ == '__main__':
    main()
