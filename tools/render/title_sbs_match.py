#!/usr/bin/env python3
"""title_sbs_match.py — phase-matched side-by-side contact sheet: native vs oracle.

For each native frame, find the closest-content oracle frame (the title always
animates, so present-count != VI-count and there's no static frame to pin).
Render oracle|native pairs + a per-pair residual into a contact sheet PNG the
user can eyeball for parity defects that pixel-statistics miss.

Usage:
  # default: native frames = scratch/screenshots/npaced.rgba.*, oracle = scratch/oracle/frames/
  tools/render/title_sbs_match.py
  # custom:
  tools/render/title_sbs_match.py --native 'scratch/screenshots/npaced.rgba.*' \
      --oracle-dir scratch/oracle/frames --out scratch/screenshots/sbs_sheet.png \
      --oracle-range 1,4454 --step 10

Native frames are raw RGBA8 1280x960 (SB_DUMP_FRAME, post-2026-07-11 fix).
Oracle frames are PNG (640x480), downscaled native 2x -> match at 640x480.
"""
import argparse, glob, os, re, sys
import numpy as np
from PIL import Image


def load_native(path):
    """raw RGBA8 1280x960 -> RGB 640x480 (LANCZOS downscale, matches 2x)."""
    with open(path, 'rb') as f:
        data = f.read()
    im = Image.frombytes('RGBA', (1280, 960), data).convert('RGB')
    return np.asarray(im.resize((640, 480), Image.LANCZOS), dtype=np.float32)


def load_oracle(path):
    return np.asarray(Image.open(path).convert('RGB').resize((640, 480), Image.LANCZOS),
                      dtype=np.float32)


def vi_of(path):
    m = re.search(r'oracle_vi(\d+)', os.path.basename(path))
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--native', default='scratch/screenshots/npaced.rgba.*')
    ap.add_argument('--oracle-dir', default='scratch/oracle/frames')
    ap.add_argument('--oracle-range', default='1,4454',
                    help='comma min,max VI index to search (dense search)')
    ap.add_argument('--step', type=int, default=10,
                    help='oracle search stride (smaller=denser+slower)')
    ap.add_argument('--out', default='scratch/screenshots/sbs_sheet.png')
    ap.add_argument('--refine', action='store_true',
                    help='coarse-then-fine search around the best match')
    args = ap.parse_args()

    vmin, vmax = [int(x) for x in args.oracle_range.split(',')]
    nat_files = sorted(glob.glob(args.native), key=lambda p: int(re.search(r'\.(\d+)$', p).group(1)))
    if not nat_files:
        sys.exit("no native frames at %r" % args.native)

    # Build oracle index (dense). Pre-decode is memory-heavy for 4400 frames; decode on the fly.
    orc_files = [os.path.join(args.oracle_dir, 'oracle_vi%08d.png' % vi)
                 for vi in range(vmin, vmax + 1, args.step)]
    orc_files = [f for f in orc_files if os.path.exists(f)]
    if not orc_files:
        sys.exit("no oracle frames in range")
    print("[sbs] %d native frames x %d oracle candidates (step %d)" %
          (len(nat_files), len(orc_files), args.step), file=sys.stderr)

    pairs = []  # (native_path, best_vi, resid, native_rgb, oracle_rgb)
    for nf in nat_files:
        nim = load_native(nf)
        # coarse pass
        best = (1e9, None)
        for of in orc_files:
            oim = load_oracle(of)
            d = np.abs(nim - oim).mean()
            if d < best[0]:
                best = (d, vi_of(of))
        if args.refine and best[1] > 0:
            # fine pass +-2*step around best
            center = best[1]
            for vi in range(max(vmin, center - 2 * args.step),
                            min(vmax, center + 2 * args.step) + 1):
                of = os.path.join(args.oracle_dir, 'oracle_vi%08d.png' % vi)
                if not os.path.exists(of) or vi_of(of) == best[1]:
                    continue
                oim = load_oracle(of)
                d = np.abs(nim - oim).mean()
                if d < best[0]:
                    best = (d, vi)
        best_vi = best[1]
        of = os.path.join(args.oracle_dir, 'oracle_vi%08d.png' % best_vi)
        oim = load_oracle(of)
        pairs.append((nf, best_vi, best[0], nim, oim))
        seq = re.search(r'\.(\d+)$', nf).group(1)
        print("  native %s -> oracle vi=%d resid=%.1f" % (os.path.basename(nf), best_vi, best[0]),
              file=sys.stderr)

    # Contact sheet: rows of [oracle | native | diff-heatmap], 4 per row
    CELL_W, CELL_H = 640, 200  # downscale each pair for the sheet
    PER_ROW = 3
    rows = (len(pairs) + PER_ROW - 1) // PER_ROW
    sheet = Image.new('RGB', (CELL_W * 3 * PER_ROW, CELL_H * rows), (32, 32, 32))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for idx, (nf, vi, resid, nim, oim) in enumerate(pairs):
        r, c = divmod(idx, PER_ROW)
        # build the triplet
        diff = np.abs(nim - oim)
        hm = (diff.max(2) * 4).clip(0, 255).astype(np.uint8)  # amplified heatmap
        triplet = Image.new('RGB', (640 * 3, 480))
        triplet.paste(Image.fromarray(oim.astype(np.uint8)), (0, 0))
        triplet.paste(Image.fromarray(nim.astype(np.uint8)), (640, 0))
        triplet.paste(Image.fromarray(np.stack([hm, hm, hm], -1)), (1280, 0))
        triplet = triplet.resize((CELL_W * 3, CELL_H), Image.LANCZOS)
        sheet.paste(triplet, (c * CELL_W * 3, r * CELL_H))
        seq = re.search(r'\.(\d+)$', nf).group(1)
        draw.text((c * CELL_W * 3 + 4, r * CELL_H + 2),
                  "nat pres=%s  |vi=%d resid=%.0f" % (str(int(seq) + 1) if seq else '?', vi, resid),
                  fill=(255, 255, 0))
    sheet.save(args.out)
    print("[sbs] wrote %s (%dx%d)" % (args.out, sheet.size[0], sheet.size[1]), file=sys.stderr)


if __name__ == '__main__':
    main()
