#!/usr/bin/env python3
"""divergence_scan — AUTOMATICALLY find which ngx draw batch diverges from the GX oracle.

The renderer composites the scene as an ordered list of draw batches (one per material/
texture run). /ngxprefix?n=N makes the native present draw only the first N batches; the
GX oracle (Dolphin XFB) is unaffected, so it always shows the FULL scene. /abshot2 writes
both from the IDENTICAL present (zero camera drift). So:

    delta(N) = mean |ngx(first N batches) - GX(full)|

As N grows, ngx builds up the scene; a FAITHFUL batch lowers delta (or leaves it), a
DIVERGING batch (e.g. an over-bright additive layer washing the frame) RAISES it. The
batch whose addition most increases the delta is the diverging render — reported with its
ti/blend (from /ngxorder) and the 4x4 region where it hurts most. No eyeballing, no oracle
of an isolated layer: it bisects the composite against rendered pixels.

Usage: divergence_scan.py [--port 17654] [--step 1] [--top 8] [--shots DIR]
Requires a running headless game with SUNBRIGHT_NGX_PRESENT=1 SUNBRIGHT_PROBE=1 at a 3D
scene (frame_swaps>=540). Drive the boot from scratch/ngx_divscan.sh.
"""
import sys, time, urllib.request, os, re

def get(port, path, timeout=8):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")

def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    assert d[:2] == b"P6", f"{path} not P6"
    i = 2; vals = []
    while len(vals) < 3:
        while d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b"#":
            while d[i:i+1] != b"\n": i += 1
            continue
        s = i
        while not d[i:i+1].isspace(): i += 1
        vals.append(int(d[s:i]))
    w, h, _ = vals
    i += 1
    return w, h, d[i:i+w*h*3]

def to_np(path):
    import numpy as np
    w, h, px = read_ppm(path)
    return np.frombuffer(px, np.uint8).reshape(h, w, 3).astype(np.int16)

def region_grid(diff):
    import numpy as np
    H, W = diff.shape[:2]; g = np.zeros((4, 4))
    for r in range(4):
        for c in range(4):
            g[r, c] = diff[r*H//4:(r+1)*H//4, c*W//4:(c+1)*W//4].mean()
    return g

def parse_order(txt):
    """/ngxorder → list of dicts in DRAW order (the prefix index space)."""
    out = []
    for ln in txt.splitlines():
        m = re.search(r"\[\s*(\d+)\]\s*ti=(-?\d+)\s+nv=(\d+)\s+epoch=(\d+)\s+cc=([0-9a-fA-F]+)\s+blend=(\d+)\s+src=(\d+)\s+dst=(\d+)\s+atest=(\d+)", ln)
        if m:
            out.append(dict(idx=int(m[1]), ti=int(m[2]), nv=int(m[3]), epoch=int(m[4]),
                            cc=m[5], blend=int(m[6]), src=int(m[7]), dst=int(m[8]), atest=int(m[9])))
    return out

def capture(port, shots):
    # two pulses (the first arms, the second lands a fresh dual capture)
    get(port, "/abshot2"); time.sleep(0.15); get(port, "/abshot2"); time.sleep(0.05)
    return os.path.join(shots, "ab2.gx.ppm"), os.path.join(shots, "ab2.ngx.ppm")

def scan_loo(port, shots, top):
    """LEAVE-ONE-OUT by ti: skip each layer class from the FULL render; if the pixel match
    IMPROVES, that layer over-contributes (it is the diverging render). Robust to draw-order
    overdraw (unlike the prefix sweep), which is why this is the primary mode."""
    import numpy as np
    get(port, "/ngxskipset"); time.sleep(0.25)
    gxp, ngxp = capture(port, shots)
    GX = to_np(gxp); base = float(np.abs(to_np(ngxp) - GX).mean())
    import shutil; shutil.copy(gxp, os.path.join(shots, "scan_gx.ppm"))
    order = parse_order(get(port, "/ngxorder"))
    # distinct ti in draw order, with summed vcount
    tis = {}
    for b in order:
        tis.setdefault(b["ti"], dict(ti=b["ti"], nv=0, n=0, blend=b["blend"], src=b["src"], dst=b["dst"], cc=b["cc"]))
        tis[b["ti"]]["nv"] += b["nv"]; tis[b["ti"]]["n"] += 1
    print(f"[divscan loo] full delta={base:.2f}; {len(tis)} layer classes")
    rows = []
    for ti, info in tis.items():
        get(port, f"/ngxskipset?ti={ti}"); time.sleep(0.25)
        _, ngxp = capture(port, shots)
        NG = to_np(ngxp)
        if NG.shape != GX.shape: continue
        d = float(np.abs(NG - GX).mean())
        improve = base - d   # >0 => removing this layer IMPROVES the match => diverging
        rows.append((improve, ti, d, info))
        print(f"  skip ti={ti:4d} (n={info['n']} nv={info['nv']:5d} bl{info['blend']} {info['src']}/{info['dst']} cc={info['cc']}) -> delta={d:6.2f}  improve={improve:+.2f}")
    get(port, "/ngxskipset")
    # first-appearance (draw order) of each ti, so "diverges first" is well-defined
    first_idx = {}
    for b in order:
        first_idx.setdefault(b["ti"], b["idx"])
    rows.sort(key=lambda x: -x[0])
    print("\n=== RANKED diverging layers (removing it IMPROVES match => it over-contributes) ===")
    for improve, ti, d, info in rows[:top]:
        tag = "  <== DIVERGING" if improve > 0.5 else ""
        print(f"  ti={ti:4d} improve={improve:+6.2f} (delta {base:.2f}->{d:.2f}) drawn@#{first_idx[ti]} "
              f"n={info['n']} nv={info['nv']} blend={info['blend']} {info['src']}/{info['dst']} cc={info['cc']}{tag}")
    div = sorted([r for r in rows if r[0] > 0.5], key=lambda r: first_idx[r[1]])
    print("\n=== DIVERGES FIRST (net-harmful layers, in DRAW ORDER) ===")
    if div:
        for improve, ti, d, info in div:
            print(f"  drawn@#{first_idx[ti]:2d}  ti={ti:4d}  improve={improve:+.2f}  blend={info['blend']} {info['src']}/{info['dst']} cc={info['cc']}")
        improve, ti, d, info = div[0]
        print(f"  -> FIRST: ti={ti} (drawn @#{first_idx[ti]}). WHY: curl /gxstate?ti={ti}  HOW: removing it drops delta {base:.1f}->{d:.1f}")
    else:
        print("  none past threshold")
    print(f"\nfull delta={base:.2f}. Positive 'improve' = that layer is the diverging render.")

def scan_walk(port, shots, top):
    """DRAW-ORDER WALK: build the scene one layer at a time (prefix N) and find the FIRST
    layer whose own on-screen footprint diverges from the GX oracle — i.e. which layer
    diverges first, where, and by how much.

    Overdraw-robust: capture every prefix image, attribute each FINAL pixel to the LAST
    layer that changed it (its 'owner' = what's actually visible there), then for each layer
    measure |ngx - GX| over the pixels it owns. A layer with a small owned-footprint delta
    painted those pixels correctly; the first layer (in draw order) with a large owned delta
    is the first divergence. The 'why' = run /gxstate?ti=<that layer> next; the 'how' = the
    region + delta printed here."""
    import numpy as np
    get(port, "/ngxskipset"); time.sleep(0.2)
    order = parse_order(get(port, "/ngxorder"))
    total = len(order)
    get(port, "/ngxprefix?n=-1"); time.sleep(0.2)
    gxp, _ = capture(port, shots)
    GX = to_np(gxp)
    import shutil; shutil.copy(gxp, os.path.join(shots, "scan_gx.ppm"))
    # capture prefix images 0..total
    imgs = []
    for N in range(0, total + 1):
        get(port, f"/ngxprefix?n={N}"); time.sleep(0.22)
        _, ngxp = capture(port, shots)
        imgs.append(to_np(ngxp))
    get(port, "/ngxprefix?n=-1")
    H, W = GX.shape[:2]
    final = imgs[total]
    # owner[y,x] = last layer index (0..total-1) whose draw changed that pixel; -1 = never (clear)
    owner = np.full((H, W), -1, np.int32)
    for N in range(1, total + 1):
        changed = np.any(imgs[N] != imgs[N-1], axis=2)
        owner[changed] = N - 1            # batch index that produced prefix N
    absdiff = np.abs(final.astype(np.int16) - GX).mean(axis=2)   # per-pixel final delta vs GX
    print(f"[divscan walk] {total} batches; full delta={absdiff.mean():.2f}")
    print(f"{'idx':>3} {'ti':>4} {'blend':>5} {'own_px':>7} {'ownΔvsGX':>9} {'cumΔ':>6}  region(r,c)  cc")
    rows = []
    for b in order:
        idx = b["idx"]
        m = owner == idx
        npx = int(m.sum())
        od = float(absdiff[m].mean()) if npx else 0.0
        # cumulative full-frame delta after this layer (prefix idx+1)
        cum = float(np.abs(imgs[idx+1].astype(np.int16) - GX).mean())
        # region of this layer's worst owned divergence
        rr = cc = -1; gv = 0.0
        if npx:
            dd = np.where(m, absdiff, 0)
            grid = region_grid(dd[..., None].repeat(3, 2))
            rr, cc = np.unravel_index(np.argmax(grid), grid.shape); gv = grid[rr, cc]
        rows.append((idx, b, npx, od, cum, rr, cc))
        print(f"{idx:3d} {b['ti']:4d} {b['blend']}:{b['src']}/{b['dst']:>1} {npx:7d} {od:9.1f} {cum:6.1f}  r{rr}c{cc}  cc={b['cc']}")
    # first divergence: earliest layer with a meaningful visible footprint that diverges
    print("\n=== FIRST DIVERGENCE (earliest draw-order layer whose VISIBLE footprint is far from GX) ===")
    cand = [r for r in rows if r[2] >= max(200, H*W//500) and r[3] >= 35.0]
    if cand:
        idx, b, npx, od, cum, rr, cc = cand[0]
        print(f"  -> batch #{idx} ti={b['ti']} blend={b['blend']} {b['src']}/{b['dst']} cc={b['cc']}: "
              f"{npx} visible px, owned delta vs GX = {od:.1f} (region r{rr}c{cc})")
        print(f"     WHY: curl /gxstate?ti={b['ti']}  HOW: this layer paints {npx}px that end up {od:.0f}/255 off the oracle")
    else:
        print("  no single layer's visible footprint diverges past threshold (distributed/desync residual)")
    # also rank by total owned divergence contribution (npx * od)
    rows.sort(key=lambda r: -(r[2] * r[3]))
    print("\n  layers by total visible divergence (owned_px * owned_delta):")
    for idx, b, npx, od, cum, rr, cc in rows[:top]:
        print(f"   #{idx} ti={b['ti']} {b['blend']}:{b['src']}/{b['dst']} owned={npx} Δ={od:.1f} total={npx*od/1000:.0f}k  cc={b['cc']}")

def main():
    import numpy as np
    port = 17654; step = 1; top = 8; mode = "loo"
    shots = os.path.join(os.path.dirname(__file__), "..", "..", "scratch", "screenshots")
    a = sys.argv[1:]
    for i, t in enumerate(a):
        if t == "--port": port = int(a[i+1])
        elif t == "--step": step = int(a[i+1])
        elif t == "--top": top = int(a[i+1])
        elif t == "--mode": mode = a[i+1]
        elif t == "--shots": shots = a[i+1]
    shots = os.path.abspath(shots)
    get(port, "/ngxfreeze?on=1"); time.sleep(0.8)
    if mode == "loo":
        scan_loo(port, shots, top); return
    if mode == "walk":
        scan_walk(port, shots, top); return
    order = parse_order(get(port, "/ngxorder"))
    total = len(order)
    print(f"[divscan] {total} drawn batches; sweeping prefix N=0..{total} step={step}")

    # fixed GX oracle (full scene) — capture once at full prefix
    get(port, "/ngxprefix?n=-1"); time.sleep(0.2)
    gxp, _ = capture(port, shots)
    GX = to_np(gxp)
    gx_full = os.path.join(shots, "scan_gx.ppm")
    import shutil; shutil.copy(gxp, gx_full)

    Ns = list(range(0, total + 1, step))
    if Ns[-1] != total: Ns.append(total)
    deltas = {}; grids = {}
    for N in Ns:
        get(port, f"/ngxprefix?n={N}"); time.sleep(0.25)
        _, ngxp = capture(port, shots)
        NG = to_np(ngxp)
        if NG.shape != GX.shape:
            print(f"  N={N}: shape mismatch {NG.shape} vs {GX.shape}; skip"); continue
        diff = np.abs(NG - GX)
        deltas[N] = float(diff.mean())
        grids[N] = region_grid(diff)
        print(f"  N={N:4d} delta={deltas[N]:6.2f}")
    get(port, "/ngxprefix?n=-1")

    # rank batches by how much ADDING them WORSENS the delta (delta[N] - delta[N-step])
    rows = []
    prev = None
    for N in Ns:
        if N not in deltas: continue
        if prev is not None and prev in deltas:
            d = deltas[N] - deltas[prev]
            # the batches added in (prev, N]
            added = [b for b in order if prev <= b["idx"] < N]
            # region of biggest worsening
            gd = grids[N] - grids[prev]
            rr, cc = np.unravel_index(np.argmax(gd), gd.shape)
            rows.append((d, prev, N, added, (rr, cc, gd[rr, cc])))
        prev = N
    rows.sort(key=lambda x: -x[0])

    print("\n=== RANKED diverging batches (addition that most WORSENED match vs GX) ===")
    print(f"{'Δdelta':>8}  prefix    region(r,c,Δ)     batches[idx ti blend src/dst]")
    for d, p, N, added, (rr, ccc, gv) in rows[:top]:
        if d <= 0: continue
        bs = " ".join(f"#{b['idx']}(ti{b['ti']},bl{b['blend']},{b['src']}/{b['dst']})" for b in added)
        print(f"{d:8.2f}  {p}->{N}   r{rr}c{ccc}+{gv:.1f}    {bs}")
    best = min(deltas, key=deltas.get)
    print(f"\nbest prefix N={best} delta={deltas[best]:.2f} (full N={total} delta={deltas[total]:.2f})")
    print("(a batch with large +Δdelta is the diverging render; map idx→layer via /ngxorder)")

if __name__ == "__main__":
    main()
