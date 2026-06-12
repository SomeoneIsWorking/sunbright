#!/usr/bin/env python3
"""Voice-level oracle-vs-native audio comparison (the reliable per-instrument check).

Both sides expose live voices over the SUNBRIGHT_PROBE HTTP REPL:
  oracle  (SUNBRIGHT_DISABLE_RECOMP=1): /vpb — guest DSP voice parameter blocks
          (ground truth: resampling ratio in 4.12 fixed point, per-bus mix volumes,
          ARAM base address of the wave)
  native  (normal recomp run):          /njas — native engine voices (same 4.12 ratio,
          pre-pan volume, bank/prog/key/vel, wave srcHash)

Join key: FNV-1a of the wave's first 64 raw .aw bytes. The native engine computes it at
load (Wave.srcHash); on the oracle side the same bytes sit in ARAM at the VPB base
address, hashed live via /aram?a=<base>&n=40.

Usage:
  vpb_compare.py capture-oracle <probe_url> <out.jsonl> <seconds>
  vpb_compare.py capture-native <probe_url> <out.jsonl> <seconds>
  vpb_compare.py compare <oracle.jsonl> <native.jsonl>

Pitch is compared in cents (exact: both sides are 4.12 resampling ratios). Volume units
differ between sides (DSP bus mix vol vs engine pre-pan gain), so volumes are compared
RELATIVELY: per-wave median volume, normalized so the population median delta is 0 dB —
a wave at +10 dB after normalization is "too loud relative to everything else".
"""
import sys, re, json, time, math, urllib.request
from collections import defaultdict

def get(url):
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.read().decode()

def capture_oracle(base_url, out_path, seconds):
    hash_cache = {}
    re_v = re.compile(r'^v(\d+) en=(\d+) done=(\d+) ratio=([0-9a-f]+)(.*)$')
    re_ch = re.compile(r'ch[0-9a-f]+=(-?\d+)/(-?\d+)')
    re_dolby = re.compile(r'dolby=(-?\d+)/(-?\d+)')
    re_base = re.compile(r'base=([0-9a-f]{4})([0-9a-f]{4})')
    re_fnv = re.compile(r'fnv=([0-9a-f]+)')
    t0 = time.time()
    with open(out_path, 'w') as out:
        while time.time() - t0 < seconds:
            try:
                txt = get(base_url + '/vpb')
            except Exception:
                time.sleep(0.5); continue
            now = round(time.time() - t0, 2)
            for line in txt.splitlines():
                m = re_v.match(line.strip())
                if not m or m.group(2) != '1':
                    continue
                ratio = int(m.group(4), 16)
                rest = m.group(5)
                mb = re_base.search(rest)
                if not mb:
                    continue
                addr = (int(mb.group(1), 16) << 16) | int(mb.group(2), 16)
                if addr == 0:
                    continue
                if addr not in hash_cache:
                    try:
                        mf = re_fnv.search(get(f'{base_url}/aram?a={addr:x}&n=40'))
                        hash_cache[addr] = mf.group(1) if mf else None
                    except Exception:
                        continue
                h = hash_cache[addr]
                if not h:
                    continue
                # Dolby (3D-positioned) voices: the ucode IGNORES channels[6] volumes
                # entirely (Zelda VPB comment) — the live gain is dolby_volume_current.
                # Taking max(ch, dolby) let a STALE channel value mask the live dolby
                # gain (constant-volume artifact: every 3D voice read as never moving).
                md = re_dolby.search(rest)
                if md:
                    vol = abs(int(md.group(1)))
                else:
                    vols = [abs(int(c)) for c, _ in re_ch.findall(rest)]
                    vol = max(vols) if vols else 0
                out.write(json.dumps({'t': now, 'hash': h, 'ratio': ratio,
                                      'vol': vol, 'base': addr}) + '\n')
            out.flush()
            time.sleep(0.1)

def capture_native(base_url, out_path, seconds):
    re_v = re.compile(r'^v(\d+) hash=([0-9a-f]+) wsys=(-?\d+) grp=(-?\d+) wave=(\d+) '
                      r'ratio=([0-9a-f]+) vol=([0-9.]+) key=(\d+) vel=(\d+) bank=(\d+) prog=(\d+)')
    t0 = time.time()
    with open(out_path, 'w') as out:
        while time.time() - t0 < seconds:
            try:
                txt = get(base_url + '/njas')
            except Exception:
                time.sleep(0.5); continue
            now = round(time.time() - t0, 2)
            for line in txt.splitlines():
                m = re_v.match(line.strip())
                if not m:
                    continue
                out.write(json.dumps({'t': now, 'hash': m.group(2), 'ratio': int(m.group(6), 16),
                                      'vol': float(m.group(7)), 'wave': int(m.group(5)),
                                      'wsys': int(m.group(3)), 'grp': int(m.group(4)),
                                      'key': int(m.group(8)), 'vel': int(m.group(9)),
                                      'bank': int(m.group(10)), 'prog': int(m.group(11))}) + '\n')
            out.flush()
            time.sleep(0.1)

def load(path):
    by_hash = defaultdict(list)
    for line in open(path):
        r = json.loads(line)
        by_hash[r['hash']].append(r)
    return by_hash

def cents(a, b):
    return 1200.0 * math.log2(a / b) if a > 0 and b > 0 else float('nan')

def compare(oracle_path, native_path):
    oside, nside = load(oracle_path), load(native_path)
    common = sorted(set(oside) & set(nside))
    print(f'waves: oracle={len(oside)} native={len(nside)} common={len(common)}')
    only_n = set(nside) - set(oside)
    only_o = set(oside) - set(nside)
    if only_n: print(f'  native-only waves (oracle never played them): {len(only_n)}')
    if only_o: print(f'  oracle-only waves (native never played them): {len(only_o)} ← MISSING natively')

    # population volume normalization (units differ between sides)
    def med(v):
        s = sorted(v); return s[len(s) // 2] if s else 0
    # p90 = peak-gain envelope: waves play in MULTIPLE contexts (BGM track + 3D SE) and
    # at varying distances; medians mix contexts. The near-peak gain is the
    # context-independent static-gain check.
    def p90(v):
        s = sorted(v); return s[int(len(s) * 0.9)] if s else 0
    vol_deltas = []
    rows = []
    for h in common:
        ov = [r['vol'] for r in oside[h] if r['vol'] > 0]
        nv = [r['vol'] for r in nside[h] if r['vol'] > 0]
        # pitch: match each distinct native ratio to the nearest distinct oracle ratio
        orat = sorted(set(r['ratio'] for r in oside[h] if r['ratio'] > 0))
        nrat = sorted(set(r['ratio'] for r in nside[h] if r['ratio'] > 0))
        pdiffs = []
        for nr in nrat:
            if orat:
                best = min(orat, key=lambda o: abs(cents(nr, o)))
                pdiffs.append(cents(nr, best))
        meta = nside[h][0]
        vol_db = (20 * math.log10(p90(nv) / p90(ov))) if ov and nv and p90(ov) > 0 and p90(nv) > 0 else None
        if vol_db is not None:
            vol_deltas.append(vol_db)
        rows.append((h, meta, pdiffs, vol_db, len(orat), len(nrat)))
    vol_norm = med(vol_deltas) if vol_deltas else 0.0
    print(f'volume unit normalization: {vol_norm:+.1f} dB (subtracted from all rows)\n')
    print(f"{'hash':8s} {'bank:prog':9s} {'keys':12s} {'pitch Δcents (med/max)':24s} {'vol ΔdB':8s} {'#rat o/n'}")
    def sortkey(row):
        _, _, pd, vdb, _, _ = row
        p = max((abs(x) for x in pd), default=0)
        v = abs(vdb - vol_norm) if vdb is not None else 0
        return -(p + 10 * v)
    for h, meta, pdiffs, vol_db, no, nn in sorted(rows, key=sortkey):
        pmed = med(sorted(pdiffs)) if pdiffs else float('nan')
        pmax = max(pdiffs, key=abs, default=float('nan'))
        vstr = f'{vol_db - vol_norm:+6.1f}' if vol_db is not None else '   n/a'
        keys = sorted(set(r['key'] for r in nside[h]))
        kstr = (','.join(map(str, keys[:4])) + ('…' if len(keys) > 4 else ''))
        print(f'{h:8s} {meta["bank"]:3d}:{meta["prog"]:<5d} {kstr:12s} '
              f'{pmed:+7.1f} / {pmax:+7.1f}      {vstr}   {no}/{nn}')

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'capture-oracle':
        capture_oracle(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    elif cmd == 'capture-native':
        capture_native(sys.argv[2], sys.argv[3], float(sys.argv[4]))
    elif cmd == 'compare':
        compare(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
