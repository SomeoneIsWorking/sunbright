#!/usr/bin/env python3
"""ab_diff — turn the /abshot2 zero-drift dual capture into a NUMBER (the renderer's
pixel-level test vs Dolphin). Reads the Dolphin-GX oracle and the ngx render captured
from the IDENTICAL present and reports how far ngx is from Dolphin — mean/worst pixel
delta overall and per screen-region — plus a diff heatmap. This is the only sound ngx
test (CPU-side xfmem is async-lagged; see memory xfmem-not-cpu-oracle): the oracle is
rendered PIXELS, freshly produced by Dolphin each run, not a stored golden.

Usage: ab_diff.py [gx.ppm] [ngx.ppm] [--heat out.ppm]
  defaults: scratch/screenshots/ab2.gx.ppm  scratch/screenshots/ab2.ngx.ppm
"""
import sys, struct

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary P6 PPM")
    # parse header: P6 W H MAXVAL\n<pixels>, tolerating whitespace/comments
    idx = 2; tok = []
    def nexttok(i):
        while i < len(data) and data[i:i+1].isspace(): i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i:i+1] != b"\n": i += 1
            return nexttok(i)
        s = i
        while i < len(data) and not data[i:i+1].isspace(): i += 1
        return data[s:i], i
    w, idx = nexttok(idx); h, idx = nexttok(idx); mx, idx = nexttok(idx)
    idx += 1  # single whitespace after maxval
    w, h = int(w), int(h)
    px = data[idx: idx + w*h*3]
    return w, h, px

def main():
    heat = None
    argv = sys.argv[1:]
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--heat":
            heat = argv[i+1]; i += 2; continue
        if argv[i].startswith("--"):
            i += 1; continue
        args.append(argv[i]); i += 1
    gx = args[0] if len(args) > 0 else "scratch/screenshots/ab2.gx.ppm"
    ng = args[1] if len(args) > 1 else "scratch/screenshots/ab2.ngx.ppm"

    gw, gh, gp = read_ppm(gx)
    nw, nh, np_ = read_ppm(ng)
    if (gw, gh) != (nw, nh):
        print(f"SIZE MISMATCH: oracle {gw}x{gh} vs ngx {nw}x{nh} — cannot diff")
        return 2
    w, h = gw, gh
    n = w * h

    # Degenerate-input guard. Under the no-recomp NGX_PRESENT architecture Dolphin does NOT render
    # the guest GX draws (ngx replaces them), so /abshot2's "GX oracle" comes back all-black — and a
    # diff against a black oracle silently reports a meaningless ~40% (it nearly got read as a real
    # regression, 2026-06-19). Refuse it loudly instead. For a real GX oracle run a SEPARATE process
    # with SUNBRIGHT_NGX_PRESENT=0 (Dolphin-GX baseline), ideally frame-matched via SUNBRIGHT_STATE.
    def frame_stats(px):
        s = 0; nb = 0
        for i in range(0, len(px), 3):
            t = px[i] + px[i+1] + px[i+2]
            s += t
            if t > 30: nb += 1
        return s / max(1, len(px)), nb / max(1, len(px)//3)
    gmean, gnb = frame_stats(gp)
    nmean, nnb = frame_stats(np_)
    for label, path, mean, nb in (("oracle/GX", gx, gmean, gnb), ("ngx", ng, nmean, nnb)):
        if nb < 0.01:
            print(f"EMPTY FRAME: {label} ({path}) is ~all black (mean={mean:.2f}, nonblack={nb*100:.2f}%).")
            if label.startswith("oracle"):
                print("  The GX oracle is empty — under no-recomp NGX_PRESENT Dolphin renders no GX XFB.")
                print("  Capture the oracle from a SEPARATE SUNBRIGHT_NGX_PRESENT=0 run (frame-match via SUNBRIGHT_STATE).")
            print("  Refusing to report a meaningless delta against an empty frame.")
            return 3

    # overall + per-region (4x4 grid) mean abs delta, 0..255 per channel averaged
    GRID = 4
    reg_sum = [0]*(GRID*GRID); reg_cnt = [0]*(GRID*GRID)
    total = 0; worst = 0
    heatbuf = bytearray(n*3) if heat else None
    for i in range(n):
        o = i*3
        d = abs(gp[o]-np_[o]) + abs(gp[o+1]-np_[o+1]) + abs(gp[o+2]-np_[o+2])
        d //= 3
        total += d
        if d > worst: worst = d
        x = (i % w) * GRID // w; y = (i // w) * GRID // h
        r = y*GRID + x; reg_sum[r] += d; reg_cnt[r] += 1
        if heatbuf is not None:
            v = 255 if d > 255 else d
            heatbuf[o] = v; heatbuf[o+1] = 0 if v < 64 else (v-64); heatbuf[o+2] = 0

    mean = total / n
    print(f"oracle={gx}  ngx={ng}  ({w}x{h})")
    print(f"MEAN abs pixel delta: {mean:6.2f} / 255   ({mean/255*100:.1f}%)   worst pixel: {worst}")
    print("per-region mean delta (4x4 grid, top-left -> bottom-right):")
    for y in range(GRID):
        row = "  " + " ".join(f"{reg_sum[y*GRID+x]/max(1,reg_cnt[y*GRID+x]):6.1f}" for x in range(GRID))
        print(row)
    if heatbuf is not None:
        with open(heat, "wb") as f:
            f.write(f"P6\n{w} {h}\n255\n".encode()); f.write(bytes(heatbuf))
        print(f"heatmap -> {heat}")
    # exit code reflects pass/fail against a coarse threshold (drive this down)
    return 0 if mean < 8.0 else 1

if __name__ == "__main__":
    sys.exit(main())
