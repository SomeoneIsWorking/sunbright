#!/usr/bin/env python3
"""gxstate_diff.py — deterministic per-draw native-vs-oracle GX-state diff.

The file-select overbright is a per-draw colorUpdate divergence: native paints draws the
Dolphin-GX oracle marks [noC] (color writes OFF). This tool ends the manual pass/phase
log-reading oscillation by comparing the two engines' per-draw GX state by VALUE.

The engines do NOT draw 1:1: native merges same-material+same-phase shapes into ~80 batches;
the oracle emits ~1170 per-primitive draws. So an index-align is impossible — the renderer-
neutral join key is the GX-state SIGNATURE (blend src/dst/subtract/enable + TEV stage count).
For each signature we report each engine's colorUpdate/alphaUpdate value-set, draw count, and
vertex total. A signature whose colorUpdate differs between engines IS the divergence.

Inputs are the ordered per-draw dumps:
  ORACLE: build/sunbright (Dolphin-GX) with SUNBRIGHT_DBG_GXDRAW=1  -> [gxdraw] fr=.. i=.. pass=.. cU=.. ...
  NATIVE: build-native/sms-boot with SB_GXDRAW=1                    -> [gxdraw] fr=.. i=.. phase=.. drawbuf=.. cU=.. ...

Usage: gxstate_diff.py <oracle.log> <native.log> [--oracle-frame N] [--native-frame N]
  Defaults: oracle frame = the last fr seen in the oracle log; native frame = last fr in native log.
"""
import sys, re, argparse
from collections import defaultdict

FACTOR = {0:'ZERO',1:'ONE',2:'SRCCLR',3:'INVSRCCLR',4:'SRCALPHA',5:'INVSRCALPHA',6:'DSTALPHA',7:'INVDSTALPHA'}

def fac(n): return FACTOR.get(n, f'?{n}')

def parse(path, kv_keys):
    """Parse [gxdraw] lines into per-frame lists of dicts. kv_keys: extra key=val fields to keep."""
    frames = defaultdict(list)
    pat = re.compile(r'\[gxdraw\]\s+(.*)')
    for line in open(path, 'r', errors='replace'):
        m = pat.search(line)
        if not m: continue
        d = {}
        for tok in m.group(1).split():
            if '=' not in tok: continue
            k, v = tok.split('=', 1)
            d[k] = v
        if 'fr' not in d: continue
        frames[int(d['fr'])].append(d)
    return frames

def sig(d):
    # renderer-neutral GX-state signature (proj excluded: native doesn't emit it per-batch)
    return (int(d['be']), int(d['src']), int(d['dst']), int(d['sub']), int(d['tev']))

def sig_str(s):
    be, src, dst, sub, tev = s
    bm = 'SUBTRACT' if sub else ('BLEND' if be else 'NONE')
    return f'{bm:8s} {fac(src):11s}/{fac(dst):11s} tev={tev}'

def aggregate(draws):
    """signature -> {n, verts, cU(set), aU(set), passes(set)}"""
    agg = defaultdict(lambda: {'n':0,'verts':0,'cU':set(),'aU':set(),'where':set()})
    for d in draws:
        s = sig(d)
        a = agg[s]
        a['n'] += 1
        a['verts'] += int(d.get('v', 0))
        a['cU'].add(int(d['cU']))
        a['aU'].add(int(d['aU']))
        # 'pass' on oracle, 'phase'/'drawbuf' on native
        if 'pass' in d: a['where'].add('p'+d['pass'])
        if 'phase' in d: a['where'].add('ph'+d['phase'])
    return agg

def setstr(s): return '{' + ','.join(str(x) for x in sorted(s)) + '}'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('oracle'); ap.add_argument('native')
    ap.add_argument('--oracle-frame', type=int, default=None)
    ap.add_argument('--native-frame', type=int, default=None)
    args = ap.parse_args()

    ofr = parse(args.oracle, ())
    nfr = parse(args.native, ())
    if not ofr: sys.exit(f"no [gxdraw] lines in oracle log {args.oracle} (run build/sunbright with SUNBRIGHT_DBG_GXDRAW=1)")
    if not nfr: sys.exit(f"no [gxdraw] lines in native log {args.native} (run sms-boot with SB_GXDRAW=1)")
    of = args.oracle_frame if args.oracle_frame is not None else max(ofr)
    nf = args.native_frame if args.native_frame is not None else max(nfr)
    od, nd = ofr[of], nfr[nf]
    print(f"oracle {args.oracle} frame {of}: {len(od)} draws   |   native {args.native} frame {nf}: {len(nd)} batches\n")

    oa, na = aggregate(od), aggregate(nd)
    allsig = sorted(set(oa) | set(na))

    print(f"{'signature':38s} | {'ORACLE n/verts cU aU where':32s} | {'NATIVE n/verts cU aU where':32s} | flag")
    print('-'*140)
    divergences = []
    for s in allsig:
        o = oa.get(s); n = na.get(s)
        def cell(a):
            if not a: return f"{'—':30s}"
            return f"{a['n']:>3d}/{a['verts']:<5d} cU{setstr(a['cU'])} aU{setstr(a['aU'])} {','.join(sorted(a['where']))}"
        flag = ''
        if o and n:
            if o['cU'] != n['cU']:
                flag = '*** cU DIFFERS ***'; divergences.append((s, o, n, 'cU'))
            elif o['aU'] != n['aU']:
                flag = '* aU differs'; divergences.append((s, o, n, 'aU'))
        elif o and not n:
            flag = 'oracle-only'
        elif n and not o:
            flag = 'NATIVE-ONLY'; divergences.append((s, None, n, 'extra'))
        print(f"{sig_str(s):38s} | {cell(o):32s} | {cell(n):32s} | {flag}")

    print('\n=== DIVERGENCES (the per-draw GX-state mismatches that drive the overbright) ===')
    if not divergences:
        print("  none — every shared signature agrees on colorUpdate/alphaUpdate.")
    for s, o, n, kind in divergences:
        if kind == 'cU':
            print(f"  {sig_str(s)}: oracle cU{setstr(o['cU'])} ({o['n']} draws, {o['verts']}v, {','.join(sorted(o['where']))}) "
                  f"vs native cU{setstr(n['cU'])} ({n['n']} batches, {n['verts']}v, {','.join(sorted(n['where']))})")
        elif kind == 'aU':
            print(f"  {sig_str(s)}: alphaUpdate differs — oracle aU{setstr(o['aU'])} vs native aU{setstr(n['aU'])}")
        elif kind == 'extra':
            print(f"  {sig_str(s)}: NATIVE-ONLY ({n['n']} batches, {n['verts']}v, {','.join(sorted(n['where']))}) — no oracle draw of this signature")

if __name__ == '__main__':
    main()
