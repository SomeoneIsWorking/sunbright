#!/usr/bin/env python3
"""parity_sweep.py — pure-Dolphin (oracle) vs sms-boot (PC-native) divergence detection.

VALUE-LEVEL only: compares the renderer-INDEPENDENT game state both engines compute from the
same J3D data — projection/viewport, GX light state, and per-model joint/draw matrices. (The
GX call args / J3D objects are the valid oracle; Dolphin's read-back xfmem is async-lagged, so
we never compare that, and we don't pixel-diff.) Both engines fastboot the same Delfino state
and dump the same JSONL schema; if either renders wrong, the matrices diverge here first.

  parity_sweep.py check dump.jsonl          # invariants on one dump (NaN/empty/off-screen/...)
  parity_sweep.py diff  oracle.jsonl native.jsonl   # find divergences (pure-Dolphin vs sms-boot)

Dump emitters:
  • sms-boot:      SB_PARITY_DUMP=path  (native/render/sb_parity_dump.h)
  • pure Dolphin:  tools/render/dolphin_j3d_probe.py  (reads guest J3D state via the probe)
"""
import sys, json, math

# ---------------------------------------------------------------------------------------
# VALUE track
# ---------------------------------------------------------------------------------------
def load_jsonl(path):
    out = []
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                out.append(json.loads(ln))
    return out

def check(path):
    """Invariant sweep over one native dump. Fails loudly on the failure classes that the
    geometry/lighting bugs this harness was built for produce."""
    frames = load_jsonl(path)
    if not frames:
        print(f"FAIL: {path} has no frames"); return 1
    fails = 0   # HARD: a broken render (rc=1)
    warns = 0   # SOFT: a real-but-non-blocking degeneracy (reported, rc unaffected)
    n_bad_total = 0
    for fr in frames:
        fi = fr.get("frame", "?")
        bs = fr.get("batches", [])
        # NaN/inf is the one unambiguous corruption signal (large clip coords are normal for
        # distant/near-plane geometry the GPU clips, so they are NOT failures).
        bad = sum(b.get("bad", 0) for b in bs)
        n_bad_total += bad
        if bad > 0:
            print(f"  FAIL f{fi}: {bad} NaN/inf verts"); fails += 1
        nverts = fr.get("nverts", 0)
        if nverts <= 0:
            print(f"  FAIL f{fi}: empty scene (nverts={nverts})"); fails += 1
        # Visibility: SOMETHING must land on-screen (w>0, |ndc|<=1). All-off-screen = broken camera/proj.
        onscr = sum(b.get("onscr", 0) for b in bs)
        if bs and onscr == 0:
            print(f"  FAIL f{fi}: NO vertex lands on-screen (broken projection/camera)"); fails += 1
        # Degenerate collapse: a many-vertex batch whose clip AABB is ~a point (all verts share one
        # matrix — the skinning-collapse signature). An ON-SCREEN collapse is a hard fail (it smears
        # / vanishes visibly); an OFF-SCREEN one is a warning (often a hidden/parked model, but still
        # worth a look — it's the same class as the Mario envelope bug for some other model).
        for i, b in enumerate(bs):
            c = b.get("clip")
            if c and b.get("vc", 0) > 60:
                ext = max(c[1]-c[0], c[3]-c[2], c[5]-c[4])
                if ext < 1e-3:
                    onscreen = b.get("onscr", 0) > 0
                    tag = "FAIL" if onscreen else "warn"
                    print(f"  {tag} f{fi} b{i}({b.get('k')}): {b['vc']} verts collapsed to a point"
                          f"{' (ON-SCREEN)' if onscreen else ' (off-screen)'}")
                    if onscreen: fails += 1
                    else: warns += 1
        # XFB: all-black or blown-white is almost always a broken render.
        xfb = fr.get("xfb")
        if xfb:
            br = xfb.get("bright", 0.0)
            if br < 1.0:
                print(f"  FAIL f{fi}: XFB near-black (bright={br:.2f}) — dead render?"); fails += 1
            elif br > 250.0:
                print(f"  FAIL f{fi}: XFB blown white (bright={br:.2f})"); fails += 1
    nf = len(frames)
    print(f"{path}: {nf} frames, {n_bad_total} NaN/inf total, {fails} FAIL, {warns} warn")
    return 1 if fails else 0

def _batch_key(b):
    return (b.get("k"), b.get("vc"))

def diff(pa, pb, ndc_tol=0.02, bright_tol=8.0):
    """Find divergences between two dumps (pure Dolphin oracle `pa` vs sms-boot native `pb`, OR
    two native A/B runs) frame-by-frame. Compares the CROSS-ENGINE comparable game state:
    projection/viewport, lights, ambient/material, and the frame geometry aggregate (on-screen
    count, NDC AABB, checksum). Per-batch clip-AABB drift is ALSO reported when both dumps share
    the same batch grouping (native A/B); it's skipped cross-engine (batch grouping differs)."""
    A, B = load_jsonl(pa), load_jsonl(pb)
    if not A or not B:
        print("FAIL: empty dump(s)"); return 1
    # Cross-engine (one dump is the geometry-only pure-Dolphin oracle: no `proj` field) → window
    # summary, NOT per-frame: the two engines number frames independently, so a shared index is
    # different game-time on each side. Per-frame is for native A/B (same engine, same frame).
    cross = not any("proj" in f for f in A) or not any("proj" in f for f in B)
    if cross:
        return _diff_summary(A, B, pa, pb)
    byf_b = {fr.get("frame"): fr for fr in B}
    div = 0; matched = 0
    def relmax(va, vb):
        return max((abs(x-y) / max(1.0, abs(x), abs(y)) for x, y in zip(va, vb)), default=0.0)
    for fa in A:
        fi = fa.get("frame")
        fb = byf_b.get(fi)
        if fb is None:
            continue
        matched += 1
        # Only compare a field present in BOTH dumps — the pure-Dolphin oracle dump is geometry-only
        # (no proj/lights/material form-matching), so those are skipped cross-engine, not flagged.
        # Projection + viewport (exact game state; native A/B only).
        if fa.get("projType") is not None and fb.get("projType") is not None and fa["projType"] != fb["projType"]:
            print(f"  f{fi}: projType {fa['projType']} -> {fb['projType']}"); div += 1
        for k in ("proj", "vp"):
            va, vb = fa.get(k), fb.get(k)
            if va and vb and relmax(va, vb) > ndc_tol:
                print(f"  f{fi}: {k} differs (rel Δ{relmax(va,vb):.3f})  {va} -> {vb}"); div += 1
        # Lighting (native A/B only).
        la, lb = fa.get("lights"), fb.get("lights")
        if la and lb and la.get("n") != lb.get("n"):
            print(f"  f{fi}: light count {la.get('n')} -> {lb.get('n')}"); div += 1
        for k in ("amb", "matc"):
            va, vb = fa.get(k), fb.get(k)
            if va and vb and any(abs(x-y) > 0.02 for x, y in zip(va, vb)):
                print(f"  f{fi}: {k} {va} -> {vb}"); div += 1
        # Frame geometry aggregate (the renderer-neutral cross-engine geometry signal).
        ga, gb = fa.get("geom"), fb.get("geom")
        if ga and gb:
            if gb.get("nan", 0) > ga.get("nan", 0):
                print(f"  f{fi}: NaN verts {ga.get('nan')} -> {gb.get('nan')}"); div += 1
            oa, ob = ga.get("onscr", 0), gb.get("onscr", 0)
            if abs(oa-ob) > max(16, 0.05*max(oa, ob, 1)):
                print(f"  f{fi}: on-screen verts {oa} -> {ob} (Δ{ob-oa:+d})"); div += 1
            na, nb = ga.get("ndc", []), gb.get("ndc", [])
            # NDC z range differs across engines (Vulkan [0,1] vs raw GX clip z) — compare X,Y only
            # cross-engine; compare Z too only for native A/B (both carry the `proj` field).
            ncmp = 6 if (fa.get("proj") and fb.get("proj")) else 4
            if na and nb and max((abs(na[i]-nb[i]) for i in range(min(ncmp, len(na), len(nb)))), default=0) > ndc_tol:
                d = max(abs(na[i]-nb[i]) for i in range(min(ncmp, len(na), len(nb))))
                print(f"  f{fi}: frame NDC {'XY' if ncmp==4 else 'XYZ'} AABB moved (max Δ{d:.3f})  {na[:ncmp]} -> {nb[:ncmp]}"); div += 1
        # XFB brightness (native dumps only; harmless if absent).
        ba, bb = fa.get("xfb", {}).get("bright"), fb.get("xfb", {}).get("bright")
        if ba is not None and bb is not None and abs(ba-bb) > bright_tol:
            print(f"  f{fi}: XFB bright {ba:.1f} -> {bb:.1f} (Δ{bb-ba:+.1f})"); div += 1
        # Per-batch clip-AABB drift — ONLY when batch grouping matches (native A/B, not cross-engine).
        Ba, Bb = fa.get("batches", []), fb.get("batches", [])
        same_grouping = len(Ba) == len(Bb) and all(x.get("k") == y.get("k") for x, y in zip(Ba, Bb))
        if same_grouping:
            for i, (xa, xb) in enumerate(zip(Ba, Bb)):
                if xa.get("bad", 0) != xb.get("bad", 0):
                    print(f"  f{fi} b{i}({xa.get('k')}): NaN/inf {xa.get('bad')} -> {xb.get('bad')}"); div += 1
                ca, cb = xa.get("clip", []), xb.get("clip", [])
                if ca and cb and relmax(ca, cb) > ndc_tol:
                    print(f"  f{fi} b{i}({xa.get('k')}): clip AABB moved (rel Δ{relmax(ca,cb):.3f})"); div += 1
    if matched == 0:
        # CROSS-ENGINE: the two engines number frames independently (pure Dolphin 0..N vs sms-boot's
        # present-frame window), so there are no shared indices. Both fastboot the same idle Delfino,
        # which is near-static frame-to-frame, so compare WINDOW SUMMARIES of the convention-robust
        # signals: total verts, on-screen count, and the on-screen screen-XY extent (width/height —
        # sign-independent, so Y-up/Y-down and Z-range differences don't matter). A gross divergence
        # (missing geometry, a smear blowing the screen extent, a collapse) shows here.
        return _diff_summary(A, B, pa, pb)
    print(f"diff {pa} vs {pb}: {matched} frames matched, {div} divergences (rel {ndc_tol}, bright {bright_tol})")
    return 1 if div else 0

def _median(xs):
    xs = sorted(xs); n = len(xs)
    return 0 if not n else (xs[n//2] if n % 2 else 0.5*(xs[n//2-1]+xs[n//2]))

def _mode(xs):
    """Most-common value (for projType, a discrete int)."""
    if not xs:
        return None
    counts = {}
    for x in xs:
        counts[x] = counts.get(x, 0) + 1
    return max(counts.items(), key=lambda kv: kv[1])[0]

def _summarize(frames):
    """Convention-robust window summary over the SETTLED window. GEOMETRY: median DISPLAYED BATCH
    count (the robust cross-engine geometry signal — independent of clip methodology), total verts,
    on-screen count, and on-screen screen-XY extent. LIGHTING (added for the title/file-select
    geometry+lighting parity work): median light count, per-channel median ambient + material colour,
    and modal projection type. Lighting fields are renderer-neutral game output (both engines load the
    SAME GX light/ambient/material state from the same guest RAM), so a median mismatch is a real
    divergence even though frame indices don't align cross-engine."""
    nb, nv, on, w, h = [], [], [], [], []
    ln = []; amb = [[], [], []]; matc = [[], [], [], []]; ptype = []
    for f in frames:
        g = f.get("geom", {})
        if g.get("onscr", 0) <= 0:   # skip blank/transition frames
            continue
        nb.append(f.get("nbatch", 0)); nv.append(f.get("nverts", 0)); on.append(g.get("onscr", 0))
        nd = g.get("ndc", [0,0,0,0,0,0])
        w.append(nd[1]-nd[0]); h.append(nd[3]-nd[2])
        L = f.get("lights")
        if L is not None and L.get("n") is not None:
            ln.append(L.get("n", 0))
        a = f.get("amb")
        if a:
            for i in range(min(3, len(a))): amb[i].append(a[i])
        m = f.get("matc")
        if m:
            for i in range(min(4, len(m))): matc[i].append(m[i])
        pt = f.get("projType")
        if pt is not None:
            ptype.append(pt)
    return {"frames": len(nv), "nbatch": _median(nb), "nverts": _median(nv), "onscr": _median(on),
            "xw": _median(w), "yh": _median(h),
            "lights_n": (_median(ln) if ln else None),
            "amb": [_median(c) for c in amb] if amb[0] else None,
            "matc": [_median(c) for c in matc] if matc[0] else None,
            "projType": _mode(ptype) if ptype else None}

def _diff_summary(A, B, pa, pb):
    sa, sb = _summarize(A), _summarize(B)
    print(f"cross-engine summary (no shared frame indices):")
    print(f"  {'metric':14s} {'oracle':>14s} {'native':>14s}   relΔ")
    div = 0
    # nbatch is the PRIMARY (robust) signal: displayed batch/material count is independent of clip
    # methodology. on-screen verts is SECONDARY (confounded — sms-boot emits unclipped, the GX
    # capture near-clips, so the in-[-1,1] count differs for near/edge geometry even at parity).
    rows = [("nbatch", 0.10, "batches*"), ("nverts", 0.15, "total verts"),
            ("onscr", 0.25, "on-screen v~"), ("xw", 0.05, "screen width"), ("yh", 0.05, "screen height")]
    for k, tol, label in rows:
        va, vb = sa[k], sb[k]
        rel = abs(va-vb) / max(1.0, abs(va), abs(vb))
        flag = "  <-- DIVERGE" if rel > tol else ""
        if rel > tol: div += 1
        print(f"  {label:14s} {va:14.1f} {vb:14.1f}   {rel:5.2f}{flag}")
    print(f"  (* batches = robust signal; ~ on-screen verts confounded by clip methodology)")

    # ── LIGHTING (geometry+lighting parity) ──────────────────────────────────────────────────────
    # Renderer-neutral game output: both engines load the SAME lights/ambient/material from guest RAM,
    # so a median mismatch over the settled window is a real divergence (sms-boot reading the wrong
    # light/ambient/material, or driving a pass without its lights). Absolute tolerances (these are
    # 0..1 colours / small integer counts, not large magnitudes where a relative tol makes sense).
    print(f"  -- lighting --")
    # Light count: integer; a difference >0.5 (i.e. ≥1 light) is a divergence.
    if sa["lights_n"] is not None and sb["lights_n"] is not None:
        va, vb = sa["lights_n"], sb["lights_n"]
        flag = "  <-- DIVERGE" if abs(va-vb) > 0.5 else ""
        if flag: div += 1
        print(f"  {'light count':14s} {va:14.1f} {vb:14.1f}   {abs(va-vb):5.1f}{flag}")
    # Ambient + material colour: per-channel max abs delta of the channel medians (tol 0.02 = ~5/255).
    for key, label in (("amb", "ambient"), ("matc", "material col")):
        va, vb = sa[key], sb[key]
        if va is None or vb is None:
            continue
        d = max((abs(x-y) for x, y in zip(va, vb)), default=0.0)
        flag = "  <-- DIVERGE" if d > 0.02 else ""
        if flag: div += 1
        sva = "[" + ",".join(f"{x:.2f}" for x in va) + "]"
        svb = "[" + ",".join(f"{x:.2f}" for x in vb) + "]"
        print(f"  {label:14s} {sva:>14s} {svb:>14s}   {d:5.3f}{flag}")
    # Projection type (0 persp / 1 ortho): discrete; any mode mismatch is a divergence.
    if sa["projType"] is not None and sb["projType"] is not None:
        va, vb = sa["projType"], sb["projType"]
        flag = "  <-- DIVERGE" if va != vb else ""
        if flag: div += 1
        print(f"  {'projType':14s} {va:14d} {vb:14d}   {'-':>5s}{flag}")

    if sa["frames"] == 0 or sb["frames"] == 0:
        print(f"  WARN: a dump has 0 non-blank frames (oracle {sa['frames']}, native {sb['frames']})")
    print(f"summary: {div} metric(s) diverge beyond tolerance")
    return 1 if div else 0

# ---------------------------------------------------------------------------------------
# PIXEL track (vs Dolphin-GX oracle)
# ---------------------------------------------------------------------------------------
def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    mode = sys.argv[1]
    if mode == "check":  return check(sys.argv[2])
    if mode == "diff":   return diff(sys.argv[2], sys.argv[3])
    print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main())
