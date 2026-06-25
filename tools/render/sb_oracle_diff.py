#!/usr/bin/env python3
# sb_oracle_diff.py — the rung-6 cross-engine renderer oracle diff.
#
# Compares a frame rendered by the host-native sms-boot SDL3-GPU renderer (sb::gxsdl)
# against a Dolphin-GX reference render of the SAME Delfino-plaza scene. This is NOT a
# frame-exact oracle: sms-boot is a host-native engine (LP64, host-malloc arena) while
# the oracle runs under Dolphin's JIT on GC guest RAM, so the two share no state and
# cannot be byte-synced. It is a DETERMINISTIC-BOOT, settled-scene comparison: both boot
# the same disc to an idle plaza and we diff per-region. A genuine renderer regression
# moves the number; an exact 0 is impossible and is itself a red flag (means a stale or
# self-compared frame).
#
# HARD GUARD (TOOLING-FIRST rule): the tool REFUSES a degenerate frame — all-black,
# washed-white (the scene-name fade card), or near-uniform low-variance — so it can never
# emit a meaningless number against a non-scene. Exit 3 on a refused input.
#
# Usage: sb_oracle_diff.py <sms_boot.ppm> <dolphin_gx.ppm> [--heat out.png]
import sys, struct

def load_ppm(p):
    with open(p, 'rb') as f:
        d = f.read()
    if d[:2] != b'P6':
        sys.exit(f"{p}: not a P6 PPM")
    i = 2; vals = []
    while len(vals) < 3:
        while i < len(d) and d[i] in b' \t\n\r': i += 1
        if d[i:i+1] == b'#':
            while d[i] not in b'\n': i += 1
            continue
        j = i
        while d[j] not in b' \t\n\r': j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    w, h, mx = vals
    return w, h, d[i:i+w*h*3]

def lum(px, o):
    return (px[o]*299 + px[o+1]*587 + px[o+2]*114) // 1000

def stats(w, h, px):
    n = w*h
    s = 0; s2 = 0
    for o in range(0, n*3, 3):
        l = lum(px, o); s += l; s2 += l*l
    mean = s/n
    var = s2/n - mean*mean
    return mean, var**0.5

def crop_letterbox(w, h, px):
    # drop fully-dark rows at top/bottom (NDC letterbox bars) so two different active
    # heights (480 with bars vs 448 without) align on content.
    def row_dark(y):
        base = y*w*3
        tot = 0
        for x in range(0, w*3, 3):
            tot += lum(px, base+x)
        return tot/w < 10
    top = 0
    while top < h and row_dark(top): top += 1
    bot = h-1
    while bot > top and row_dark(bot): bot -= 1
    if bot <= top:
        return w, h, px  # all dark — let the degenerate guard handle it
    nh = bot-top+1
    return w, nh, px[top*w*3:(bot+1)*w*3]

def resize_nn(w, h, px, nw, nh):
    out = bytearray(nw*nh*3)
    for y in range(nh):
        sy = y*h//nh
        for x in range(nw):
            sx = x*w//nw
            so = (sy*w+sx)*3; do = (y*nw+x)*3
            out[do] = px[so]; out[do+1] = px[so+1]; out[do+2] = px[so+2]
    return out

def guard(label, w, h, px):
    if w < 64 or h < 64:
        sys.exit(f"REFUSE [{label}]: degenerate size {w}x{h} (exit 3)" or 3)
    mean, sd = stats(w, h, px)
    reason = None
    if mean < 6:   reason = f"all-black (mean lum {mean:.1f})"
    elif mean > 244: reason = f"washed-white / fade card (mean lum {mean:.1f})"
    elif sd < 12:  reason = f"near-uniform (lum stddev {sd:.1f}) — not a rendered scene"
    if reason:
        print(f"REFUSE [{label}]: {reason}", file=sys.stderr)
        sys.exit(3)
    return mean, sd

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: sb_oracle_diff.py <sms_boot.ppm> <dolphin_gx.ppm> [--heat out.png]")
    a_path, b_path = sys.argv[1], sys.argv[2]
    heat = None
    if '--heat' in sys.argv:
        heat = sys.argv[sys.argv.index('--heat')+1]
    aw, ah, apx = load_ppm(a_path)
    bw, bh, bpx = load_ppm(b_path)
    aw, ah, apx = crop_letterbox(aw, ah, apx)
    bw, bh, bpx = crop_letterbox(bw, bh, bpx)
    am, asd = guard(f"A {a_path}", aw, ah, apx)
    bm, bsd = guard(f"B {b_path}", bw, bh, bpx)
    # canonical compare grid
    NW, NH = 256, 192
    a = resize_nn(aw, ah, apx, NW, NH)
    b = resize_nn(bw, bh, bpx, NW, NH)
    n = NW*NH*3
    tot = 0
    G = 4
    gr = [[0]*G for _ in range(G)]; cnt = [[0]*G for _ in range(G)]
    heatpx = bytearray(NW*NH*3) if heat else None
    for y in range(NH):
        gy = min(G-1, y*G//NH)
        for x in range(NW):
            gx = min(G-1, x*G//NW)
            o = (y*NW+x)*3
            d = abs(a[o]-b[o])+abs(a[o+1]-b[o+1])+abs(a[o+2]-b[o+2])
            tot += d; gr[gy][gx] += d; cnt[gy][gx] += 3
            if heatpx is not None:
                v = min(255, d)
                heatpx[o] = v; heatpx[o+1] = 0; heatpx[o+2] = 255-v
    overall = tot/n
    print(f"A (sms-boot) content {aw}x{ah}  lum mean={am:.1f} sd={asd:.1f}")
    print(f"B (dolphin)  content {bw}x{bh}  lum mean={bm:.1f} sd={bsd:.1f}")
    print(f"overall mean abs delta (per channel, on {NW}x{NH} grid): {overall:.2f}")
    print("4x4 region grid:")
    for gy in range(G):
        print("  " + " ".join("%6.1f" % (gr[gy][gx]/cnt[gy][gx]) for gx in range(G)))
    if heat:
        from PIL import Image
        Image.frombytes('RGB', (NW, NH), bytes(heatpx)).save(heat)
        print(f"heatmap -> {heat}")

if __name__ == '__main__':
    main()
