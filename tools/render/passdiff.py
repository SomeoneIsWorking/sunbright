#!/usr/bin/env python3
"""passdiff.py — the INTERLEAVED per-pass framebuffer A/B differ for the file-select composite.

The file-select frame is a render-to-EFB-texture composite: pass1 renders the mirror off-screen
(→ a 256x256 texture), pass2 renders the main scene off-screen (→ a 320x224 soft-focus texture),
pass3 composites those into the visible frame. The GX compatibility renderer flattens all of this
into one forward-composited framebuffer → overbright. The aggregate signature diff (gxstate_diff.py)
can't say WHICH pass first diverges; this tool does, by comparing each engine's framebuffer at the
SHARED pass boundaries (the EFB copies) as IMAGES.

  ORACLE side  (build/sunbright, SUNBRIGHT_DUMP_EFB=1): Dolphin dumps every EFB→texture copy decoded
               to <userdir>/Dump/Textures/<gameid>/efb1_n######_WxH_F.png. Grouped by dims:
               256x256 = pass1 (mirror), 320x224 = pass2 (soft-focus). Final visible frame = the
               oracle XFB PNG (scratch/oracle/fileselect_gx_oracle.png).
  NATIVE side  (build-native/sms-boot, SB_PASS_DUMP=1): dumps its CUMULATIVE framebuffer at each
               copy boundary → scratch/frames/pass{1,2}_native_NNNN.ppm. Final = boot_NNNN.ppm.

For each pass it prints the mean per-channel delta and writes a side-by-side PNG (native | oracle |
heat) to scratch/passes/passdiff_passK.png. The FIRST pass whose delta is large is where the bug
lives — and as the composite is implemented, each pass goes green in order.

Usage: passdiff.py [native_frames_dir] [oracle_efb_dir] [oracle_final_png]
  defaults: scratch/frames  +  (auto-locate Dolphin dump dir)  +  scratch/oracle/fileselect_gx_oracle.png
"""
import sys, glob, os, re
import numpy as np
from PIL import Image

NATIVE_DIR = sys.argv[1] if len(sys.argv) > 1 else 'scratch/frames'
ORACLE_EFB = sys.argv[2] if len(sys.argv) > 2 else None
ORACLE_FIN = sys.argv[3] if len(sys.argv) > 3 else 'scratch/oracle/fileselect_gx_oracle.png'
OUT = 'scratch/passes'
os.makedirs(OUT, exist_ok=True)

def auto_efb_dir():
    home = os.path.expanduser('~')
    for root in (os.path.join(home, '.local/share/dolphin-emu/Dump/Textures'),
                 os.path.join(home, '.dolphin-emu/Dump/Textures'),
                 os.path.join(home, 'Library/Application Support/Dolphin/Dump/Textures')):
        if os.path.isdir(root):
            if glob.glob(os.path.join(root, 'efb1_n*.png')):
                return root                               # dumps land directly in Textures/
            for sub in sorted(glob.glob(os.path.join(root, '*'))):
                if glob.glob(os.path.join(sub, 'efb1_n*.png')):
                    return sub                            # …or under a <gameid>/ subdir
    return None

if ORACLE_EFB is None:
    ORACLE_EFB = auto_efb_dir()
    if ORACLE_EFB is None:
        sys.exit("could not auto-locate the Dolphin EFB dump dir; pass it as arg 2 "
                 "(run the oracle with SUNBRIGHT_DUMP_EFB=1 first)")
print(f"native dir: {NATIVE_DIR}   oracle EFB dir: {ORACLE_EFB}")

def newest(pattern):
    fs = glob.glob(pattern)
    if not fs: return None
    return max(fs, key=lambda f: int(re.findall(r'(\d+)', os.path.basename(f))[-1]))

def load(path):
    return np.asarray(Image.open(path).convert('RGB')).astype(float)

# ── Oracle EFB dumps grouped by dims; highest n###### per (w,h) = the settled copy ──
def readable(path):
    try: Image.open(path).load(); return True
    except Exception: return False
oracle_by_dim = {}   # (w,h) -> (n, path); pick the highest-n READABLE dump (the last may be truncated)
cand = {}
for f in glob.glob(os.path.join(ORACLE_EFB, 'efb1_n*.png')):
    m = re.match(r'efb1_n(\d+)_(\d+)x(\d+)_', os.path.basename(f))
    if not m: continue
    n, w, h = int(m.group(1)), int(m.group(2)), int(m.group(3))
    cand.setdefault((w, h), []).append((n, f))
for key, lst in cand.items():
    for n, f in sorted(lst, reverse=True):
        if readable(f): oracle_by_dim[key] = (n, f); break
print("oracle EFB copies (latest per dim):")
for (w, h), (n, f) in sorted(oracle_by_dim.items()):
    print(f"  {w}x{h}  n={n}  {os.path.basename(f)}")

# ── Native cumulative pass dumps + final ──
native_passes = []   # (label, path, dims_from_copy)
k = 1
while True:
    p = newest(os.path.join(NATIVE_DIR, f'pass{k}_native_*.ppm'))
    if not p: break
    native_passes.append((f'pass{k}', p))
    k += 1
final_native = newest(os.path.join(NATIVE_DIR, 'boot_*.ppm'))

def diff_pair(label, npath, opath):
    n = load(npath)
    o = load(opath)
    H, W = n.shape[:2]
    o_r = np.asarray(Image.fromarray(o.astype('uint8')).resize((W, H))).astype(float)
    d = n - o_r
    mad = np.abs(d).reshape(-1, 3).mean(0)
    print(f"\n=== {label} ===")
    print(f"  native {os.path.basename(npath)} ({W}x{H})  vs  oracle {os.path.basename(opath)}")
    print(f"  mean|delta| RGB = [{mad[0]:6.1f} {mad[1]:6.1f} {mad[2]:6.1f}]   overall={np.abs(d).mean():5.1f}")
    print(f"  native meanRGB={n.reshape(-1,3).mean(0).round(1)}  oracle meanRGB={o_r.reshape(-1,3).mean(0).round(1)}")
    heat = np.clip(np.abs(d).mean(2), 0, 255).astype('uint8')
    heat = np.stack([heat, heat // 2, 255 - heat], -1)
    sbs = np.concatenate([n.astype('uint8'), o_r.astype('uint8'), heat], axis=1)
    out = os.path.join(OUT, f'passdiff_{label}.png')
    Image.fromarray(sbs).save(out)
    print(f"  side-by-side (native | oracle | heat) -> {out}")
    return np.abs(d).mean()

# pass1 = native pass1 vs oracle 256x256 (mirror); pass2 = native pass2 vs 320x224 (soft-focus)
DIM_FOR_PASS = {'pass1': (256, 256), 'pass2': (320, 224)}
for label, npath in native_passes:
    dim = DIM_FOR_PASS.get(label)
    if dim and dim in oracle_by_dim:
        diff_pair(label, npath, oracle_by_dim[dim][1])
    else:
        print(f"\n=== {label} ===  no matching oracle EFB dump for expected dim {dim}; "
              f"oracle dims available: {sorted(oracle_by_dim)}")

# pass3 (visible frame): prefer the oracle's 640x448 full-frame EFB copy (Dolphin-decoded, exact
# same settled state) over the separately-captured XFB PNG; fall back to ORACLE_FIN if absent.
final_oracle = oracle_by_dim.get((640, 448), (None, None))[1] or ORACLE_FIN
if final_native and final_oracle and os.path.exists(final_oracle):
    diff_pair('pass3_final', final_native, final_oracle)
