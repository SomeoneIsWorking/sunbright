#!/usr/bin/env python3
"""Crop each 2D element out of a SUNBRIGHT_DUMP frame → a PNG of itself.

Companion to the SUNBRIGHT_2DID tool: that logs each J2D element's name + game-space rect
(640x480) to scratch/2d_elements/elements.log; this maps those rects through the widescreen 2D
squeeze into a dumped 16:9 frame and crops them out, so you get one PNG per element to eyeball.

Usage:
    tools/render/crop_2d_elements.py <frame.png> [--log scratch/2d_elements/elements.log]
                              [--squeeze 0.75] [--game-w 640] [--game-h 480] [--frame N]

The 2D ortho is squeezed horizontally by `squeeze` (=(4:3)/(16:9)) about screen centre and the
EFB is presented at 16:9, so game-x maps to frame-x as:
    fx = ( squeeze*(2*x/game_w - 1) + 1 ) / 2 * W      (centre stays centre; [0,game_w] -> centre 75%)
    fy =   y/game_h * H                                (vertical is untouched)
Tune --squeeze / --game-w if the crops are offset (root screen reports 600 wide, panes 640-ish).

The logged rects are the pane's GLOBAL (absolute screen) rect, so nested panes crop correctly too
(verified: 'shn0' crops to the shine icon, 'yaji' to the OPTIONS arrow).
"""
import argparse, os, re, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frame")
    ap.add_argument("--log", default="scratch/2d_elements/elements.log")
    ap.add_argument("--squeeze", type=float, default=0.75)
    ap.add_argument("--game-w", type=float, default=640.0)
    ap.add_argument("--game-h", type=float, default=480.0)
    ap.add_argument("--frame", type=int, default=-1, help="which logged frame block (default: last)")
    ap.add_argument("--out", default="scratch/2d_elements/crops")
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.exit("needs Pillow:  pip install --user Pillow")

    # Parse the log into frame blocks: {frame_num: [(type, name, x, y, w, h), ...]}.
    blocks, cur = {}, None
    rx = re.compile(r"^\s*(\w+)\s+'(.*?)'\s+id=([0-9a-f]+)\s+rect=\((-?\d+),(-?\d+)\s+(-?\d+)x(-?\d+)\)")
    for line in open(args.log):
        m = re.match(r"── frame (\d+) ──", line)
        if m:
            cur = int(m.group(1)); blocks[cur] = []; continue
        e = rx.match(line)
        if e and cur is not None:
            t, nm, _id, x, y, w, h = e.group(1), e.group(2), e.group(3), *map(int, e.groups()[3:])
            blocks[cur].append((t, nm, _id, x, y, w, h))
    if not blocks:
        sys.exit("no frame blocks parsed from " + args.log)
    fn = args.frame if args.frame in blocks else max(blocks)
    elems = blocks[fn]

    img = Image.open(args.frame).convert("RGBA")
    W, H = img.size
    os.makedirs(args.out, exist_ok=True)

    def fx(x): return (args.squeeze * (2 * x / args.game_w - 1) + 1) / 2 * W
    def fy(y): return y / args.game_h * H

    n = 0
    for t, nm, _id, x, y, w, h in elems:
        if w <= 0 or h <= 0:
            continue
        l, r = int(fx(x)), int(fx(x + w))
        tp, bt = int(fy(y)), int(fy(y + h))
        l, r = max(0, min(l, W)), max(0, min(r, W))
        tp, bt = max(0, min(tp, H)), max(0, min(bt, H))
        if r - l < 2 or bt - tp < 2:
            continue
        safe = re.sub(r"[^\w.-]", "_", nm)
        out = os.path.join(args.out, f"{t}_{safe}_{_id}.png")
        img.crop((l, tp, r, bt)).save(out)
        n += 1
    print(f"frame {fn}: cropped {n} elements from {args.frame} ({W}x{H}) -> {args.out}/")

if __name__ == "__main__":
    main()
