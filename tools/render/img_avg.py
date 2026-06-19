#!/usr/bin/env python3
"""img_avg — time-average a set of frames into one image (P6 PPM).

Used by the file-select oracle (fs_oracle.sh) to kill ANIMATION-PHASE confounds
before an A/B diff. File-select has scrolling clouds, animated water, and a Mario
that runs around — a single-frame ngx-vs-GX diff is dominated by where those
happened to be, NOT by a real renderer difference (memory
fileselect-cloud-wash-drift-artifact: the multi-session file-select "wash" was
largely this artifact). Averaging N frames spread over time converges each side
to its time-mean, so periodic animation cancels and only SYSTEMATIC differences
(material shading/color/blend) survive into the diff.

Usage:
  img_avg.py <out.ppm> <frame1.png> [frame2.png ...]
Refuses (exit 3) if the averaged frame is ~all black (degenerate capture).
"""
import sys
from PIL import Image


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    frames = sys.argv[2:]
    acc = None
    w = h = None
    n = 0
    for f in frames:
        try:
            im = Image.open(f).convert("RGB")
        except Exception as e:
            print(f"  skip {f}: {e}", file=sys.stderr)
            continue
        if w is None:
            w, h = im.size
        elif im.size != (w, h):
            print(f"  skip {f}: size {im.size} != {(w, h)}", file=sys.stderr)
            continue
        px = im.tobytes()
        if acc is None:
            acc = bytearray(len(px))
            sums = [0] * len(px)
        for i, b in enumerate(px):
            sums[i] += b
        n += 1
    if n == 0:
        print("img_avg: no usable frames", file=sys.stderr)
        return 2

    avg = bytes((s + n // 2) // n for s in sums)
    # degenerate (all-black) guard — never average garbage into a meaningful-looking PPM
    tot = sum(avg)
    nonblack = sum(1 for i in range(0, len(avg), 3)
                   if avg[i] + avg[i + 1] + avg[i + 2] > 30) / (len(avg) // 3)
    if nonblack < 0.01:
        print(f"img_avg: averaged frame is ~all black (nonblack={nonblack*100:.2f}%) — refusing",
              file=sys.stderr)
        return 3

    with open(out, "wb") as fp:
        fp.write(f"P6\n{w} {h}\n255\n".encode())
        fp.write(avg)
    print(f"img_avg: {n} frames -> {out} ({w}x{h}, mean luma {tot/len(avg):.1f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
