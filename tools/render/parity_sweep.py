#!/usr/bin/env python3
"""parity_sweep.py — Dolphin-GX vs PC-native parity sweep (geometry / lighting / XFB).

TWO TRACKS, by design (see sb_parity_dump.h):
  • VALUE track: the native engine's per-frame geometry+lighting+XFB-grid state, dumped as
    JSONL by SB_PARITY_DUMP. Verified by `check` (invariants — the oracle is the SPEC +
    self-consistency, since Dolphin's CPU-side intermediate state is async-lagged and is NOT
    a valid value oracle) and `diff` (A/B between two native runs at matched frame indices —
    regression detection across a renderer change).
  • PIXEL track: the FINAL frame is the only trustworthy reference vs Dolphin-GX. `image`
    does a per-region pixel comparison of two frames (PPM/PNG): the native render and a
    Dolphin-GX oracle render of the SAME state. Refuses an all-black/empty frame so it can
    never report a meaningless number against a dead oracle.

Usage:
  parity_sweep.py check  dump.jsonl                 # invariants on one native run
  parity_sweep.py diff   a.jsonl b.jsonl            # A/B between two native runs
  parity_sweep.py image  native.ppm oracle.ppm      # per-region pixel diff vs Dolphin-GX
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
    """A/B two native dumps frame-by-frame; report per-batch NDC-AABB / checksum / lighting /
    XFB divergence above thresholds. Use to confirm a change is inert where it should be, or to
    localise where it moved geometry."""
    A, B = load_jsonl(pa), load_jsonl(pb)
    if not A or not B:
        print("FAIL: empty dump(s)"); return 1
    byf_b = {fr.get("frame"): fr for fr in B}
    div = 0
    for fa in A:
        fi = fa.get("frame")
        fb = byf_b.get(fi)
        if fb is None:
            continue
        # Lighting.
        if fa.get("lights", {}).get("n") != fb.get("lights", {}).get("n"):
            print(f"  f{fi}: light count {fa['lights']['n']} -> {fb['lights']['n']}"); div += 1
        for k in ("amb", "matc"):
            va, vb = fa.get(k, []), fb.get(k, [])
            if any(abs(x-y) > 0.02 for x, y in zip(va, vb)):
                print(f"  f{fi}: {k} {va} -> {vb}"); div += 1
        # XFB brightness.
        ba, bb = fa.get("xfb", {}).get("bright"), fb.get("xfb", {}).get("bright")
        if ba is not None and bb is not None and abs(ba-bb) > bright_tol:
            print(f"  f{fi}: XFB bright {ba:.1f} -> {bb:.1f} (Δ{bb-ba:+.1f})"); div += 1
        # Per-batch geometry (match by index): on-screen count, NaN/inf, and clip-AABB drift.
        Ba, Bb = fa.get("batches", []), fb.get("batches", [])
        for i, (xa, xb) in enumerate(zip(Ba, Bb)):
            if xa.get("bad", 0) != xb.get("bad", 0):
                print(f"  f{fi} b{i}({xa.get('k')}): NaN/inf {xa.get('bad')} -> {xb.get('bad')}"); div += 1
            if abs(xa.get("onscr", 0) - xb.get("onscr", 0)) > max(4, 0.02*xa.get("vc", 0)):
                print(f"  f{fi} b{i}({xa.get('k')}): on-screen {xa.get('onscr')} -> {xb.get('onscr')}"); div += 1
            ca, cb = xa.get("clip", []), xb.get("clip", [])
            # relative drift so far geometry (large clip) isn't over-flagged.
            if ca and cb:
                worst = max((abs(x-y) / max(1.0, abs(x), abs(y)) for x, y in zip(ca, cb)), default=0)
                if worst > ndc_tol:
                    print(f"  f{fi} b{i}({xa.get('k')}): clip AABB moved (rel Δ{worst:.3f})"); div += 1
    print(f"diff {pa} vs {pb}: {div} divergences above tol (rel {ndc_tol}, bright {bright_tol})")
    return 1 if div else 0

# ---------------------------------------------------------------------------------------
# PIXEL track (vs Dolphin-GX oracle)
# ---------------------------------------------------------------------------------------
def _read_ppm(path):
    if path.lower().endswith(".png"):
        from PIL import Image
        im = Image.open(path).convert("RGB")
        return im.width, im.height, list(im.tobytes())
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", f"{path} is not a binary PPM"
    # parse header: P6 W H MAXVAL\n<data>
    idx, tok = 2, []
    while len(tok) < 3:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        s = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        tok.append(int(data[s:idx]))
    idx += 1  # single whitespace after maxval
    w, h, _mx = tok
    return w, h, list(data[idx:idx + w*h*3])

REGIONS = {  # named 1/9 grid + meaningful bands for SMS framing (sky top, character center, HUD)
    "sky":       (0.0, 0.0, 1.0, 0.33),
    "mid":       (0.0, 0.33, 1.0, 0.66),
    "floor":     (0.0, 0.66, 1.0, 1.0),
    "center":    (0.33, 0.33, 0.66, 0.80),   # where the player usually is
    "hud_top":   (0.0, 0.0, 1.0, 0.12),
    "hud_bot":   (0.0, 0.88, 1.0, 1.0),
}

def _region_delta(w, h, A, B, box):
    x0, y0, x1, y1 = int(box[0]*w), int(box[1]*h), int(box[2]*w), int(box[3]*h)
    s = 0.0; n = 0
    for y in range(y0, y1):
        row = (y*w)*3
        for x in range(x0, x1):
            p = row + x*3
            s += abs(A[p]-B[p]) + abs(A[p+1]-B[p+1]) + abs(A[p+2]-B[p+2])
            n += 3
    return (s/n) if n else 0.0

def image(pa, pb):
    """Per-region mean |Δ| between native (pa) and Dolphin-GX oracle (pb). Refuses a dead frame."""
    wa, ha, A = _read_ppm(pa)
    wb, hb, B = _read_ppm(pb)
    if (wa, ha) != (wb, hb):
        print(f"FAIL: size mismatch {wa}x{ha} vs {wb}x{hb}"); return 3
    if max(A) < 4 or max(B) < 4:
        print(f"FAIL: a frame is all-black (dead oracle?) maxA={max(A)} maxB={max(B)}"); return 3
    overall = _region_delta(wa, ha, A, B, (0, 0, 1, 1))
    print(f"image {pa} vs {pb} ({wa}x{ha}): overall mean |Δ| = {overall:.2f} / 255")
    for name, box in REGIONS.items():
        d = _region_delta(wa, ha, A, B, box)
        bar = "#" * int(min(d, 60))
        print(f"  {name:10s} {d:6.2f}  {bar}")
    return 0

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    mode = sys.argv[1]
    if mode == "check":  return check(sys.argv[2])
    if mode == "diff":   return diff(sys.argv[2], sys.argv[3])
    if mode == "image":  return image(sys.argv[2], sys.argv[3])
    print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main())
