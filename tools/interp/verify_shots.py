#!/usr/bin/env python3
"""Render 60fps-interp pan shots + pixel diffs from a /verify capture session.

The runtime (interp_verify.cpp) writes full-res PPM frames to scratch/verify/s<NNN>/ during a
`/verify?n=K` capture, in present order: real N, in-between N+1/2, real N+1, ... tagged real/btwn.
This picks a triplet (real A, in-between B, real C) that has camera/scene motion and renders:
  - the three pan shots side by side (A | B | C)
  - amplified absolute pixel diffs: |A-B|, |B-C|, |A-C|
A genuine midpoint B looks visually between A and C, and |A-B| ~ |B-C| (each ~half of |A-C|).
A duplicate B would make one diff near-black.

Usage: python3 tools/interp/verify_shots.py [session_dir]   (default: newest scratch/verify/s*)
Outputs PNGs into the session dir and prints their paths.
"""
import sys, glob, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont

def newest_session():
    dirs = sorted(glob.glob("scratch/verify/s*"), key=os.path.getmtime)
    if not dirs:
        sys.exit("no scratch/verify/s* sessions — run /verify?n=K first")
    return dirs[-1]

def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)

def label(img_arr, text):
    im = Image.fromarray(np.clip(img_arr, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 8 + 7 * len(text), 18], fill=(0, 0, 0))
    d.text((4, 3), text, fill=(0, 255, 0))
    return im

def amplify_diff(a, b, scale=4.0):
    return np.clip(np.abs(a - b) * scale, 0, 255)

def hstack(imgs, gap=6):
    h = max(i.height for i in imgs); w = sum(i.width for i in imgs) + gap * (len(imgs) - 1)
    out = Image.new("RGB", (w, h), (32, 32, 32)); x = 0
    for i in imgs:
        out.paste(i, (x, 0)); x += i.width + gap
    return out

def mse(a, b):
    return float(np.mean((a - b) ** 2))

def main():
    sess = sys.argv[1] if len(sys.argv) > 1 else newest_session()
    files = sorted(glob.glob(os.path.join(sess, "f*.ppm")))
    if len(files) < 3:
        sys.exit(f"{sess}: need >=3 frames, found {len(files)}")
    tags = ["btwn" if "_btwn_" in f else "real" for f in files]
    frames = [load(f) for f in files]

    # find the triplet (real, btwn, real) with the largest A-C motion
    best = None
    for i in range(1, len(files) - 1):
        if tags[i] == "btwn" and tags[i-1] == "real" and tags[i+1] == "real":
            full = mse(frames[i-1], frames[i+1])
            if best is None or full > best[1]:
                best = (i, full)
    if best is None:
        sys.exit(f"{sess}: no real,btwn,real triplet — capture with motion (/pad?do=cright&ms=3000)")
    i, full = best
    A, B, C = frames[i-1], frames[i], frames[i+1]
    lo, hi = mse(A, B), mse(B, C)
    print(f"session {sess}  triplet f{i-1}(real) f{i}(btwn) f{i+1}(real)")
    print(f"  MSE: |A-B|={lo:.1f}  |B-C|={hi:.1f}  |A-C|={full:.1f}   lo/full={lo/full:.2f} hi/full={hi/full:.2f}")

    shots = hstack([label(A, "real N"), label(B, "in-between N+1/2"), label(C, "real N+1")])
    diffs = hstack([label(amplify_diff(A, B), "|N - N+1/2| x4"),
                    label(amplify_diff(B, C), "|N+1/2 - N+1| x4"),
                    label(amplify_diff(A, C), "|N - N+1| x4 (full step)")])
    combo = Image.new("RGB", (max(shots.width, diffs.width), shots.height + diffs.height + 6), (32,32,32))
    combo.paste(shots, (0, 0)); combo.paste(diffs, (0, shots.height + 6))

    p_shots = os.path.join(sess, "pan_shots.png")
    p_diffs = os.path.join(sess, "pan_diffs.png")
    p_combo = os.path.join(sess, "pan_combo.png")
    shots.save(p_shots); diffs.save(p_diffs); combo.save(p_combo)
    print("wrote:", p_shots, p_diffs, p_combo)

if __name__ == "__main__":
    main()
