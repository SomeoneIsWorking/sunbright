#!/usr/bin/env python3
"""60fps interpolation verification: mean abs diff between consecutive dumped frames.

With the blend redraw working, every consecutive presented-frame pair differs
where motion exists (the in-between frame is a half-step). Without it (NOBLEND,
or interpolation off at 30fps content presented twice), pairs alternate
identical/different. Prints per-pair diff and the identical-pair ratio.
"""
import sys, glob
import numpy as np
from PIL import Image

d = sys.argv[1] if len(sys.argv) > 1 else "."
files = sorted(glob.glob(d + "/*.png"))
if len(files) < 10:
    sys.exit(f"only {len(files)} frames in {d}")
files = files[len(files)//2:]          # steady-state half
prev = None
diffs = []
for f in files:
    img = np.asarray(Image.open(f).convert("L"), dtype=np.int16)
    if prev is not None and prev.shape == img.shape:
        diffs.append(float(np.abs(img - prev).mean()))
    prev = img
diffs = np.array(diffs)
ident = (diffs < 0.05).sum()
print(f"pairs={len(diffs)} mean_diff={diffs.mean():.3f} median={np.median(diffs):.3f} "
      f"identical_pairs={ident} ({100.0*ident/len(diffs):.1f}%)")
print("first 20 diffs:", " ".join(f"{x:.2f}" for x in diffs[:20]))
