#!/usr/bin/env python3
"""Turn a long /verify capture (e.g. a 10s walk = ~600 frames) into watchable artifacts.

Reads the full-res PPM frames the runtime dumped to scratch/verify/s<NNN>/ during a `/verify?n=600`
capture (present order: real, in-between, real, in-between, ...) and produces, in that dir:
  walk.mp4       — every captured frame at 60fps (the actual interpolated output; watch the motion)
  walk_diff.mp4  — amplified x4 consecutive pixel diffs (where motion is each frame)
  walk_mse.png   — consecutive-frame MSE over the whole walk, colored real(green)/btwn(cyan);
                   a near-zero dip = a duplicate (no interp); uniform = smooth.
Plus a printed summary: frame counts, DUP triplets, cadence doublings.

Usage: python3 tools/interp/verify_walk.py [session_dir]   (default newest scratch/verify/s*)
Needs ffmpeg + PIL/numpy.
"""
import sys, glob, os, subprocess
import numpy as np
from PIL import Image, ImageDraw

def newest_session():
    dirs = sorted(glob.glob("scratch/verify/s*"), key=os.path.getmtime)
    if not dirs: sys.exit("no scratch/verify/s* sessions — run /verify?n=600 first")
    return dirs[-1]

def main():
    sess = sys.argv[1] if len(sys.argv) > 1 else newest_session()
    files = sorted(glob.glob(os.path.join(sess, "f*.ppm")))
    if len(files) < 4: sys.exit(f"{sess}: only {len(files)} frames")
    tags = ["btwn" if "_btwn_" in f else "real" for f in files]
    print(f"{sess}: {len(files)} frames  ({tags.count('real')} real, {tags.count('btwn')} btwn)")

    pngdir = os.path.join(sess, "png"); os.makedirs(pngdir, exist_ok=True)
    dfdir  = os.path.join(sess, "dpng"); os.makedirs(dfdir, exist_ok=True)

    prev = None
    mses = []                # consecutive MSE
    doublings = 0
    for i, f in enumerate(files):
        im = Image.open(f).convert("RGB")
        a = np.asarray(im, dtype=np.float32)
        # tag banner + frame index, write the playback PNG
        d = ImageDraw.Draw(im)
        col = (0, 255, 0) if tags[i] == "real" else (0, 220, 255)
        d.rectangle([0, 0, 150, 16], fill=(0, 0, 0)); d.text((3, 3), f"{i:04d} {tags[i]}", fill=col)
        im.save(os.path.join(pngdir, f"{i:04d}.png"))
        if prev is not None:
            mse = float(np.mean((a - prev) ** 2)); mses.append(mse)
            df = np.clip(np.abs(a - prev) * 4.0, 0, 255).astype(np.uint8)
            Image.fromarray(df).save(os.path.join(dfdir, f"{i:04d}.png"))
            if tags[i] == tags[i-1]: doublings += 1
        prev = a

    # videos
    def ff(srcdir, out):
        subprocess.run(["ffmpeg", "-y", "-framerate", "60", "-i", os.path.join(srcdir, "%04d.png"),
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", out],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    walk = os.path.join(sess, "walk.mp4"); dvid = os.path.join(sess, "walk_diff.mp4")
    ff(pngdir, walk); ff(dfdir, dvid)

    # MSE-over-time plot (PIL)
    W, H, pad = max(900, len(mses) + 80), 300, 40
    plot = Image.new("RGB", (W, H), (16, 16, 16)); pd = ImageDraw.Draw(plot)
    mmax = max(mses) if mses else 1.0
    pd.text((6, 4), f"consecutive-frame MSE over the walk  (max={mmax:.0f})  green=real cyan=btwn  dip~0=duplicate", fill=(200,200,200))
    base = H - pad
    pd.line([(pad, base), (W - 10, base)], fill=(80, 80, 80))
    for k, m in enumerate(mses):
        x = pad + k
        y = base - int((m / mmax) * (H - 2 * pad))
        col = (0, 255, 0) if tags[k+1] == "real" else (0, 220, 255)
        pd.line([(x, base), (x, y)], fill=col)
    plotp = os.path.join(sess, "walk_mse.png"); plot.save(plotp)

    # cadence / dup summary over the whole walk
    dups = 0; trips = 0
    for i in range(1, len(files) - 1):
        if tags[i] == "btwn" and tags[i-1] == "real" and tags[i+1] == "real":
            A = np.asarray(Image.open(files[i-1]).convert("RGB"), np.float32)
            B = np.asarray(Image.open(files[i]).convert("RGB"), np.float32)
            C = np.asarray(Image.open(files[i+1]).convert("RGB"), np.float32)
            full = np.mean((A - C) ** 2)
            if full < 1: continue
            lo = np.mean((A - B) ** 2) / full; hi = np.mean((B - C) ** 2) / full
            trips += 1
            if lo < 0.08 or hi < 0.08: dups += 1
    print(f"  doublings={doublings}/{len(files)-1} ({100*doublings/(len(files)-1):.0f}%)  "
          f"clean-triplets={trips} DUP={dups}")
    print("wrote:", walk, dvid, plotp)

if __name__ == "__main__":
    main()
