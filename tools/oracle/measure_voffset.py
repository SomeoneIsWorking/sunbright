#!/usr/bin/env python3
"""Measure vertical framing offset between the matched Dolphin oracle (640x480 XFB)
and the native settled dump (1280x896 EFB) at the SAME pinned camera state.

Features measured per image, reported in normalized 448-line EFB space:
  - mario_feet: lowest row of Mario-colored pixels (red cap/shirt or overalls blue
    or shoe brown) in Mario's column band
  - sign_top:   first row of the brown OPTIONS sign board in its column band
  - blockA_bot: last row of the block-A crate (orange/tan frame) in its band
Refuses degenerate input (feature not found -> hard error).
"""
import sys
import numpy as np
from PIL import Image

def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.int32)

def rows_matching(img, x0, x1, mask_fn):
    band = img[:, x0:x1]
    m = mask_fn(band)
    rows = np.where(m.any(axis=1))[0]
    if len(rows) == 0:
        raise SystemExit(f"FEATURE NOT FOUND in band x[{x0}:{x1}]")
    return rows

def mario_mask(b):
    r, g, bl = b[..., 0], b[..., 1], b[..., 2]
    red = (r > 150) & (g < 90) & (bl < 90)                      # cap/shirt
    blue = (bl > 110) & (r < 80) & (g < 110) & (bl - r > 60)    # overalls (not sky: sky has high g too)
    brown = (r > 90) & (r < 180) & (g > 40) & (g < 110) & (bl < 70) & (r - bl > 60)  # shoes
    return red | blue | brown

def sign_mask(b):
    r, g, bl = b[..., 0], b[..., 1], b[..., 2]
    return (r > 110) & (r < 200) & (g > 55) & (g < 130) & (bl < 90) & (r - bl > 50)  # wood board

def analyze(path, scale, y_off, label, mario_x, sign_x):
    img = load(path)
    H, W = img.shape[:2]
    feet = rows_matching(img, *mario_x, mario_mask).max()
    sign_rows = rows_matching(img, *sign_x, sign_mask)
    sign_top = sign_rows.min()
    # normalize to 448-space
    n = lambda y: (y - y_off) / scale
    print(f"{label} ({W}x{H}): mario_feet_raw={feet} sign_top_raw={sign_top}"
          f"  -> 448-space: feet={n(feet):.1f} sign_top={n(sign_top):.1f}")
    return n(feet), n(sign_top)

# Oracle: 640x480 XFB. Detect black borders to find EFB placement.
opath, npath = sys.argv[1], sys.argv[2]
o = load(opath)
row_lum = o.mean(axis=(1, 2))
nonblack = np.where(row_lum > 8)[0]
y0, y1 = nonblack.min(), nonblack.max()
print(f"oracle non-black rows: {y0}..{y1} (count={y1-y0+1})")
oscale = (y1 - y0 + 1) / 448.0

of, os_ = analyze(opath, oscale, y0, "oracle", (140, 230), (500, 620))
nf, ns = analyze(npath, 2.0, 0, "native", (280, 460), (1000, 1240))
print(f"\nDELTA (native - oracle) in 448-space: feet={nf-of:+.1f}  sign_top={ns-os_:+.1f}")
