#!/usr/bin/env python3
"""Extract per-draw pos/nrm matrix palettes from an SB_LOG=pn draw-dump log.

Input: a log containing [draw-dump] lines plus [pn] lines
  [pn] #N skinned=S cur=C
  [pn] pos #N M [r0 | r1 | r2]
  [pn] nrm #N M [r0 | r1 | r2]
Outputs one record per draw: draw index, marker, verts, tex0 dims, tev count,
skinned flag, and for each palette slot the nrm 3x3 determinant + row norms —
the sanity signal for "garbage vs valid" skinning matrices. --full prints the
raw matrices for a single draw (--draw N) for exact replay-vs-native diffing.
Refuses logs with zero [pn] lines (degenerate input).
"""
import re, sys, argparse
import numpy as np

def parse(path):
    draws = {}
    dd_re = re.compile(r"\[draw-dump\] #(\d+) prim=\d+ verts=(\d+) tex0=(\d+x\d+).*?tev=(\d+).*?mark='([^']*)'")
    pn_re = re.compile(r"\[pn\] #(\d+) skinned=(\d) cur=(\d+)")
    mat_re = re.compile(r"\[pn\] (pos|nrm) #(\d+) (\d+) \[([^\]]+)\]")
    for line in open(path, 'r', errors='replace'):
        m = dd_re.search(line)
        if m:
            d = draws.setdefault(int(m.group(1)), {})
            d['verts'] = int(m.group(2)); d['tex0'] = m.group(3)
            d['tev'] = int(m.group(4)); d['mark'] = m.group(5)
            continue
        m = pn_re.search(line)
        if m:
            d = draws.setdefault(int(m.group(1)), {})
            d['skinned'] = m.group(2) == '1'; d['cur'] = int(m.group(3))
            continue
        m = mat_re.search(line)
        if m:
            d = draws.setdefault(int(m.group(2)), {})
            rows = [[float(x) for x in r.split()] for r in m.group(4).split('|')]
            d.setdefault(m.group(1), {})[int(m.group(3))] = np.array(rows)
    if not any('nrm' in d for d in draws.values()):
        sys.exit(f"pn_extract: no [pn] matrix lines in {path} — wrong log or SB_LOG=pn not enabled")
    return draws

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log'); ap.add_argument('--skinned-only', action='store_true')
    ap.add_argument('--draw', type=int); ap.add_argument('--full', action='store_true')
    a = ap.parse_args()
    draws = parse(a.log)
    for i in sorted(draws):
        d = draws[i]
        if a.draw is not None and i != a.draw: continue
        if a.skinned_only and not d.get('skinned'): continue
        if 'nrm' not in d: continue
        dets = {mi: float(np.linalg.det(M[:, :3])) for mi, M in sorted(d['nrm'].items())}
        bad = [mi for mi, dt in dets.items() if not (0.5 < abs(dt) < 2.0)]
        print(f"#{i} verts={d.get('verts','?')} tex0={d.get('tex0','?')} tev={d.get('tev','?')} "
              f"skinned={int(d.get('skinned', False))} cur={d.get('cur','?')} mark='{d.get('mark','')}' "
              f"nrmDet={[f'{dets[mi]:.3f}' for mi in sorted(dets)]} badSlots={bad}")
        if a.full:
            for kind in ('pos', 'nrm'):
                for mi, M in sorted(d.get(kind, {}).items()):
                    print(f"  {kind}[{mi}] {np.array2string(M, precision=4, suppress_small=True)}")

if __name__ == '__main__':
    main()
