#!/usr/bin/env python3
"""Resolve a WEAK virtual method's US address via vtable-slot alignment, then disassemble it.

port_dossier.py can only find functions listed in reference/sms_gmse01_funcs.txt. Many base-class
virtuals (e.g. TMapObjBase::kill / getHitObjNumMax / loadBeforeInit) are *weak* and absent from
that list, so the US address is unknown. But a SUBCLASS that overrides the method and IS listed
gives an anchor: the override sits at the same vtable slot index in every subclass, so we can:
  1. locate the base class's vtable in the US DOL (via any US-listed method of that class),
  2. locate the anchor subclass's vtable (via the override address),
  3. align the two vtables on a shared inherited method pointer,
  4. read the base vtable at the override's slot index -> the weak base method's US address,
  5. disassemble it.

Usage:
  vtable_re.py <vt-anchor-method> <override-symbol> [n_insns]

    <vt-anchor-method>  a US-listed method of the BASE class, used to find the base vtable
                        (e.g. calcRootMatrix__11TMapObjBaseFv). Pass its name or 0xADDR.
    <override-symbol>   a US-listed SUBCLASS override of the target method, the slot anchor
                        (e.g. getHitObjNumMax__9TEggYoshiFv or 0x801bf5f4).
    [n_insns]           how many instructions to disassemble (default 16).

Example (TMapObjBase::getHitObjNumMax, overridden by TEggYoshi):
  vtable_re.py calcRootMatrix__11TMapObjBaseFv getHitObjNumMax__9TEggYoshiFv
Data: scratch/bin/sms.dol, reference/sms_gmse01_funcs.txt.
"""
import sys, struct, bisect, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
raw = (ROOT / "scratch/bin/sms.dol").read_bytes()
offs = struct.unpack_from(">18I", raw, 0x00)
loads = struct.unpack_from(">18I", raw, 0x48)
sizes = struct.unpack_from(">18I", raw, 0x90)
SECS = [(loads[i], offs[i], sizes[i]) for i in range(18) if sizes[i]]

def rva_off(rva):
    for load, off, sz in SECS:
        if load <= rva < load + sz:
            return off + (rva - load)
    return None

def rd(rva):
    o = rva_off(rva)
    return struct.unpack_from(">I", raw, o)[0] if o is not None else None

SYMS = []
for line in (ROOT / "reference/sms_gmse01_funcs.txt").read_text().splitlines():
    p = line.split()
    if len(p) >= 2:
        try:
            SYMS.append((int(p[0], 16), " ".join(p[1:])))
        except ValueError:
            pass
SYMS.sort()
KS = [a for a, _ in SYMS]
NAME2ADDR = {n: a for a, n in SYMS}

def resolve(a):
    if not a or a < 0x80000000 or a >= 0x80400000:
        return "NULL" if not a else f"{a:08x}"
    i = bisect.bisect_right(KS, a) - 1
    aa, nn = SYMS[i]
    return nn if aa == a else f"{nn}+0x{a - aa:x}"

def to_addr(tok):
    if tok.startswith("0x"):
        return int(tok, 16)
    if tok in NAME2ADDR:
        return NAME2ADDR[tok]
    hits = [(a, n) for a, n in SYMS if tok in n]
    if not hits:
        sys.exit(f"[vtable_re] no symbol/addr for '{tok}'")
    return hits[0][0]

def find_ptr(val, lo=0x80300000):
    out = []
    for load, off, sz in SECS:
        if load < lo:
            continue
        for p in range(0, sz - 4, 4):
            if struct.unpack_from(">I", raw, off + p)[0] == val:
                out.append(load + p)
    return out

def vtable_base(anchor_method_addr):
    """Find the vtable slot0 by locating anchor_method_addr in .data and walking back to slot0.
    Returns (slot0_rva, index_of_anchor)."""
    locs = find_ptr(anchor_method_addr)
    if not locs:
        sys.exit(f"[vtable_re] anchor method {anchor_method_addr:08x} not found in any vtable")
    # Take the first; slot0 is the run start. Walk back while previous words look like code ptrs.
    loc = locs[0]
    s0 = loc
    while True:
        prev = rd(s0 - 4)
        if prev and 0x80000000 <= prev < 0x80400000:
            s0 -= 4
        else:
            break
    return s0, (loc - s0) // 4

def peek(rva, n):
    words = []
    for k in range(n):
        w = rd(rva + 4 * k)
        if w is None:
            break
        words.append(w)
        if w == 0x4E800020:  # blr
            break
    lines = []
    try:
        from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
        md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
        blob = b"".join(struct.pack(">I", w) for w in words)
        for ins in md.disasm(blob, rva):
            tgt = ""
            if ins.mnemonic in ("bl", "b"):
                try:
                    tgt = "  -> " + resolve(int(ins.op_str.replace("0x", ""), 16))
                except ValueError:
                    pass
            lines.append(f"  {ins.address:08x}: {ins.mnemonic} {ins.op_str}{tgt}")
    except ImportError:
        for k, w in enumerate(words):
            lines.append(f"  {rva + 4 * k:08x}: .{w:08x}")
    return lines

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    anchor_method = to_addr(sys.argv[1])
    override = to_addr(sys.argv[2])
    n_insns = int(sys.argv[3]) if len(sys.argv) > 3 else 16

    base_s0, _ = vtable_base(anchor_method)
    over_locs = find_ptr(override)
    if not over_locs:
        sys.exit(f"[vtable_re] override {override:08x} not found in any vtable")

    # Align by HISTOGRAM VOTING: the two vtables share the same inherited-method layout, so the
    # correct slot-delta is the one where the MOST base/override pointer pairs agree. A single
    # coincidental match (e.g. a JDrama getter shared by everything) must not decide the delta.
    NVT = 200
    base_slot0, _ = vtable_base(anchor_method)
    base_ptr = {i: rd(base_slot0 + 4 * i) for i in range(NVT)}
    best = None  # (votes, delta, over_slot0)
    for oloc in over_locs:
        # find override's slot0 the same way (walk back to run start)
        os0 = oloc
        while True:
            prev = rd(os0 - 4)
            if prev and 0x80000000 <= prev < 0x80400000:
                os0 -= 4
            else:
                break
        over_ptr = {i: rd(os0 + 4 * i) for i in range(NVT)}
        over_idx = (oloc - os0) // 4
        # delta such that base_ptr[j] == over_ptr[j] for the shared prefix; try all plausible deltas
        from collections import Counter
        votes = Counter()
        for j, ov in over_ptr.items():
            if ov and 0x80000000 <= ov < 0x80400000:
                for bi, bv in base_ptr.items():
                    if bv == ov:
                        votes[bi - j] += 1
        if not votes:
            continue
        delta, nvotes = votes.most_common(1)[0]
        if best is None or nvotes > best[0]:
            best = (nvotes, delta, os0, over_idx)
    if best is None:
        sys.exit("[vtable_re] could not align vtables (no shared method pointers)")
    nvotes, delta, os0, over_idx = best
    slot = over_idx + delta  # base slot index for the target method
    tgt = rd(base_slot0 + 4 * slot)
    print(f"base vtable slot0 = {base_slot0:08x}  (override vtable slot0 = {os0:08x})")
    print(f"aligned by voting: delta={delta} with {nvotes} agreeing shared slots")
    print(f"override slot index = {over_idx}  ->  base slot index = {slot}")
    print(f"base method US addr = {tgt:08x}  ({resolve(tgt)})")
    for line in check_result(slot, nvotes, tgt):
        print(line)
    print("disasm:")
    for ln in peek(tgt, n_insns):
        print(ln)


# Minimum agreeing slots to believe an alignment. Calibrated against BOTH classes on 2026-08-12:
# a correct same-immediate-base alignment (TResetFruit vs TMapObjGeneral) scored 87, while three
# alignments that were provably wrong scored 6 and 8. Anything in single digits is two classes
# sharing only a shallow JDrama::TViewObj prefix, where the vote has nothing to lock onto.
MIN_VOTES = 20


def looks_like_entry(addr):
    """Does `addr` begin like a real function? A wrong alignment lands MID-function, and this is
    the cheapest structural way to notice. MWCC function entries here start by saving LR
    (`mflr r0`) or opening a frame (`stwu r1, -N(r1)`); a leaf may start with neither, so a False
    here is a WARNING and not by itself a refusal."""
    try:
        w = rd(addr)
    except Exception:
        return False
    return w == 0x7C0802A6 or (w >> 16) == 0x9421


def check_result(slot, nvotes, tgt):
    """Refuse a result that cannot be right, instead of printing it confidently.

    WHY THIS EXISTS. On 2026-08-12 this tool resolved three weak methods to slot indices of -110,
    -43 and -45 and printed the addresses without comment; each landed deep inside an unrelated
    function (`getNextIndex+0x123c`, `resetLife+0x1930`). A NEGATIVE vtable slot is structurally
    impossible, so that answer was never a near miss — it was noise with a confident face on it.
    A porter following one of those addresses would have transcribed the middle of another class's
    method as their function body, and nothing downstream would have said otherwise.
    """
    out = []
    fatal = []
    if slot < 0:
        fatal.append(f"base slot index {slot} is NEGATIVE, which is structurally impossible — "
                     f"the alignment is wrong, not merely uncertain.")
    if nvotes < MIN_VOTES:
        fatal.append(f"only {nvotes} agreeing shared slots (need >= {MIN_VOTES}). The two classes "
                     f"probably share just a shallow base prefix, so the vote had nothing to lock "
                     f"onto. Pick an override from a class with the SAME immediate base.")
    if fatal:
        for f in fatal:
            out.append(f"REFUSED: {f}")
        out.append("REFUSED: no address is reported as a result. Re-run with a better-matched "
                   "override symbol; do NOT port from the address printed above.")
        print("\n".join(out), file=sys.stderr)
        sys.exit(2)
    if not looks_like_entry(tgt):
        out.append("WARNING: the target does not start with `mflr r0` or `stwu r1,-N(r1)`. That is "
                   "normal for a leaf function and suspicious for anything else — confirm the "
                   "disassembly below reads as a function ENTRY before porting it.")
    return out

if __name__ == "__main__":
    main()
