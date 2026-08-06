#!/usr/bin/env python3
"""addr2sym.py — name a guest address from the US function list.

Every diagnostic in this project that prints a guest address expects the reader to resolve it, and
until now that meant grepping reference/sms_gmse01_funcs.txt by hand and eyeballing which entry the
address falls inside. That is a lookup, so it should be a tool.

    python3 tools/re/addr2sym.py 0x802e0390 0x80244800
    python3 tools/re/addr2sym.py --stdin < some.log        # resolve every 0x8xxxxxxx it finds

An address INSIDE a function resolves to `name+offset`, which is the common case for a return
address captured mid-call. An address before the first symbol or after the last is reported as
UNRESOLVED with its nearest neighbour, never silently attributed to whatever happens to precede it —
a return address attributed to the wrong function is worse than one left unnamed.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FUNCS = os.path.join(ROOT, "reference", "sms_gmse01_funcs.txt")
# Beyond this an address is not "inside" the preceding function, it is past the end of it. SMS's
# largest functions are a few KB; 64 KB is far outside anything real and still catches genuine
# large-function hits.
MAX_OFFSET = 0x10000


def load():
    if not os.path.isfile(FUNCS):
        # REFUSE. A missing symbol file must not produce "no symbols found", which reads as "these
        # addresses are unknown" rather than "nothing was searched".
        sys.exit(f"REFUSES: {FUNCS} does not exist, so NOTHING was searched. This is not a result.")
    syms = []
    for line in open(FUNCS, encoding="utf-8", errors="ignore"):
        parts = line.split()
        if len(parts) >= 2:
            try:
                syms.append((int(parts[0], 16), parts[1]))
            except ValueError:
                continue
    if not syms:
        sys.exit(f"REFUSES: {FUNCS} parsed to zero symbols. Nothing was searched.")
    syms.sort()
    return syms


def resolve(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return None, f"below the first symbol ({syms[0][1]} @ 0x{syms[0][0]:08x})"
    base, name = syms[best]
    off = addr - base
    if off > MAX_OFFSET:
        return None, (f"UNRESOLVED — {off:#x} past {name} @ 0x{base:08x}, which is beyond any real "
                      f"function. Not attributed.")
    return (name, off), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--stdin", action="store_true", help="resolve every 0x8xxxxxxx found on stdin")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    syms = load()

    if a.selftest:
        ok = True
        # A known symbol must resolve exactly, and the SAME address +4 must resolve to +4 — the
        # in-function case that a bare exact-match lookup would miss.
        base, name = syms[len(syms) // 2]
        for probe, want in ((base, 0), (base + 4, 4)):
            r, err = resolve(syms, probe)
            good = r is not None and r[0] == name and r[1] == want
            print(f"  {'PASS' if good else 'FAIL'}  0x{probe:08x} -> {name}+{want}")
            ok = ok and good
        # An address far past the last symbol must REFUSE rather than attribute to it.
        r, err = resolve(syms, syms[-1][0] + MAX_OFFSET * 4)
        good = r is None
        print(f"  {'PASS' if good else 'FAIL'}  far-past-last address is left UNRESOLVED")
        ok = ok and good
        print("SELFTEST", "PASSED" if ok else "FAILED")
        return 0 if ok else 1

    want = list(a.addrs)
    if a.stdin:
        want += re.findall(r"0x8[0-9a-fA-F]{7}", sys.stdin.read())
    if not want:
        ap.error("give at least one address, --stdin, or --selftest")

    seen = []
    for t in want:
        try:
            addr = int(t, 16)
        except ValueError:
            print(f"{t}: not a hex address")
            continue
        if addr in seen:
            continue
        seen.append(addr)
        r, err = resolve(syms, addr)
        if r is None:
            print(f"0x{addr:08x}  {err}")
        else:
            name, off = r
            print(f"0x{addr:08x}  {name}" + (f"+0x{off:x}" if off else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
