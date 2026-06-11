#!/usr/bin/env python3
"""PC-native jingle pipeline: nintendo.szs → Yaz0 → RARC → mSound.aaf → WSYS wave table →
split + AFC-decode waves from w1stLoad_0.aw at their real sample rates. Zero emulation.

usage: jingle.py <nintendo.szs> <banks_dir> <outdir>
"""
import os, struct, sys, wave

def be32(d, o): return struct.unpack_from(">I", d, o)[0]
def be16(d, o): return struct.unpack_from(">H", d, o)[0]

def yaz0(d):
    assert d[:4] == b"Yaz0"
    size = be32(d, 4)
    out = bytearray()
    i = 16
    code = 0
    bits = 0
    while len(out) < size:
        if bits == 0:
            code = d[i]; i += 1; bits = 8
        if code & 0x80:
            out.append(d[i]); i += 1
        else:
            b1, b2 = d[i], d[i + 1]; i += 2
            dist = ((b1 & 0xF) << 8) | b2
            src = len(out) - dist - 1
            n = (b1 >> 4) + 2
            if n == 2:
                n = d[i] + 0x12; i += 1
            for _ in range(n):
                out.append(out[src]); src += 1
        code <<= 1; bits -= 1
    return bytes(out)

def rarc_files(d):
    assert d[:4] == b"RARC"
    data_off = be32(d, 0xC) + 0x20
    node_count = be32(d, 0x20)
    node_off = be32(d, 0x24) + 0x20
    ent_off = be32(d, 0x2C) + 0x20
    str_off = be32(d, 0x34) + 0x20
    files = {}
    for ni in range(node_count):
        n = node_off + ni * 0x10
        first, count = be32(d, n + 0xC) & 0xFFFF, be16(d, n + 0xA)
        count = be16(d, n + 0xA)
        first = be32(d, n + 0xC)
        for fi in range(count):
            e = ent_off + (first + fi) * 0x14
            fid = be16(d, e)
            name_o = be32(d, e + 4) & 0xFFFFFF
            typ = (be32(d, e + 4) >> 24) & 0xFF
            off = be32(d, e + 8)
            size = be32(d, e + 0xC)
            name = d[str_off + name_o:d.index(b"\0", str_off + name_o)].decode()
            if fid != 0xFFFF and not (typ & 0x02):  # file, not dir
                files[name] = d[data_off + off:data_off + off + size]
    return files

COEF = [
    (0, 0), (2048, 0), (0, 2048), (1024, 1024),
    (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560),
    (4200, -2248), (4800, -2300), (5120, -3072), (2048, -2048),
    (1024, -1024), (-1024, 1024), (-1024, 0), (-2048, 0),
]

def afc_decode(data, hq=True):
    out = []
    yn1 = yn2 = 0
    bs = 9 if hq else 5
    for b in range(len(data) // bs):
        blk = data[b * bs:b * bs + bs]
        delta = 1 << (blk[0] >> 4)
        c0, c1 = COEF[blk[0] & 0xF]
        nibs = []
        if hq:
            for i in range(16):
                nib = (blk[1 + i // 2] >> (4 if i % 2 == 0 else 0)) & 0xF
                nibs.append((nib - 16 if nib >= 8 else nib) << 11)
        else:
            for i in range(16):
                nib = (blk[1 + i // 4] >> (6 - 2 * (i % 4))) & 3
                nibs.append((nib - 4 if nib >= 2 else nib) << 13)
        for nib in nibs:
            s = (delta * nib + yn1 * c0 + yn2 * c1) >> 11
            s = max(-32768, min(32767, s))
            yn2, yn1 = yn1, s
            out.append(s)
    return out

def parse_aaf_wsys(d):
    """AAF: sequence of (chunk_id, then id-specific offset/size lists). Returns WSYS blobs."""
    out = []
    o = 0
    while o + 4 <= len(d):
        cid = be32(d, o); o += 4
        if cid == 0:
            break
        if cid in (1, 4, 5, 6, 7):          # single: offset, size, flag
            o += 12
        elif cid in (2, 3):                  # multi until 0 terminator: offset,size,flag
            while True:
                off = be32(d, o)
                if off == 0:
                    o += 4
                    break
                size = be32(d, o + 4)
                o += 12
                if cid == 3:
                    out.append(d[off:off + size])
        else:
            raise ValueError(f"unknown AAF chunk id {cid} at {o-4:#x}")
    return out

def parse_wsys(d):
    """Returns (aw_name, [waves]) lists. Wave: dict(format, rate, start, length, key)."""
    assert d[:4] == b"WSYS"
    winf = be32(d, 0x10)
    assert d[winf:winf + 4] == b"WINF"
    arc_count = be32(d, winf + 4)
    arcs = []
    for ai in range(arc_count):
        a = be32(d, winf + 8 + ai * 4)
        name = d[a:d.index(b"\0", a)].decode()
        wave_count = be32(d, a + 0x70)
        waves = []
        for wi in range(wave_count):
            w = be32(d, a + 0x74 + wi * 4)
            fmt = d[w + 1]
            key = d[w + 2]
            rate = struct.unpack_from(">f", d, w + 4)[0]
            start = be32(d, w + 8)
            length = be32(d, w + 0xC)
            loop = be32(d, w + 0x10)
            waves.append(dict(fmt=fmt, key=key, rate=rate, start=start, length=length,
                              loop=loop))
        arcs.append((name, waves))
    return arcs

def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(rate))
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))

def main():
    szs, banks_dir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    arc = rarc_files(yaz0(open(szs, "rb").read()))
    aaf = arc["msound.aaf"]
    print("mSound.aaf:", len(aaf), "bytes")
    for blob in parse_aaf_wsys(aaf):
        for name, waves in parse_wsys(blob):
            aw_path = os.path.join(banks_dir, name)
            if not os.path.exists(aw_path):
                print(f"  (skip {name}: no .aw here)")
                continue
            aw = open(aw_path, "rb").read()
            print(f"{name}: {len(waves)} waves")
            for i, wv in enumerate(waves):
                # WSYS wave format: 0 = AFC 4-bit HQ (9-byte blocks), 1 = AFC 2-bit (5-byte)
                hq = wv["fmt"] == 0
                if wv["fmt"] not in (0, 1):
                    print(f"  wave{i}: fmt={wv['fmt']} (not AFC) rate={wv['rate']:.0f} "
                          f"start={wv['start']:#x} len={wv['length']:#x}")
                    continue
                pcm = afc_decode(aw[wv["start"]:wv["start"] + wv["length"]], hq)
                out = os.path.join(outdir, f"{os.path.splitext(name)[0]}_w{i:02d}.wav")
                write_wav(out, pcm, wv["rate"])
                print(f"  wave{i}: fmt={wv['fmt']} rate={wv['rate']:.0f} "
                      f"len={wv['length']:#x} → {out} ({len(pcm)/wv['rate']:.2f}s)")

if __name__ == "__main__":
    main()
