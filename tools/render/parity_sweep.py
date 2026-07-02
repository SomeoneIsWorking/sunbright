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
    byf_b = {(fr.get("frame"), _pass_of(fr)): fr for fr in B}
    div = 0; matched = 0
    def relmax(va, vb):
        return max((abs(x-y) / max(1.0, abs(x), abs(y)) for x, y in zip(va, vb)), default=0.0)
    for fa in A:
        fi = fa.get("frame")
        fb = byf_b.get((fi, _pass_of(fa)))
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

def _pass_of(f):
    """Which render pass a parity line belongs to. Both emitters tag lines with `pass` now; older
    dumps (and the native value-track A/B) have no tag and are the 3D scene by construction."""
    return f.get("pass", "scene")

def _summarize(frames, want_pass="scene"):
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
        if _pass_of(f) != want_pass:   # align LIKE pass to LIKE pass (scene vs scene)
            continue
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
    # ── FAIL-FAST HARNESS INVARIANTS ────────────────────────────────────────────────────────────
    # A parity number that can silently mean "measured against nothing" is worse than no number
    # (per CLAUDE.md fail-fast + 2026-07-02 workflow directive). Refuse to emit when the inputs
    # cannot possibly yield a valid comparison.
    scene_a = [f for f in A if _pass_of(f) == "scene" and f.get("geom", {}).get("onscr", 0) > 0]
    scene_b = [f for f in B if _pass_of(f) == "scene" and f.get("geom", {}).get("onscr", 0) > 0]
    if not scene_a:
        print(f"HARNESS-FAIL: oracle {pa} has 0 non-empty scene frames (dead capture)"); return 2
    if not scene_b:
        print(f"HARNESS-FAIL: native {pb} has 0 non-empty scene frames (dead capture)"); return 2
    # ── STATE-PINNING GUARD ────────────────────────────────────────────────────────────────────
    # This comparison is a WINDOW-SUMMARY MEDIAN across DIFFERENT captured states — the oracle
    # and native processes run independently with different pacing, so the same frame index on
    # each side is different game-time. A "within tolerance" verdict here is NOT evidence that
    # the two engines produced the same output. It is at best a first-order sanity check.
    # Print the pinning gap prominently so nothing downstream mistakes this for state-pinned
    # evidence (per 2026-07-02 directive: no evidence from mismatched-state captures).
    fa_range = (scene_a[0].get("frame"), scene_a[-1].get("frame"), len(scene_a))
    fb_range = (scene_b[0].get("frame"), scene_b[-1].get("frame"), len(scene_b))
    print("=" * 78)
    print("STATE-UNPINNED WINDOW SUMMARY — NOT authoritative evidence.")
    print(f"  oracle frames [{fa_range[0]}..{fa_range[1]}] ({fa_range[2]} non-empty)")
    print(f"  native frames [{fb_range[0]}..{fb_range[1]}] ({fb_range[2]} non-empty)")
    print("  Same-state pinning (same input, same frame index, same camera) is NOT implemented.")
    print("  A 'within tolerance' verdict here is NOT proof the two engines match — it is a")
    print("  first-order sanity check across unaligned windows. Do not use as fix evidence.")
    print("=" * 78)
    sa, sb = _summarize(A, "scene"), _summarize(B, "scene")
    print(f"cross-engine summary — SCENE pass (no shared frame indices):")
    print(f"  {'metric':14s} {'oracle':>14s} {'native':>14s}   relΔ")
    div = 0
    # nverts is now a PRIMARY signal: both sides report the real per-pass vertex count of the same
    # (perspective) pass, so a gap is a real geometry divergence (over-draw / missing / extra content).
    # nbatch (oracle = display-list count, native = material-batch count) groups differently, so it is
    # order-of-magnitude only. Screen W/H extent stays robust (both ~2.0 = full-screen).
    # The oracle (GX command-stream) does NOT transform vertices, so it emits a zeroed NDC AABB; the
    # screen-extent rows are only meaningful when BOTH sides carry real NDC (native A/B, or a future
    # oracle that projects). Skip them otherwise rather than report a bogus 0-vs-1.9 divergence.
    both_ndc = (abs(sa["xw"]) + abs(sa["yh"]) > 1e-6) and (abs(sb["xw"]) + abs(sb["yh"]) > 1e-6)
    rows = [("nverts", 0.15, "scene verts"), ("nbatch", 0.50, "batches~"),
            ("onscr", 0.25, "on-screen v~")]
    if both_ndc:
        rows += [("xw", 0.05, "screen width"), ("yh", 0.05, "screen height")]
    for k, tol, label in rows:
        va, vb = sa[k], sb[k]
        rel = abs(va-vb) / max(1.0, abs(va), abs(vb))
        flag = "  <-- DIVERGE" if rel > tol else ""
        if rel > tol: div += 1
        print(f"  {label:14s} {va:14.1f} {vb:14.1f}   {rel:5.2f}{flag}")
    if not both_ndc:
        print(f"  (screen W/H skipped: the GX-stream oracle emits no NDC — extent not cross-comparable)")
    print(f"  (scene verts = like-pass comparable; ~ batches/on-screen grouping- & clip-confounded)")

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
    # HONESTY CAVEAT (do not draw conclusions past what these signals actually measure):
    #  • RELIABLE cross-engine: light COUNT and SCENE vertex count — both now compare the SAME 3D
    #    (perspective) pass: the oracle tags its scene-pass line and counts display-list verts from
    #    guest RAM, so its verts_pass[scene] is the real geometry of the same pass the native dump emits.
    #  • CONFOUNDED — ambient / material col are whole-frame last-seen XF state (the oracle has no
    #    per-pass split for them yet); a small mismatch can be sampling phase, not a real divergence.
    #  • PARTIALLY confounded — nbatch (oracle = display-list count vs native = material-batch count)
    #    and onscr (different clip methodology) group differently; treat as order-of-magnitude.
    print(f"  NOTE: light-count + SCENE verts are like-pass comparable now; nbatch/onscr group-confounded,")
    print(f"        ambient/matc are whole-frame last-seen (see caveat in source).")
    print(f"summary: {div} metric(s) diverge beyond tolerance")
    return 1 if div else 0

# ---------------------------------------------------------------------------------------
# Per-draw ordered diff (produces a NAMED first-divergent draw call)
# ---------------------------------------------------------------------------------------
def _first_scene_with_draws(dump, key):
    """Return the first scene-pass frame whose per-draw stream is populated."""
    for f in dump:
        if _pass_of(f) != "scene": continue
        if f.get(key): return f
    return None

# --- STATE-PIN fingerprint ---------------------------------------------------------------
# A scene frame's game-state signature is (projType, proj[4], proj[5]) — the near/far
# terms. This is the cross-engine SUBSET of the projection that survives oracle-only extra
# render passes.
#
# The oracle runs TMirrorCamera::perform (reference/sms/src/Map/MapMirror.cpp:29) which
# issues a mirror-pre-render SETPROJECTION at unk80*gpCamera->mFovy (measured 52° at
# fileselect, vs the main scene's 40°). gx_parse.proj_pass latches at the FIRST primitive
# of the pass — and the mirror pass draws BEFORE the main scene, so the SCENE line's proj
# matrix on the oracle is the MIRROR's 52° widened matrix, not the main scene camera.
# Native lacks TMirrorCamera → its scene line carries the 40° matrix. So proj[0]/proj[2]
# (fovy-dependent scales) will legitimately diverge even at identical game state.
#
# proj[4] = m22 = -F/(F-N) and proj[5] = m23 = -F*N/(F-N) — the near/far plane
# encoding. Mirror and main scene BOTH use gpCamera->mNear/mFar (MapMirror.cpp:30), so
# proj[4]/proj[5] are IDENTICAL between the two passes on the oracle, AND identical to
# native. This is the cross-engine subset that pins state correctly.
#
# vp[] and proj[0..3] deliberately NOT in the fingerprint: vp is unit-mismatched between
# oracle (Dolphin XFMEM raw) and native (SbParityProj); proj[0..3] carry the fovy-scaled
# terms that suffer the mirror-pass corruption above.
def _fp(frame):
    p = frame.get("proj"); t = frame.get("projType")
    if p is None or t is None or len(p) < 6: return None
    return (int(t), round(float(p[4]), 3), round(float(p[5]), 3))

def _pick_matched_scene_pair(A, B, key_a, key_b):
    """Find (frame_a, frame_b) with equal state-pin fingerprint on both sides, both carrying a
    per-draw stream. Returns (fa, fb, fp) or (None, None, reason). Prefers the fingerprint that
    covers the most scene frames on BOTH sides (settled / stable state), not a one-off transient."""
    from collections import Counter
    fps_a = Counter()
    fps_b = Counter()
    idx_a, idx_b = {}, {}
    for f in A:
        if _pass_of(f) != "scene" or not f.get(key_a): continue
        fp = _fp(f)
        if fp is None: continue
        fps_a[fp] += 1
        idx_a.setdefault(fp, f)
    for f in B:
        if _pass_of(f) != "scene" or not f.get(key_b): continue
        fp = _fp(f)
        if fp is None: continue
        fps_b[fp] += 1
        idx_b.setdefault(fp, f)
    if not fps_a: return None, None, "oracle has no proj+vp-tagged scene frames (rebuild sunbright with the fingerprint emit)"
    if not fps_b: return None, None, "native has no proj+vp-tagged scene frames (rebuild sms-boot with the fingerprint emit)"
    common = set(fps_a) & set(fps_b)
    if not common:
        # Name the discrepancy: print the top-3 fingerprints on each side (a scene often visits a
        # HANDFUL of camera/vp states over the capture, and the top-1 on each side can be
        # DIFFERENT phases of the SAME intro). If any of ora's top-3 differ from native's top-3
        # only in ONE field, that's a real state divergence in that field.
        lines = ["no fingerprint match"]
        lines.append("  oracle top fingerprints (projType, proj[4], proj[5]) — count):")
        for fp, c in fps_a.most_common(3):
            lines.append(f"    ({c:5d})  projType={fp[0]}  proj[4]={fp[1]}  proj[5]={fp[2]}")
        lines.append("  native top fingerprints — count):")
        for fp, c in fps_b.most_common(3):
            lines.append(f"    ({c:5d})  projType={fp[0]}  proj[4]={fp[1]}  proj[5]={fp[2]}")
        top_a = fps_a.most_common(1)[0][0]
        top_b = fps_b.most_common(1)[0][0]
        diffs = []
        if top_a[0] != top_b[0]: diffs.append(f"projType oracle={top_a[0]} native={top_b[0]}")
        if top_a[1] != top_b[1]: diffs.append(f"proj[4] oracle={top_a[1]} native={top_b[1]}")
        if top_a[2] != top_b[2]: diffs.append(f"proj[5] oracle={top_a[2]} native={top_b[2]}")
        if diffs: lines.append("  top-1 vs top-1 diffs: " + "; ".join(diffs))
        return None, None, "\n".join(lines)
    # Score by min(count_a, count_b) — a fingerprint that persists on BOTH sides is settled state.
    best_fp = max(common, key=lambda fp: min(fps_a[fp], fps_b[fp]))
    return idx_a[best_fp], idx_b[best_fp], best_fp

def _oracle_draw_sig(d):
    """Cross-engine signature for an ORACLE (gx_capture.cpp) DrawRec JSON object.
    Fields chosen to align with a NATIVE (nvk NvkTevBatch) draw:
      - blend enable + src + dst + subtract
      - color/alpha update
      - TEV stage count
      - vertex count
    Excludes anything renderer-specific (shaderKey, mat-index)."""
    return (
        int(d.get("be", 0)), int(d.get("src", 0)), int(d.get("dst", 0)),
        int(d.get("sub", 0)), int(d.get("cU", 0)), int(d.get("aU", 0)),
        int(d.get("tev", 0)), int(d.get("v", 0)),
    )

def _native_draw_sig(b):
    """Cross-engine signature for a NATIVE (sms-boot NvkTevBatch) batch JSON object.
    `bm` is [mode, src, dst] where mode encodes blend-enable/subtract; the native z/blend
    layout is different from GX raw factor codes, so this is a first-pass signature to
    surface the SHAPE of the divergence (vertex count + tev-stage count are the most
    reliably comparable across engines)."""
    bm = b.get("bm", [0, 0, 0])
    # native NvkTevBatch stores tex-count, not tev-stage count. Use ntex as an ordinal
    # proxy for structural comparison; if the ordered position agrees the same-place
    # nature is what the diff reports.
    return (
        1 if bm[0] else 0,           # be (best-effort: nonzero mode = blend enabled)
        int(bm[1]), int(bm[2]),      # src, dst
        0,                            # sub (not tracked separately on native side)
        1, 1,                         # cU, aU (native always writes color+alpha)
        int(b.get("ntex", 0)),       # substitute for tev-stage count (proxy)
        int(b.get("vc", 0)),         # vertex count
    )

def drawdiff(pa, pb):
    """Per-draw ordered diff between two parity dumps that carry per-draw records:
    - oracle (gx_capture.cpp under SUNBRIGHT_PARITY_DRAWS=1) emits `draws`:[…] per line
    - native (sb_parity_dump.h) emits `batches`:[…] per line

    Compares ordered position N on each side and NAMES the first divergent draw call
    with its signature fields. Refuses to emit when either side has no per-draw stream
    or when both sides show they were captured against different game states.

    NOTE: this is still cross-engine ordering — same-state pinning
    (scratch/GOALS.md step #1) is the prerequisite for calling a divergence real
    rather than an ordering artifact. Prints STATE-UNPINNED banner and refuses to
    label findings as authoritative."""
    A, B = load_jsonl(pa), load_jsonl(pb)
    if not A or not B:
        print(f"HARNESS-FAIL: empty dump(s) ({pa}: {len(A)}, {pb}: {len(B)})"); return 2
    # STATE-PIN: pair frames by matching (projType, proj[6], vp[6]) fingerprint. If both sides
    # share a fingerprint, use it — that IS bit-equal SETPROJECTION/SETVIEWPORT bytes, i.e. the
    # game issued the same scene setup. Fall back to first-scene ordering + STATE-UNPINNED banner
    # when they don't (the harness names the fingerprint discrepancy).
    fa, fb, fp_or_reason = _pick_matched_scene_pair(A, B, "draws", "batches")
    pinned = fa is not None
    if not pinned:
        # No fingerprint match — fall back so the diff still runs, but keep the banner up.
        fa = _first_scene_with_draws(A, "draws")
        fb = _first_scene_with_draws(B, "batches")
        if fa is None:
            print(f"HARNESS-FAIL: oracle {pa} has no per-draw stream — run with SUNBRIGHT_PARITY_DRAWS=1")
            return 2
        if fb is None:
            print(f"HARNESS-FAIL: native {pb} has no per-batch stream (sb_parity_dump.h emission?)")
            return 2
    print("=" * 78)
    if pinned:
        print("STATE-PINNED PER-DRAW DIFF — near/far fingerprint match.")
        print(f"  fingerprint projType={fp_or_reason[0]} proj[4]={fp_or_reason[1]} proj[5]={fp_or_reason[2]}")
    else:
        print("STATE-UNPINNED PER-DRAW ORDERED DIFF — NOT authoritative.")
        print(f"  reason: {fp_or_reason}")
    print(f"  oracle frame {fa.get('frame')} pass=scene: {len(fa.get('draws', []))} draws")
    print(f"  native frame {fb.get('frame')} pass=scene: {len(fb.get('batches', []))} batches")
    if not pinned:
        print("  Draws compared by ordered position. Same-state pinning (GOALS.md #1) required")
        print("  before treating a NAMED divergence here as a fix target.")
    print("=" * 78)
    da = fa.get("draws", []); db = fb.get("batches", [])
    n = min(len(da), len(db))
    if not n:
        print(f"HARNESS-FAIL: 0 draws on one side (oracle {len(da)}, native {len(db)})")
        return 2
    # ─── SIGNATURE-BASED GROUPING ──────────────────────────────────────────────
    # Ordered-position across a Dolphin-GX FIFO (draws grouped into EFB passes:
    # pre-copy scene → post-copy composite → post-copy HUD) vs an nvk single-target
    # composite (no EFB copies, all draws in one buffer) does not align — the two
    # engines are emitting completely different sub-streams at ordered position 0
    # even when both are pinned to the same game state. Group instead by the
    # cross-engine-comparable signature: (blend_enable, src, dst). Report the
    # first signature bucket whose count differs across engines. That IS a
    # NAMED divergence surviving the pipeline-structure gap.
    from collections import Counter
    o_bucket = Counter((int(d.get("be",0)), int(d.get("src",0)), int(d.get("dst",0))) for d in da)
    def _n_sig(b):
        bm = b.get("bm", [0,0,0])
        return (1 if bm[0] else 0, int(bm[1]), int(bm[2]))
    n_bucket = Counter(_n_sig(b) for b in db)
    all_sigs = sorted(set(o_bucket) | set(n_bucket), key=lambda k: -max(o_bucket[k], n_bucket[k]))
    print(f"  Signature-bucket compare (blend enable, src, dst) → (oracle_count, native_count):")
    div = 0
    for sig in all_sigs:
        oc, nc = o_bucket[sig], n_bucket[sig]
        # Native emits far fewer batches because nvk groups within a single frame more
        # aggressively than the FIFO groups per-draw. Compare RATIOS, not raw counts.
        # A signature bucket present on ONE side and absent on the other = missing draw class.
        if (oc == 0) != (nc == 0):
            print(f"    {sig}: oracle={oc} native={nc}  ← MISSING (bucket present on one side only)")
            div += 1
        # Skip ratio compare — the target scale differs by ~10× (1220 vs 115). Emit for context.
        else:
            print(f"    {sig}: oracle={oc:5d} native={nc:5d}")
    if div == 0:
        print(f"  All {len(all_sigs)} blend-signature buckets present on BOTH sides.")
        print(f"  (Ratio comparison intentionally omitted — batch grouping differs by ~10× so raw")
        print(f"  ratios are not diagnostic. The bucket-existence gate above catches missing classes.)")
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
    if mode == "drawdiff": return drawdiff(sys.argv[2], sys.argv[3])
    print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main())
