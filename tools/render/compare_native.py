#!/usr/bin/env python3
"""compare_native.py — diff the SDL3-GPU GX compatibility renderer against the aurora oracle.

The native render arc (CLAUDE.md RENDERER DOCTRINE, 2026-07-23) is built alongside aurora
specifically so every step has a known-good to diff against. This is that diff.

    SBR_RENDER_APPROVED=1 ./run-render.sh \\
      SBR_RENDER_DUMP=scratch/render/native.rgba \\
      SB_DUMP_FRAME=scratch/render/aurora.rgba SB_DUMP_FRAME_AFTER=900
    tools/render/compare_native.py scratch/render/native.rgba scratch/render/aurora.rgba

Both inputs are raw RGBA8, top-left origin (aurora normalises its dump to true RGBA on output; the
native dump writes the same convention deliberately, so this is apples to apples).

The two are usually DIFFERENT SIZES — the native target is EFB-sized (640x448) while aurora presents
at the window/anamorphic size — so the comparison is done on a normalised grid. Sizes are inferred
from file size against a list of plausible shapes; pass --size WxH to be explicit.

Reported metrics, chosen because early milestones do not have colour parity yet:
  coverage   fraction of non-background pixels in each image — "is geometry being drawn at all",
             the first thing that matters and the last thing a screenshot tells you reliably.
  iou        intersection-over-union of the two coverage masks — SILHOUETTE agreement, which is the
             real signal while shading/TEV is still absent.
  mean|d|    mean absolute RGB difference over the common grid — only meaningful once colours match.
"""

import sys
import argparse


def infer_size(nbytes, explicit=None):
    if explicit:
        w, h = (int(v) for v in explicit.lower().split("x"))
        if w * h * 4 != nbytes:
            sys.exit(f"--size {w}x{h} is {w*h*4} bytes, file is {nbytes}")
        return w, h
    px = nbytes // 4
    # Shapes this project actually produces: the EFB, common windows, and the anamorphic present.
    for w, h in [(640, 448), (640, 480), (1280, 960), (1593, 896), (1280, 720), (1920, 1080)]:
        if w * h == px:
            return w, h
    # Fall back to any factor pair with a plausible aspect.
    for h in range(200, 2200):
        if px % h == 0:
            w = px // h
            if 1.0 < w / h < 2.5:
                return w, h
    sys.exit(f"cannot infer dimensions for {nbytes} bytes ({px} px) — pass --size WxH")


def load(path, explicit=None):
    with open(path, "rb") as source:
        data = source.read()
    w, h = infer_size(len(data), explicit)
    return data, w, h


def sample(data, w, h, gx, gy, gw, gh):
    """Nearest-neighbour sample onto a gw x gh grid; returns a flat list of (r,g,b)."""
    out = []
    for y in range(gh):
        sy = min(h - 1, y * h // gh)
        for x in range(gw):
            sx = min(w - 1, x * w // gw)
            i = (sy * w + sx) * 4
            out.append((data[i], data[i + 1], data[i + 2]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("native")
    ap.add_argument("aurora")
    ap.add_argument("--size-native", default=None, help="WxH override")
    ap.add_argument("--size-aurora", default=None, help="WxH override")
    ap.add_argument("--grid", default="320x224", help="comparison grid (default 320x224)")
    ap.add_argument("--bg", default=None,
                    help="background RGB 'r,g,b' to treat as empty; default = each image's own "
                         "most common pixel, which is the clear colour in practice")
    a = ap.parse_args()

    gw, gh = (int(v) for v in a.grid.lower().split("x"))
    nd, nw, nh = load(a.native, a.size_native)
    ad, aw, ah = load(a.aurora, a.size_aurora)
    print(f"native {nw}x{nh}   aurora {aw}x{ah}   grid {gw}x{gh}")

    ns = sample(nd, nw, nh, 0, 0, gw, gh)
    as_ = sample(ad, aw, ah, 0, 0, gw, gh)

    def bg_of(px):
        if a.bg:
            return tuple(int(v) for v in a.bg.split(","))
        counts = {}
        for p in px:
            counts[p] = counts.get(p, 0) + 1
        return max(counts.items(), key=lambda kv: kv[1])[0]

    nbg, abg = bg_of(ns), bg_of(as_)

    def near(p, q, tol=12):
        return abs(p[0] - q[0]) <= tol and abs(p[1] - q[1]) <= tol and abs(p[2] - q[2]) <= tol

    nmask = [not near(p, nbg) for p in ns]
    amask = [not near(p, abg) for p in as_]
    ncov = sum(nmask) / len(nmask)
    acov = sum(amask) / len(amask)

    inter = sum(1 for i in range(len(nmask)) if nmask[i] and amask[i])
    union = sum(1 for i in range(len(nmask)) if nmask[i] or amask[i])
    iou = inter / union if union else 1.0

    diff = sum(abs(ns[i][c] - as_[i][c]) for i in range(len(ns)) for c in range(3))
    mean_d = diff / (len(ns) * 3)

    print(f"background   native {nbg}  aurora {abg}")
    print(f"coverage     native {ncov:6.2%}   aurora {acov:6.2%}")
    print(f"silhouette   IoU {iou:6.2%}   (the metric that matters before shading exists)")
    print(f"mean|d| RGB  {mean_d:6.2f}   (only meaningful once colours match)")

    if ncov == 0.0:
        print("\nnative drew NOTHING — geometry is not reaching the renderer.")
    elif iou < 0.5:
        print("\nsilhouettes disagree badly — suspect the transform (matrix or projection), "
              "not shading.")


if __name__ == "__main__":
    main()
