#!/usr/bin/env python3
"""frame_regions.py — WHERE two frames differ, not just how much.

WHY THIS EXISTS

A whole-frame mean |delta| cannot distinguish "the entire background shifted by a little" from "one
character-sized region changed a lot". Those are the same number and they have opposite causes, and
the 60fps arc has now spent three rounds attributing a residual by ELIMINATION — naming a candidate
population, substituting it correctly, and measuring no change because that population was off
screen. Elimination keeps answering a question the data was never asked.

So this asks the data directly: a coarse tile grid over the frame, mean |delta| per tile, plus the
concentration statistics that separate the two shapes above.

WHAT IT REPORTS, AND WHY EACH NUMBER IS THERE

  coverage   — share of tiles with any difference at all. Near 100% means a global change (camera,
               a fullscreen effect); a small number means something local moved.
  top-decile — share of the TOTAL difference carried by the worst 10% of tiles. Near 10% means the
               difference is spread evenly; near 100% means one region carries everything.
  the map    — the grid itself, so a shape (a character, a horizon band, a HUD corner) is visible
               rather than inferred from statistics.

THE CONTROL

`--selftest` builds two synthetic pairs and asserts the statistics separate them: a uniform shift
must read as high coverage and low concentration, and a single blob must read as low coverage and
high concentration. A grid that cannot tell those apart would report a confident shape for any input,
and this is the one check that catches it. Wired to exit non-zero on failure.

Usage:
  frame_regions.py <a.rgba> <b.rgba> [--width 1280] [--cols 16] [--rows 12]
  frame_regions.py --selftest
"""
import argparse
import sys


def load(path):
    with open(path, "rb") as f:
        return f.read()


def tile_stats(a, b, width, height, cols, rows):
    """Mean |delta| per channel for each tile of a cols x rows grid."""
    tiles = [[0.0] * cols for _ in range(rows)]
    counts = [[0] * cols for _ in range(rows)]
    for y in range(height):
        r = y * rows // height
        base = y * width
        row = tiles[r]
        crow = counts[r]
        for x in range(width):
            i = (base + x) * 4
            d = abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
            c = x * cols // width
            row[c] += d
            crow[c] += 1
    for r in range(rows):
        for c in range(cols):
            if counts[r][c]:
                tiles[r][c] /= 3.0 * counts[r][c]
    return tiles


def summarize(tiles, cols, rows, label, show_map=True):
    flat = sorted((v for row in tiles for v in row), reverse=True)
    total = sum(flat)
    n = len(flat)
    nonzero = sum(1 for v in flat if v > 0.0)
    top_n = max(1, n // 10)
    top_share = (sum(flat[:top_n]) / total * 100.0) if total else 0.0
    print(f"\n{label}")
    print(f"  whole-frame mean |d|/channel : {total / n:.3f}")
    print(f"  coverage (tiles with any d)  : {nonzero}/{n} ({100.0 * nonzero / n:.1f}%)")
    print(f"  top-decile share of total    : {top_share:.1f}%"
          f"   (10% = spread evenly, 100% = one region carries it all)")
    if not show_map:
        return
    # A glyph ramp keyed to each tile's share of the frame's own maximum, so the shape is visible
    # at any absolute scale. The legend is printed because an unlabelled ramp is unreadable.
    hi = flat[0] if flat else 0.0
    ramp = " .:-=+*#%@"
    print(f"  map (relative to this pair's max tile = {hi:.2f}):")
    for r in range(rows):
        line = "".join(ramp[min(len(ramp) - 1, int(tiles[r][c] / hi * (len(ramp) - 1)))]
                       if hi else " " for c in range(cols))
        print(f"    |{line}|")


def synth(width, height, kind):
    """Two synthetic frames whose difference has a KNOWN shape."""
    a = bytearray(width * height * 4)
    b = bytearray(width * height * 4)
    for i in range(width * height):
        a[4 * i:4 * i + 3] = bytes((100, 100, 100))
        b[4 * i:4 * i + 3] = bytes((100, 100, 100))
    if kind == "uniform":
        for i in range(width * height):           # every pixel differs a little
            b[4 * i] = 104
    else:
        for y in range(height // 2, height // 2 + height // 8):
            for x in range(width // 2, width // 2 + width // 12):
                b[4 * (y * width + x)] = 255      # one blob differs a lot
    return bytes(a), bytes(b)


def selftest():
    W, H, C, R = 320, 240, 16, 12
    ok = True
    a, b = synth(W, H, "uniform")
    t = tile_stats(a, b, W, H, C, R)
    flat = sorted((v for row in t for v in row), reverse=True)
    cov = 100.0 * sum(1 for v in flat if v > 0) / len(flat)
    top = sum(flat[:max(1, len(flat) // 10)]) / sum(flat) * 100.0
    print(f"selftest uniform shift : coverage {cov:.1f}% (expect 100), top-decile {top:.1f}% (expect ~10)")
    if cov < 99.0 or top > 20.0:
        print("  FAIL: a uniform difference did not read as spread out"); ok = False

    a, b = synth(W, H, "blob")
    t = tile_stats(a, b, W, H, C, R)
    flat = sorted((v for row in t for v in row), reverse=True)
    cov = 100.0 * sum(1 for v in flat if v > 0) / len(flat)
    top = sum(flat[:max(1, len(flat) // 10)]) / sum(flat) * 100.0
    print(f"selftest single blob   : coverage {cov:.1f}% (expect <25), top-decile {top:.1f}% (expect >50)")
    if cov > 25.0 or top < 50.0:
        print("  FAIL: a concentrated difference did not read as concentrated"); ok = False

    print("SELFTEST PASSED" if ok else "SELFTEST FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="*")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--cols", type=int, default=16)
    ap.add_argument("--rows", type=int, default=12)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if len(args.frames) != 2:
        print("usage: frame_regions.py <a.rgba> <b.rgba>   (or --selftest)")
        return 2

    a, b = load(args.frames[0]), load(args.frames[1])
    if len(a) != len(b) or not a:
        print(f"REFUSES: sizes {len(a)} vs {len(b)} — not the same framebuffer, nothing compared.")
        return 1
    height = len(a) // 4 // args.width
    if height * args.width * 4 != len(a):
        print(f"REFUSES: {len(a)} bytes is not a whole number of {args.width}px rows.")
        return 1
    tiles = tile_stats(a, b, args.width, height, args.cols, args.rows)
    summarize(tiles, args.cols, args.rows,
              f"{args.frames[0]}  vs  {args.frames[1]}   ({args.width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
