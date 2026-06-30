#!/usr/bin/env python3
"""blend_drill.py — attribute the file-select main-pass OVERBRIGHT (bug #1) to a blend CLASS, by VALUE.

Pairs with SB_BLEND_DRILL=1 (sms_boot_present.cpp), which in ONE native run dumps the main-pass scene
(phase!=1, so NO ph1 double-draw and NO imm soft-focus quad — directly comparable to the oracle's
320x224 pass2 clean scene) in three forms per distinct blend (src,dst) class:
  drill_base_NNNN.ppm        — all main-pass batches
  drill_drop_S_D_NNNN.ppm    — that blend class removed
  drill_only_S_D_NNNN.ppm    — only that class, over the opaque base

This tool region-diffs each against the oracle pass2 (scratch/passes/oracle_pass2_softfocus.png) and
ranks the classes: how much does REMOVING each class reduce the overbright (= how much that class
over-contributes), and what does each class look like in isolation. A class whose `drop` sharply cuts
the sky/sea overbright is the over-bright layer to fix (its TEV/blend, not its presence — the oracle
has the layer too, just dimmer).

GX blend factor names: 0 ZERO 1 ONE 2 SRCCLR 3 INVSRCCLR 4 SRCALPHA 5 INVSRCALPHA 6 DSTALPHA 7 INVDSTALPHA

Usage: blend_drill.py [frames_dir] [oracle_pass2.png]
"""
import sys, glob, os, re
import numpy as np
from PIL import Image

FR = sys.argv[1] if len(sys.argv) > 1 else 'scratch/frames'
ORACLE = sys.argv[2] if len(sys.argv) > 2 else 'scratch/passes/oracle_pass2_softfocus.png'
FAC = {0:'ZERO',1:'ONE',2:'SRCCLR',3:'INVSRCCLR',4:'SRCALPHA',5:'INVSRCALPHA',6:'DSTALPHA',7:'INVDSTALPHA'}

def newest(pat):
    fs = glob.glob(pat)
    return max(fs, key=lambda f:int(re.findall(r'(\d+)',os.path.basename(f))[-1])) if fs else None

base = newest(os.path.join(FR,'drill_base_*.ppm'))
if not base: sys.exit("no drill_base_*.ppm — run sms-boot with SB_BLEND_DRILL=1 first")
df = int(re.findall(r'(\d+)', os.path.basename(base))[-1])
n0 = np.asarray(Image.open(base).convert('RGB')).astype(float)
H,W = n0.shape[:2]
o = np.asarray(Image.open(ORACLE).convert('RGB').resize((W,H))).astype(float)

# Regions: sky = top 40% rows, sea = 40-70%, beach = bottom 30%.
def regions(img):
    return {'sky': img[:int(H*0.40)], 'sea': img[int(H*0.40):int(H*0.70)], 'beach': img[int(H*0.70):],
            'all': img}
def md(a,b):  # mean abs delta per region
    ra, rb = regions(a), regions(b)
    return {k: float(np.abs(ra[k]-rb[k]).mean()) for k in ra}
def meanrgb(a, reg):
    return regions(a)[reg].reshape(-1,3).mean(0)

b = md(n0, o)
print(f"frame {df}   {W}x{H}   oracle={os.path.basename(ORACLE)}")
print(f"BASE (all main-pass) overbright vs oracle:  sky={b['sky']:.1f} sea={b['sea']:.1f} "
      f"beach={b['beach']:.1f} all={b['all']:.1f}")
print(f"  base sky meanRGB={meanrgb(n0,'sky').round(1)}  oracle sky meanRGB={meanrgb(o,'sky').round(1)}")
print()
print(f"{'class (src/dst)':<26} {'drop→sky':>9} {'Δsky':>7} {'drop→sea':>9} {'Δsea':>7}   (Δ<0 = removing it REDUCES overbright)")
rows = []
for f in sorted(glob.glob(os.path.join(FR, f'drill_drop_*_{df:04d}.ppm'))):
    m = re.match(r'drill_drop_(\d+)_(\d+)_', os.path.basename(f))
    if not m: continue
    s,d = int(m.group(1)), int(m.group(2))
    nd = np.asarray(Image.open(f).convert('RGB')).astype(float)
    dd = md(nd, o)
    rows.append((s,d, dd['sky']-b['sky'], dd['sea']-b['sea'], dd['sky'], dd['sea']))
# rank by how much removing the class reduces the SKY overbright (most negative first)
rows.sort(key=lambda r: r[2])
for s,d,dsky,dsea,sky,sea in rows:
    name = f"{FAC.get(s,s)}/{FAC.get(d,d)}"
    print(f"  {name:<24} {sky:>9.1f} {dsky:>+7.1f} {sea:>9.1f} {dsea:>+7.1f}")
print("\nMost-overbright sky layer = the class whose `drop` most REDUCES sky delta (top of list).")
print("Inspect drill_only_S_D_*.ppm to see that layer in isolation; the fix is its TEV/blend brightness,")
print("not removing it (the oracle has the layer, just dimmer).")
