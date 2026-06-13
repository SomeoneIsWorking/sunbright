#!/usr/bin/env python3
# 60 fps interpolation verifier — consecutive dumped-frame pixel diffs.
#
# Dolphin dumps every presented XFB to PNG. With SUNBRIGHT_INTERP60 we present
# TWO fields per game frame: [real N, in-between, real N+1, in-between, ...].
# This computes mean-abs pixel diff between consecutive dumped frames, so the
# interpolation can be judged COMPUTATIONALLY (no eyeballing):
#
#   no interp / blend not reaching pixels : diffs alternate  ~0, BIG, ~0, BIG
#       (in-between == copy of real N, then a full 30 Hz jump to N+1)
#   working interpolation                 : diffs roughly UNIFORM and ~half
#       (each field advances ~half a 30 Hz step)
#   perturb (camera shoved)               : diffs huge everywhere
#
# Usage: frame_diff.py <dir> [--newer-than <epoch>] [--last N]
import sys, os, glob
import numpy as np
from PIL import Image

d = sys.argv[1]
newer = None; last = None
for i, a in enumerate(sys.argv):
    if a == "--newer-than": newer = float(sys.argv[i+1])
    if a == "--last": last = int(sys.argv[i+1])

pngs = sorted(glob.glob(os.path.join(d, "*.png")))
if newer is not None:
    pngs = [p for p in pngs if os.path.getmtime(p) >= newer]
if last is not None:
    pngs = pngs[-last:]
if len(pngs) < 3:
    print(f"only {len(pngs)} frames — need >=3"); sys.exit(1)

prev = None; diffs = []
for p in pngs:
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
    if prev is not None and prev.shape == a.shape:
        diffs.append(float(np.abs(a - prev).mean()))
    prev = a

diffs = np.array(diffs)
print(f"frames={len(pngs)} diffs={len(diffs)}")
print("seq: " + " ".join(f"{x:5.1f}" for x in diffs))
# even/odd split: with paired presents, "real->inbetween" and "inbetween->real"
# alternate. If no interp, one parity ~0 and the other BIG.
ev, od = diffs[0::2], diffs[1::2]
print(f"mean={diffs.mean():.2f}  even-parity mean={ev.mean():.2f}  odd-parity mean={od.mean():.2f}")
print(f"min={diffs.min():.2f} max={diffs.max():.2f}")
# A uniformity score: ratio of the two parity means (1.0 = perfectly interpolated,
# ->0 or ->inf = alternating = in-between not advancing).
lo, hi = sorted([ev.mean()+1e-6, od.mean()+1e-6])
print(f"parity ratio (1.0=interp, ~0=no interp)= {lo/hi:.3f}")
