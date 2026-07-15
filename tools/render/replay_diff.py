#!/usr/bin/env python3
"""Pixel-aligned aurora-replay vs Dolphin-playback diff for the SAME .dff.

aurora SB_DUMP_FRAME = 1280x896 (2x of the 640x448 EFB, raw RGBA8, already
normalized to true RGBA by the dump path). Dolphin playback PNG = 640x480
(VI-scaled 448->480). To align we 2x-box-downscale aurora -> 640x448, then
resample BOTH to 640x448 (dolphin 480->448 vertical) so geometry lines up.
Any residual diff is pure aurora render fidelity vs the oracle.

Emits: <out>_aur.png, <out>_dol.png (aligned), <out>_heat.png (abs-diff x3,
clamped), and prints per-region medians. Static geometry near-0 in the heatmap
== alignment is trustworthy; only then are the color numbers meaningful.
"""
import sys, numpy as np
from PIL import Image

def load_aurora_raw(p):
    b = np.frombuffer(open(p,'rb').read(), dtype=np.uint8)
    a = b[:1280*896*4].reshape(896,1280,4)[:,:,:3].astype(np.float32)
    # 2x box downscale -> 448x640
    a = (a[0::2,0::2]+a[1::2,0::2]+a[0::2,1::2]+a[1::2,1::2])/4.0
    return a  # 448x640x3

def main():
    aur_raw, dol_png, out = sys.argv[1], sys.argv[2], sys.argv[3]
    aur = load_aurora_raw(aur_raw)                       # 448x640
    dol = Image.open(dol_png).convert('RGB').resize((640,448), Image.BILINEAR)
    dol = np.asarray(dol).astype(np.float32)             # 448x640
    Image.fromarray(aur.astype(np.uint8)).save(out+'_aur.png')
    Image.fromarray(dol.astype(np.uint8)).save(out+'_dol.png')
    d = np.abs(aur-dol)
    heat = np.clip(d*3,0,255).astype(np.uint8)
    Image.fromarray(heat).save(out+'_heat.png')
    print(f'whole-frame meanABS={d.mean():.2f}')
    # coarse grid to SEE where difference lives
    H,W,_=d.shape; gh,gw=H//8,W//8
    print('  8x8 meanABS grid (rows top->bottom):')
    for r in range(8):
        print('   '+''.join(f'{d[r*gh:(r+1)*gh,c*gw:(c+1)*gw].mean():5.0f}' for c in range(8)))
if __name__=='__main__': main()
