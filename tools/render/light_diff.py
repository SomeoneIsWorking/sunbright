#!/usr/bin/env python3
"""light_diff.py — cross-engine per-shape lighting diagnostic.

Consumes two per-frame JSONL captures:
  * oracle: build/sunbright with SUNBRIGHT_PARITY_DUMP=... SUNBRIGHT_PARITY_DRAWS=1
                                 SUNBRIGHT_DBG_GXLIGHT=1
    → per-draw records include cc/amb/matc/lm/lights
  * native: build-native/sms-boot with SB_PARITY_DUMP=...
    → per-batch records include cc/amb/matc/lm/lights + phase

For each side, group by (blend-signature, tev-stages, vert-count-bucket) to
build a stable identity that survives coalescing differences, then diff the
raster-stage inputs across phases and across engines. That pins WHICH pipeline
stage (chan_ctrl / amb / matc / lights) diverges — the real "where's the wash"
signal.

Usage:
  tools/render/light_diff.py <oracle.jsonl> <native.jsonl> [<frame_oracle>] [<frame_native>]
    defaults: last frame on each side.
"""
from __future__ import annotations
import json
import sys
from collections import defaultdict


def load_frame(path, frame_no=None):
    frames = defaultdict(list)   # frame -> list of records (scene pass only)
    for line in open(path):
        try: d = json.loads(line)
        except: continue
        f = d.get('frame')
        frames[f].append(d)
    fnums = sorted(f for f in frames if f is not None)
    if frame_no is None:
        # settled-ish: last frame with substantial content
        for f in reversed(fnums):
            recs = frames[f]
            if any(len(r.get('draws', [])) > 20 or len(r.get('batches', [])) > 20 for r in recs):
                frame_no = f; break
    return frame_no, frames.get(frame_no, [])


def oracle_scene_draws(records):
    """Oracle: per-frame has multiple records (pass=scene/hud). Return scene draws only."""
    draws = []
    for r in records:
        if r.get('pass') == 'scene':
            for d in r.get('draws', []):
                if d.get('proj') == 0:   # perspective only
                    draws.append(d)
    return draws


def native_scene_batches(records):
    """Native: one record per frame with a batches array. Filter to scene (perspective)."""
    for r in records:
        # native records don't split pass; take all batches
        return r.get('batches', [])
    return []


def phase_of_oracle_draw(d):
    """Bucket oracle draw's efb_pass into a scene phase label (best-effort)."""
    e = d.get('efb', 0)
    return f"efb{e}"


def phase_of_native(b):
    return f"ph{b.get('ph', 0)}"


def sig_of(d, is_oracle):
    """Stable material-signature: blend factors, tev-stage count, update flags."""
    if is_oracle:
        return (d.get('be'), d.get('src'), d.get('dst'), d.get('tev'),
                d.get('cU'), d.get('aU'))
    else:
        bm = d.get('bm', [0, 0, 0])
        return (bm[0], bm[1], bm[2], d.get('ntex') and d.get('cU') or d.get('cU'),
                d.get('cU'), d.get('aU'))


def light_summary(rec):
    """Reduce (cc, amb, matc, lights) to a compact tuple for comparison."""
    cc = rec.get('cc', 0)
    amb = tuple(round(x, 3) for x in rec.get('amb', [0, 0, 0]))
    matc = tuple(round(x, 3) for x in rec.get('matc', [1, 1, 1, 1]))
    lm = rec.get('lm', 0)
    lights = rec.get('lights', [])
    lc = tuple(sorted(round(sum(l['c']) / 3.0, 3) for l in lights))   # sorted mean-per-light colour
    return {'cc': cc, 'amb': amb, 'matc': matc, 'lm': lm, 'light_col_summary': lc, 'nlights': len(lights)}


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(2)
    ora_path, nat_path = sys.argv[1], sys.argv[2]
    ora_frame = int(sys.argv[3]) if len(sys.argv) > 3 else None
    nat_frame = int(sys.argv[4]) if len(sys.argv) > 4 else None
    ora_f, ora_recs = load_frame(ora_path, ora_frame)
    nat_f, nat_recs = load_frame(nat_path, nat_frame)
    print(f"oracle: {ora_path} frame={ora_f}")
    print(f"native: {nat_path} frame={nat_f}")
    if not ora_recs or not nat_recs:
        print("empty records on one side"); sys.exit(1)

    ora_draws = oracle_scene_draws(ora_recs)
    nat_batches = native_scene_batches(nat_recs)
    print(f"oracle scene-perspective draws: {len(ora_draws)}")
    print(f"native scene batches: {len(nat_batches)}")

    # Per side: group by (signature, phase) → aggregate the light snap.
    # We look at ONE dominant signature at a time to keep the diff readable.
    print("\n──── ORACLE draws by (blend, tev, efb_pass) ────")
    ora_by_sig_phase = defaultdict(list)
    for d in ora_draws:
        sig = sig_of(d, True)
        ora_by_sig_phase[(sig, phase_of_oracle_draw(d))].append(d)
    for (sig, ph), draws in sorted(ora_by_sig_phase.items(), key=lambda kv: -sum(x.get('v',0) for x in kv[1]))[:10]:
        verts = sum(x.get('v', 0) for x in draws)
        # aggregate lighting: cc/amb/matc are usually consistent per material; lights may vary.
        L = light_summary(draws[0])
        print(f"  sig={sig} {ph}: {len(draws)} draws, {verts} verts, "
              f"cc={L['cc']} amb={L['amb']} matc={L['matc']} "
              f"lm={L['lm']} lights={L['nlights']} colSummary={L['light_col_summary']}")

    print("\n──── NATIVE batches by (blend, phase) ────")
    nat_by_sig_phase = defaultdict(list)
    for b in nat_batches:
        sig = sig_of(b, False)
        nat_by_sig_phase[(sig, phase_of_native(b))].append(b)
    for (sig, ph), bs in sorted(nat_by_sig_phase.items(), key=lambda kv: -sum(x.get('vc',0) for x in kv[1]))[:10]:
        verts = sum(x.get('vc', 0) for x in bs)
        L = light_summary(bs[0])
        print(f"  sig={sig} {ph}: {len(bs)} batches, {verts} verts, "
              f"cc={L['cc']} amb={L['amb']} matc={L['matc']} "
              f"lm={L['lm']} lights={L['nlights']} colSummary={L['light_col_summary']}")

    # Focus on the map-material class: bm=(1,4,5) tev=1 cU=1 (matches native's dominant duplicate).
    print("\n══════ FOCUS: bm=(1,4,5) cU=1 material — the wash suspect ══════")
    print("\nORACLE (per efb_pass):")
    for (sig, ph), draws in sorted(ora_by_sig_phase.items()):
        # oracle sig: (be, src, dst, tev, cU, aU)
        if sig[:3] != (1, 4, 5) or sig[4] != 1: continue
        verts = sum(x.get('v', 0) for x in draws)
        L = light_summary(draws[0])
        print(f"  {ph}: {len(draws):4d} draws, {verts:5d} verts, "
              f"cc={L['cc']:04x} amb={L['amb']} matc={L['matc']} "
              f"nlights={L['nlights']} colSum={L['light_col_summary']}")
    print("\nNATIVE (per phase):")
    for (sig, ph), bs in sorted(nat_by_sig_phase.items()):
        # native sig: (be_hi, src, dst, cU_dup, cU, aU) — approx
        if sig[:3] != (1, 4, 5): continue
        verts = sum(x.get('vc', 0) for x in bs)
        L = light_summary(bs[0])
        print(f"  {ph}: {len(bs):4d} batches, {verts:5d} verts, "
              f"cc={L['cc']:04x} amb={L['amb']} matc={L['matc']} "
              f"nlights={L['nlights']} colSum={L['light_col_summary']}")

    # ── DECISIVE STAGE DIAGNOSIS ────────────────────────────────────────────
    print("\n══════ STAGE DIAGNOSIS ══════")
    # Extract lighting summaries for ALL bm=(1,4,5) cU=1 material occurrences across engines.
    ora_snaps = []
    for (sig, ph), draws in ora_by_sig_phase.items():
        if sig[:3] != (1, 4, 5) or sig[4] != 1: continue
        for d in draws: ora_snaps.append((ph, light_summary(d), d.get('v', 0)))
    nat_snaps = []
    for (sig, ph), bs in nat_by_sig_phase.items():
        if sig[:3] != (1, 4, 5): continue
        for b in bs: nat_snaps.append((ph, light_summary(b), b.get('vc', 0)))
    def sample(snaps):
        # pick the FIRST snap per phase as representative
        by_ph = {}
        for ph, L, v in snaps:
            if ph not in by_ph: by_ph[ph] = L
        return by_ph
    ora_ph = sample(ora_snaps)
    nat_ph = sample(nat_snaps)
    print("Per-phase raster-stage inputs (bm=(1,4,5) cU=1 material):")
    all_phs = sorted(set(list(ora_ph.keys()) + list(nat_ph.keys())))
    for ph in all_phs:
        o = ora_ph.get(ph, {})
        print(f"  {ph:>10s}  ORACLE cc={o.get('cc','-')!r:>8} amb={o.get('amb','-')} matc={o.get('matc','-')} lights={o.get('nlights','-')}")
        n = nat_ph.get(ph, {})
        print(f"             NATIVE cc={n.get('cc','-')!r:>8} amb={n.get('amb','-')} matc={n.get('matc','-')} lights={n.get('nlights','-')}")

if __name__ == '__main__':
    main()
