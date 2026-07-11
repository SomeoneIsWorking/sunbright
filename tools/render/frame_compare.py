#!/usr/bin/env python3
"""frame_compare.py — exact-equality gate for pinned native vs oracle frames.

Usage:
  # exact: are two frames pixel-identical?
  tools/render/frame_compare.py exact <native.rgba> <oracle.png>
  # nearest: given a native frame + an oracle range, find the closest oracle VI
  #   and report whether ANY frame in range is identical (resid=0)
  tools/render/frame_compare.py nearest <native.rgba> --vi-min 3400 --vi-max 4200
  # offset-scan: given several native frames, find the constant present->VI offset
  tools/render/frame_compare.py offset-scan 'scratch/screenshots/nb_*.rgba'

native dumps are raw RGBA8 (640x480 when SB_FB_SCALE=1). oracle is PNG 640x480.
Exit 0 if identical, 1 if not (usable as a gate).
"""
import argparse, glob, os, re, sys
import numpy as np
from PIL import Image


def load_native(p):
    sz = os.path.getsize(p)
    px = sz // 4
    # infer dims (640x480 native, or 1280x960)
    if px == 640 * 480:
        w, h = 640, 480
    elif px == 1280 * 960:
        w, h = 1280, 960
    else:
        # try square-ish
        import math
        s = int(math.isqrt(px))
        if s * s == px:
            w = h = s
        else:
            sys.exit("can't infer dims for %s (%d bytes)" % (p, sz))
    im = Image.frombytes('RGBA', (w, h), open(p, 'rb').read()).convert('RGB')
    if (w, h) != (640, 480):
        im = im.resize((640, 480), Image.LANCZOS)
    return np.asarray(im, dtype=np.uint8)


def load_oracle(vi, ds=None):
    f = 'scratch/oracle/frames/oracle_vi%08d.png' % vi
    try:
        im = Image.open(f).convert('RGB')
    except Exception:
        return None  # corrupt/truncated frame -- caller must skip
    if ds:
        im = im.resize((ds, int(ds * 0.75)), Image.LANCZOS)
    elif im.size != (640, 480):
        im = im.resize((640, 480), Image.LANCZOS)
    return np.asarray(im, dtype=np.uint8)


def report_diff(a, b):
    diff = (a.astype(int) != b.astype(int))
    n_diff_px = (diff.any(axis=2)).sum()
    total = a.shape[0] * a.shape[1]
    if n_diff_px == 0:
        print("IDENTICAL")
        return True
    # per-channel magnitude of difference
    ch = (a.astype(int) - b.astype(int))
    print("differ on %d / %d pixels (%.2f%%)" % (n_diff_px, total, 100.0 * n_diff_px / total))
    print("  per-channel mean |diff|: %s" % np.abs(ch).reshape(-1, 3).mean(0).round(2))
    print("  per-channel max |diff| : %s" % np.abs(ch).reshape(-1, 3).max(0))
    # where
    ys, xs = np.where(diff.any(axis=2))
    print("  differing-pixel bbox: y[%d..%d] x[%d..%d]" % (ys.min(), ys.max(), xs.min(), xs.max()))
    return False


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)

    e = sub.add_parser('exact')
    e.add_argument('native')
    e.add_argument('oracle')  # a PNG path OR vi=NNNN

    n = sub.add_parser('nearest')
    n.add_argument('native')
    n.add_argument('--vi-min', type=int, default=1)
    n.add_argument('--vi-max', type=int, default=4454)

    o = sub.add_parser('offset-scan')
    o.add_argument('glob')
    o.add_argument('--vi-min', type=int, default=1)
    o.add_argument('--vi-max', type=int, default=4454)

    args = ap.parse_args()

    if args.cmd == 'exact':
        nim = load_native(args.native)
        if args.oracle.startswith('vi='):
            oim = load_oracle(int(args.oracle[3:]))
        else:
            oim = np.asarray(Image.open(args.oracle).convert('RGB').resize((640, 480), Image.LANCZOS),
                             dtype=np.uint8)
        ok = report_diff(nim, oim)
        sys.exit(0 if ok else 1)

    elif args.cmd == 'nearest':
        nim = load_native(args.native).astype(np.float32)
        best = (1e9, None)
        for vi in range(args.vi_min, args.vi_max + 1):
            f = 'scratch/oracle/frames/oracle_vi%08d.png' % vi
            if not os.path.exists(f):
                continue
            oim = load_oracle(vi).astype(np.float32)
            d = np.abs(nim - oim).mean()
            if d < best[0]:
                best = (d, vi)
                if d == 0:
                    break
        print("nearest oracle: vi=%d resid=%.3f" % (best[1], best[0]))
        if best[0] == 0:
            print("EXACT MATCH at vi=%d" % best[1])
            sys.exit(0)
        # show top-3 runners-up to judge offset stability
        sys.exit(1)

    elif args.cmd == 'offset-scan':
        files = sorted(glob.glob(args.glob),
                       key=lambda p: int(re.search(r'\.(\d+)$', p).group(1)))
        # Use small thumbnails for the scan (content-match is robust to resolution,
        # and full-res L1 over 4454 frames x N native is too slow).
        DS = 80
        # index oracle thumbnails once
        print("[offset-scan] indexing oracle @ %dx%d..." % (DS, int(DS*0.75)), file=sys.stderr)
        orc_idx, orc = [], []
        for vi in range(args.vi_min, args.vi_max + 1):
            o = load_oracle(vi, ds=DS)
            if o is not None:
                orc_idx.append(vi)
                orc.append(o.astype(np.float32).ravel())
        orc = np.array(orc)
        print("[offset-scan] %d oracle frames indexed" % len(orc_idx), file=sys.stderr)

        offsets = []
        for nf in files:
            nim_im = Image.frombytes('RGBA', (640, 480), open(nf, 'rb').read()).convert('RGB') \
                if os.path.getsize(nf) == 640*480*4 else \
                Image.open(nf).convert('RGB')
            nim = np.asarray(nim_im.resize((DS, int(DS*0.75)), Image.LANCZOS),
                             dtype=np.float32).ravel()
            d = np.abs(orc - nim).mean(axis=1)
            bi = int(d.argmin())
            pres = int(re.search(r'\.(\d+)$', nf).group(1))
            off = pres - orc_idx[bi]
            offsets.append(off)
            print("pres=%5d -> vi=%5d resid=%5.1f offset(pres-vi)=%d" %
                  (pres, orc_idx[bi], d[bi], off))
        if offsets:
            print("\noffset stats: min=%d max=%d (stable if min==max)" %
                  (min(offsets), max(offsets)))


if __name__ == '__main__':
    main()
