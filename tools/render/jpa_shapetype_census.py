#!/usr/bin/env python3
"""JPA shapeType census — verify-first tooling for the N7 particle port.

Scans the game's particle data for every JPABaseShape ('BSP1') block and counts
the shapeType byte (mType = data[0x24] from the 'BSP1' magic, per
reference/sms/.../JPABaseShape.cpp:102). This answers, from DATA (not from a
scene sweep), which JPA draw types actually EXIST in the shipped game — so we
never port a renderer path for a type that can never appear (verify-first).

Sources scanned:
  - /data/particle.szs            : the global particle archive (Yaz0 -> RARC of .jpa)
  - /data/scene/*.szs             : every per-scene archive (Yaz0; .jpa embedded)

shapeType map (JPADrawVisitor.cpp dispatch):
  0 Point  1 Line  2 BillBoard  3 Direction  4 DirectionCross  5 Stripe
  6 StripeCross  7 Rotation  8 RotationCross  9 DirBillBoard  10 YBillBoard

Usage:
  jpa_shapetype_census.py <rom.rvz>            # extract from ROM via sunbright-jingle
  jpa_shapetype_census.py --dir scratch/scenes # scan already-extracted *.szs tree

Finding (2026-06-19): t7/t8 (Rotation/RotationCross) occur ZERO times game-wide.
The Rotation family is dead code in SMS — do NOT spend effort reaching/porting it.
"""
import sys, os, glob, subprocess, tempfile
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "jingle"))
from jingle import yaz0  # noqa: E402

TYPE_NAME = {0: "Point", 1: "Line", 2: "BillBoard", 3: "Direction",
             4: "DirectionCross", 5: "Stripe", 6: "StripeCross",
             7: "Rotation", 8: "RotationCross", 9: "DirBillBoard",
             10: "YBillBoard"}


def scan_bsp1(data):
    """Count shapeType (byte at +0x24 from each 'BSP1' magic) in a blob."""
    c = Counter()
    i = 0
    while True:
        j = data.find(b"BSP1", i)
        if j < 0:
            break
        if j + 0x25 <= len(data):
            c[data[j + 0x24]] += 1
        i = j + 4
    return c


def scan_szs(path):
    raw = open(path, "rb").read()
    dec = yaz0(raw) if raw[:4] == b"Yaz0" else raw
    return scan_bsp1(dec)


def find_jingle():
    for b in sorted(glob.glob("build*/sunbright-jingle"), key=os.path.getmtime, reverse=True):
        return b
    raise SystemExit("no sunbright-jingle binary found (build it)")


def extract_from_rom(rom):
    d = tempfile.mkdtemp(prefix="jpa_census_")
    jingle = find_jingle()
    subprocess.run([jingle, rom, "--extract", "/data/particle.szs", d],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([jingle, rom, "--extract", "/data/scene/", d],
                   check=True, stdout=subprocess.DEVNULL)
    return d


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if sys.argv[1] == "--dir":
        root = sys.argv[2]
    else:
        root = extract_from_rom(sys.argv[1])

    total = Counter()
    per_type_scenes = {}  # type -> list of (scene, count)
    files = []
    g = os.path.join(root, "data", "particle.szs")
    if os.path.exists(g):
        files.append(g)
    files += sorted(glob.glob(os.path.join(root, "data", "scene", "*.szs")))

    for f in files:
        c = scan_szs(f)
        total += c
        for st, n in c.items():
            per_type_scenes.setdefault(st, []).append((os.path.basename(f), n))

    print(f"scanned {len(files)} archive(s) under {root}\n")
    print("=== game-wide BSP1 shapeType census ===")
    for st in sorted(total):
        print(f"  t{st:<2d} {TYPE_NAME.get(st, '?'):16s} : {total[st]}")
    for st in (7, 8):
        if st not in total:
            print(f"  t{st:<2d} {TYPE_NAME[st]:16s} : 0  (NOT PRESENT — dead code in SMS)")
    print("\n=== per-type scene breakdown (the rarer/unported types) ===")
    for st in (0, 1, 4, 5, 7, 8, 10):
        scenes = per_type_scenes.get(st, [])
        if scenes:
            tag = ", ".join(f"{n}×{c}" for n, c in scenes[:40])
            print(f"  t{st} {TYPE_NAME[st]}: {tag}")


if __name__ == "__main__":
    main()
