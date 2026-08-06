#!/usr/bin/env python3
"""Disassemble a PPC address range from a raw DOL, with a name resolver.

Usage: disasm_range.py <sms.dol> <start_hex> <end_hex> [funcs.txt]

Output: capstone PPC disassembly with branch targets resolved against funcs.txt.

A `bl` target is a function ENTRY, so it is named only on an EXACT match; anything else is reported
as an unnamed function at its address. It is never rendered as `nearest_symbol+offset`, because that
reads as a call into that symbol and the gaps between listed entries are full of weak/inlined
functions the list does not carry. Local branches keep the offset form.
"""
import sys, struct
from pathlib import Path

if len(sys.argv) < 4:
    print(__doc__); sys.exit(1)

dol_path = Path(sys.argv[1])
start = int(sys.argv[2], 16)
end = int(sys.argv[3], 16)
funcs_path = Path(sys.argv[4]) if len(sys.argv) > 4 else None

# --- DOL parse (Gekko DOL: 7 text + 11 data sections, sizes/loads at 0x00/0x48/0x90) ---
raw = dol_path.read_bytes()
hdr_off = struct.unpack_from(">7I", raw, 0x00)
hdr_load = struct.unpack_from(">7I", raw, 0x48)
hdr_size = struct.unpack_from(">7I", raw, 0x90)
sections = [(hdr_load[i], hdr_off[i], hdr_size[i]) for i in range(7)]
def rva_to_off(rva):
    for load, off, sz in sections:
        if load and load <= rva < load + sz:
            return off + (rva - load)
    return None

start_off = rva_to_off(start)
end_off   = rva_to_off(end - 4) + 4 if rva_to_off(end - 4) else None
if start_off is None or end_off is None:
    print(f"address range not mapped in DOL"); sys.exit(2)
code = raw[start_off:end_off]

# --- funcs.txt for symbol resolution (addr NAME per line) ---
symbols = []  # (addr, name) sorted
if funcs_path and funcs_path.exists():
    for line in funcs_path.read_text().splitlines():
        line = line.strip()
        if not line: continue
        parts = line.split(None, 1)
        if len(parts) >= 2:
            try: symbols.append((int(parts[0], 16), parts[1]))
            except: pass
    symbols.sort()

def resolve(addr, is_call):
    """Name a branch target — and REFUSE to name a call it cannot identify.

    This function used to return nearest-preceding-symbol+offset unconditionally, so a `bl` to an
    UNNAMED function that merely follows a known one rendered as `known_symbol+0x80`. That is not a
    formatting nit: it reads as "this function calls known_symbol", and it produced a confidently
    wrong RE conclusion (a call to the unnamed TLiveActor::calcRootMatrix at 0x80218370 was read as
    a call to setGroundCollision, which it follows in the text, and an NPC's calcRootMatrix override
    was written off as a motion-blend routine on the strength of it).

    funcs.txt carries ENTRY ADDRESSES ONLY, with no sizes, so "inside the gap after symbol N" and
    "inside symbol N" are indistinguishable — and the gaps are full of weak/inlined functions that
    the list does not carry. A `bl` target is by definition a function ENTRY, so a non-exact match
    means the callee is UNNAMED, not interior. Say that.

    Local branches (b/bc within the function being disassembled) legitimately land mid-function, so
    they keep the offset form — but they are never presented as a callee.
    """
    import bisect
    ks = [s[0] for s in symbols]
    i = bisect.bisect_right(ks, addr) - 1
    if i < 0:
        return f"0x{addr:08x} <no symbol at or before this address>"
    a, name = symbols[i]
    off = addr - a
    if off == 0:
        return name
    if is_call:
        # The honest rendering: the callee has no entry in the list. The neighbouring symbol is
        # offered only as a LOCATION, phrased so it cannot be misread as the thing being called.
        return f"0x{addr:08x} <UNNAMED fn; not a listed entry — lies {off:#x} after {name}>"
    return f"{name}+{off:#x}"

# --- Disassemble ---
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
md.detail = True
md.skipdata = True  # emit '.byte' for undecodable words instead of stopping mid-stream

for ins in md.disasm(code, start):
    # Resolve bl/b/bc target if this is a branch to an absolute address that's in .text.
    target_str = ""
    op = ins.mnemonic
    ops = ins.op_str
    if op in ("bl", "b", "ba", "bla") and ops.startswith("0x"):
        try:
            tgt = int(ops, 16)
            # Only bl/bla are CALLS. A plain `b` is a tail-call or a local jump; treating it as a
            # call would apply the strict naming to ordinary intra-function control flow.
            target_str = f"  -> {resolve(tgt, op in ('bl', 'bla'))}"
        except: pass
    elif op.startswith("bc") and " 0x" in ops:
        # bc conditional: "cr,cond,0xADDR" or similar — take the last 0x-prefixed token
        toks = ops.split(",")
        for t in reversed(toks):
            t = t.strip()
            if t.startswith("0x"):
                try:
                    tgt = int(t, 16)
                    target_str = f"  -> {resolve(tgt, False)}"
                    break
                except: pass
    print(f"{ins.address:08x}: {ins.bytes.hex()}  {op:8s} {ops}{target_str}")
