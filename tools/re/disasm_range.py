#!/usr/bin/env python3
"""Disassemble a PPC address range from a raw DOL, with a name resolver.

Usage: disasm_range.py <sms.dol> <start_hex> <end_hex> [funcs.txt]

Output: capstone PPC disassembly with `bl` targets resolved to nearest funcs.txt symbol.
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

def resolve(addr):
    # Return nearest-preceding symbol + offset, e.g. "GXSetColorUpdate+0x10".
    import bisect
    ks = [s[0] for s in symbols]
    i = bisect.bisect_right(ks, addr) - 1
    if i < 0: return f"{addr:08x}"
    a, name = symbols[i]
    off = addr - a
    return name if off == 0 else f"{name}+0x{off:x}"

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
            target_str = f"  -> {resolve(tgt)}"
        except: pass
    elif op.startswith("bc") and " 0x" in ops:
        # bc conditional: "cr,cond,0xADDR" or similar — take the last 0x-prefixed token
        toks = ops.split(",")
        for t in reversed(toks):
            t = t.strip()
            if t.startswith("0x"):
                try:
                    tgt = int(t, 16)
                    target_str = f"  -> {resolve(tgt)}"
                    break
                except: pass
    print(f"{ins.address:08x}: {ins.bytes.hex()}  {op:8s} {ops}{target_str}")
