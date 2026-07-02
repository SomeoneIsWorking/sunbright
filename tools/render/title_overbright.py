#!/usr/bin/env python3
"""title_overbright.py — quantify the SB_OWN_GXLIST title screen OVERBRIGHT as a VALUE.

The geometry gap is closed by SB_OWN_GXLIST (see debug_journal/2026-06-30_fileselect_geometry_
gap_is_ownlist.md), but on that foundation the scene renders OVERBRIGHT vs the Dolphin-GX oracle.
This turns that into a number a fix must move: per-channel + 4x4 per-region MEAN-RGB delta between a
SETTLED native frame and the settled Dolphin-GX oracle, plus the std (a std-PRESERVING positive
delta == an additive brightness offset; a std-SCALING delta == a multiply).

Capture the inputs first:
  ORACLE: tools/render/title_oracle.sh   -> scratch/oracle/title_gx_oracle.png
  NATIVE: build-native/sms-boot ... SB_OWN_GXLIST=1 SB_FRAME_DUMP=1 SB_FRAME_DUMP_SETTLE=1
          (the settle-gated dump waits for the option camera to settle) -> newest scratch/frames/boot_*.ppm

Usage: title_overbright.py [native.ppm] [oracle.png]
  defaults: newest scratch/frames/boot_*.ppm  +  scratch/oracle/title_gx_oracle.png
A fix is good iff the per-channel |delta| shrinks toward 0 with std staying matched.
"""
import sys, glob, os
import numpy as np
from PIL import Image

def newest_native():
    fs = glob.glob('scratch/frames/boot_*.ppm')
    if not fs: sys.exit("no scratch/frames/boot_*.ppm — run sms-boot with SB_FRAME_DUMP_SETTLE=1")
    return max(fs, key=lambda f: int(f.split('_')[-1].split('.')[0]))

def main():
    npath = sys.argv[1] if len(sys.argv) > 1 else newest_native()
    opath = sys.argv[2] if len(sys.argv) > 2 else 'scratch/oracle/title_gx_oracle.png'
    n = np.asarray(Image.open(npath).convert('RGB')).astype(float)
    H, W = n.shape[:2]
    # Rescale the oracle to the native frame size (the oracle is full-res; framing matches per
    # memory fileselect-camera-is-correct, so mean/region brightness is comparable after resize).
    o = np.asarray(Image.open(opath).convert('RGB').resize((W, H))).astype(float)
    print(f"native: {os.path.basename(npath)} ({W}x{H})   oracle: {os.path.basename(opath)}")
    nm, om = n.reshape(-1,3).mean(0), o.reshape(-1,3).mean(0)
    ns, os_ = n.reshape(-1,3).std(0), o.reshape(-1,3).std(0)
    print(f"  channel   native            oracle            delta(N-O)        std N / O")
    for i,ch in enumerate('RGB'):
        print(f"    {ch}      {nm[i]:7.1f}           {om[i]:7.1f}           {nm[i]-om[i]:+7.1f}           {ns[i]:5.1f} / {os_[i]:5.1f}")
    # channel-mean |delta| — the OLD headline metric. MISLEADING: it computes each channel's mean
    # separately then takes |delta|, so a scene that is +80 in the sky and -80 in the ground reports
    # ~0. Kept as `channel_mean_delta` for continuity; the real headline is `mean_abs_pixel_delta`.
    dmean = float(np.abs(nm-om).mean())
    # TRUE per-pixel mean |delta| — average |N-O| over every pixel and channel. This is the number
    # a real fidelity fix must move; cannot be fooled by sky-vs-ground error cancellation.
    dmean_pix = float(np.abs(n-o).mean())
    stds_match = bool(np.all(np.abs(ns-os_) < 0.25*np.maximum(os_,1)))
    print(f"  channel_mean_delta:   {dmean:5.1f}   (MISLEADING: cancels sky-vs-ground errors)")
    print(f"  mean_abs_pixel_delta: {dmean_pix:5.1f}   (TRUE fidelity metric — move THIS)")
    print(f"  std-preserving(additive)={stds_match}")
    # 4x4 region grid: signed mean-RGB delta (shows +/- overbright/underbright direction) AND
    # mean|delta| per region (shows magnitude regardless of sign — a region that is +80 in one half
    # and -80 in the other reports huge |delta| but tiny signed delta).
    print("  region SIGNED delta(N-O) mean-RGB (4x4 grid, top-left -> bottom-right):")
    for ry in range(4):
        row = []
        for rx in range(4):
            ys, ye = ry*H//4, (ry+1)*H//4
            xs, xe = rx*W//4, (rx+1)*W//4
            d = n[ys:ye,xs:xe].reshape(-1,3).mean(0) - o[ys:ye,xs:xe].reshape(-1,3).mean(0)
            row.append(f"[{d[0]:+5.0f},{d[1]:+5.0f},{d[2]:+5.0f}]")
        print("    " + " ".join(row))
    print("  region mean_abs_pixel_delta (4x4 grid):")
    for ry in range(4):
        row = []
        for rx in range(4):
            ys, ye = ry*H//4, (ry+1)*H//4
            xs, xe = rx*W//4, (rx+1)*W//4
            row.append(f"{float(np.abs(n[ys:ye,xs:xe]-o[ys:ye,xs:xe]).mean()):5.1f}")
        print("    " + "  ".join(row))

if __name__ == '__main__':
    main()
