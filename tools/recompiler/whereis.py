#!/usr/bin/env python3
"""whereis.py — identify a guest address in the DOL.

The standalone recomp boot advances one blocker at a time, and each blocker is an
address. Answering "what is this, who reaches it, and is it a real function start?"
by hand costs several tool round-trips every iteration, so it lives here instead.

    python3 tools/recompiler/whereis.py 0x8033ba90 [0x80338e8c ...]
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOL = ROOT / "scratch/bin/sms.dol"
FUNCS = ROOT / "reference/sms_gmse01_funcs.txt"
JUMP_TABLE = ROOT / "sms-recomp/generated/jump_table.cpp"


class Dol:
    def __init__(self, path):
        self.d = path.read_bytes()
        be = lambda o: struct.unpack(">I", self.d[o:o + 4])[0]
        self.secs = [(be(i * 4), be(0x48 + i * 4), be(0x90 + i * 4)) for i in range(18)]
        self.secs = [s for s in self.secs if s[0] and s[2]]
        self.entry = be(0xE0)

    def word(self, a):
        for off, ad, sz in self.secs:
            if ad <= a < ad + sz:
                return struct.unpack(">I", self.d[off + a - ad:off + a - ad + 4])[0]
        return None


def symbols():
    out = {}
    if FUNCS.exists():
        for line in FUNCS.read_text(errors="replace").splitlines():
            p = line.split()
            if len(p) >= 2:
                try:
                    out[int(p[0], 16)] = p[1]
                except ValueError:
                    pass
    return out


def recompiled():
    if not JUMP_TABLE.exists():
        return []
    txt = JUMP_TABLE.read_text()
    return sorted(int(m, 16) for m in re.findall(r"\{ 0x([0-9a-f]{8})u,", txt))


def is_terminator(w):
    if w in (0x4E800020, 0x4E800420) or w == 0:
        return True
    op = w >> 26
    if op == 18 and not (w & 1):
        return True
    return op == 19 and ((w >> 1) & 0x3FF) in (16, 528)


def describe(addr, dol, syms, table):
    print(f"\n=== 0x{addr:08x} ===")
    sym = syms.get(addr)
    if sym:
        print(f"  symbol      : {sym}")
    else:
        prev = max((a for a in syms if a <= addr), default=None)
        if prev is not None:
            print(f"  symbol      : (none) — inside 0x{prev:08x} {syms[prev]} "
                  f"+0x{addr - prev:x}")

    if addr in table:
        print("  recompiled  : YES (own entry)")
    else:
        below = [a for a in table if a <= addr]
        if below:
            print(f"  recompiled  : NO — swallowed by 0x{below[-1]:08x}")
        else:
            print("  recompiled  : NO")

    prev_w = dol.word(addr - 4)
    here = dol.word(addr)
    if here is None:
        print("  in DOL      : NO (address is not in any section)")
        return
    starts = prev_w is not None and is_terminator(prev_w)
    print(f"  prev word   : {prev_w:08x} {'(terminator)' if starts else ''}")
    print(f"  looks like a function start: {'yes' if starts else 'NO — mid-body'}")
    print("  disassembly (words):")
    for a in range(addr, addr + 24, 4):
        w = dol.word(a)
        if w is None:
            break
        print(f"    {a:08x}  {w:08x}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    dol, syms, table = Dol(DOL), symbols(), recompiled()
    for a in sys.argv[1:]:
        describe(int(a, 16), dol, syms, table)
    return 0


if __name__ == "__main__":
    sys.exit(main())
