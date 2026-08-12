#!/usr/bin/env python3
"""dol_sda.py — resolve SDA2 constants and r13 globals referenced by a function in sms.dol.

The recurring blocker named in memory session10-autonomous-port-cycle-2026-07-01: many
candidate ports touch SDA2 constants at negative offsets from `_SDA2_BASE_ = 0x80416BA0`
(usually f32/f64 literals for math thresholds) or r13-relative globals at negative offsets
from `_SDA_BASE_  = 0x804141C0` (usually pointers to engine singletons like gpMarDirector,
gpMarioPos, gpCamera, gpSun, gpApplication). Each port re-derived these from raw disasm at
about 20 minutes/port; this tool dumps them in seconds.

Usage:
    tools/re/dol_sda.py <hex_addr> [n_instructions]
        Scan the function starting at <hex_addr> for SDA references and dump each with its
        resolved value. Default scan length: up to first `blr` (or 256 instructions).

    tools/re/dol_sda.py --sda2 0xNNNN
        One-shot: resolve a single SDA2 constant at `_SDA2_BASE_ - 0xNNNN` as f32 + f64.

    tools/re/dol_sda.py --sda1 0xNNNN
        One-shot: resolve r13-relative global at `_SDA_BASE_ - 0xNNNN` as pointer, look up
        the target in reference/sms_gmse01_funcs.txt if it points to code.

The output is designed to paste into a port's comments — each line names the offset AND
the value AND (for pointers) the resolved symbol name.
"""

import os, sys, struct

# THREE dirnames: this file is at <repo>/tools/re/dol_sda.py, so two levels reach <repo>/tools
# and the DOL path resolved to <repo>/tools/scratch/bin/sms.dol — which does not exist, so the
# tool died on import with a FileNotFoundError for anyone who ran it from a checkout.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOL_PATH  = os.path.join(REPO_ROOT, "scratch/bin/sms.dol")
FUNCS_TXT = os.path.join(REPO_ROOT, "reference/sms_gmse01_funcs.txt")

SDA_BASE  = 0x804141C0   # r13 base — engine singletons + game state
SDA2_BASE = 0x80416BA0   # r2  base — float/double constants in .sdata2 / .rodata

# Known r13-relative global names (GMSE01, US). Populated from prior port work — extend as
# new offsets are identified. `.sbss` symbols live at these VAs but are zero at rest in the
# DOL image (runtime-populated), so we can't dump their live values from the file — we CAN
# name them. Format: SDA1 offset (signed) → symbol name.
KNOWN_SDA1 = {
    -0x6048: "gpMarDirector",           # System/MarDirector.hpp — mMap at +0x7C
    -0x60b4: "gpMarioPos",              # MapObjWave RE — Vec3 (world-space)
    -0x6070: "gpMarioParticleManager",  # ShiningStone RE
    -0x6328: "gpMap",                   # TMBindShadowManager::request passes to isInArea
    -0x7118: "gpCamera or gpPolarCamera",  # TSunMgr passes to CPolarSubCamera::isNowInbetween;
                                        # also TMBindShadowManager::request reads mForwardPos +0x50
    -0x62b8: "gpMapObjManager",         # ShiningStone::load — getMActorAnmData() for map-obj MActors
    -0x6318: "gpMirrorModelManager",    # MammaMirrorMapOperator::perform — unk18 = active-mirror idx, unk24 = TMirrorCamera
    -0x6308: "gpMapWireManager",        # TWireBell::loadAfter — getWireNo(const Vec3&) → wire index
    -0x6060: "gpFlagManager",           # TRedCoinSwitch::load / TCoverFruit::loadAfter — TFlagManager::getShineFlag / getFlag
    -0x6044: "gpMSound",                # TMapObjSwitch::control — MSound::playTimer + game-wide SE dispatch
    -0x5fe0: "gpResourceManager",       # ShiningStone::load — JPAResourceManager::load(name, id)
    -0x626c: "gpMapObjWave",            # TMap::update + TMapObjWaterFilter::perform — getHeight(x,y,z) receiver
    -0x7160: "MSGBasic",                # MSoundSE::startSoundActorInner — JAIBasic cast of MSGMSound
    -0x7164: "MSGMSound",               # MSoundSE::startSoundActorInner — .unkD1 = stage sound bank
    -0x7168: "MSSeCallBack::smWaterFilter",  # u16, NOT a pointer (in-place value)
    -0x6010: "gArBkGuide",               # TARAMBlock for guide.arc. Identified structurally, not
                                         # by name: the DOL forms this address in 5 places (TGuide
                                         # dtor/setup/load, TApplication::mountStageArchive x2) and
                                         # at the mountStageArchive sites +0x0 is filled by
                                         # JKRDvdAramRipper::loadToAram with a u8 flag at +0x4 --
                                         # TARAMBlock's layout.
    -0x63a8: "TGuide::sMountCountdown",  # u8, NOT a pointer. Touched by exactly 5 instructions in
                                         # the image, ALL in TGuide, so it is that class's
                                         # file-scope static: setup/load set it to 0x10 when there
                                         # is no archive to mount, and TGuide::perform (0x801791d0,
                                         # weak, so absent from the US list) decrements it and
                                         # calls SMSSwitch2DArchive at 0. A frame delay, not a
                                         # refcount -- an earlier note here said "the dtor", which
                                         # was the disassembly rendering perform's body as
                                         # __dt__6TGuideFv+0x.. because perform is unlisted.
    # -0x5db8: setup-holder used by MammaMirror loadAfter ((unaff_r13-0x5db8)+4)
    # -0x6044: (TSunMgr reads [+0x7c] after — sibling of gpMarDirector; maybe gpApplication child)
    # -0x7100: (TSunMgr/LensFlare test bit at +0x15 — maybe cinematics-manager, TApplication child)
    # -0x7110: (LensFlare passes to FUN_80028f38 — likely visibility/camera query)
    # -0x70f8: (LensFlare + TSunMgr — clip-x at +0xf8, clip-y at +0xfc = sun's projected pos;
    #          likely gpSun/gpSunOccluder/similar)
    # -0x7814: (HangingBridge draw arg — maybe TDrawSyncManager)
    # -0x789c: (ResetFruit sets flag to this value = some manager instance)
}

# ── DOL memory map ──────────────────────────────────────────────────────────
DOL = open(DOL_PATH, "rb").read()
_offs  = struct.unpack(">18I", DOL[0:72])
_addrs = struct.unpack(">18I", DOL[72:144])
_sizes = struct.unpack(">18I", DOL[144:216])
_secs  = [(_addrs[i], _offs[i], _sizes[i]) for i in range(18) if _sizes[i]]

def foff(va):
    for a, o, s in _secs:
        if a <= va < a + s:
            return o + (va - a)
    return None

def read_bytes(va, n):
    fo = foff(va)
    if fo is None: return None
    return DOL[fo:fo+n]

def read_u32_be(va):
    b = read_bytes(va, 4)
    return None if b is None else struct.unpack(">I", b)[0]

def read_f32_be(va):
    b = read_bytes(va, 4)
    return None if b is None else struct.unpack(">f", b)[0]

def read_f64_be(va):
    b = read_bytes(va, 8)
    return None if b is None else struct.unpack(">d", b)[0]

# ── Function symbol lookup ──────────────────────────────────────────────────
_FUNCS = None
def load_funcs():
    global _FUNCS
    if _FUNCS is not None: return _FUNCS
    _FUNCS = {}
    if not os.path.exists(FUNCS_TXT): return _FUNCS
    with open(FUNCS_TXT) as f:
        for line in f:
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                try: _FUNCS[int(parts[0], 16)] = parts[1]
                except ValueError: pass
    return _FUNCS

def symbol_at(va):
    """Return function name at VA, or the nearest function containing VA (with +offset)."""
    funcs = load_funcs()
    if va in funcs: return funcs[va]
    # nearest lower
    prev = max((a for a in funcs if a <= va), default=None)
    if prev is None: return None
    off = va - prev
    if off < 0x2000: # reasonable function size cap
        return f"{funcs[prev]}+0x{off:x}"
    return None

# ── SDA constant resolver ───────────────────────────────────────────────────
def resolve_sda2(offset_signed):
    """Given a signed 16-bit offset (immediate as used in `lfs f, off(r2)`), report the
    constant at SDA2_BASE + offset as u32/f32/f64."""
    va = (SDA2_BASE + offset_signed) & 0xFFFFFFFF
    u   = read_u32_be(va)
    f32 = read_f32_be(va)
    f64 = read_f64_be(va)
    return (va, u, f32, f64)

def resolve_sda1(offset_signed):
    """Given a signed 16-bit offset (as used in `lwz r, off(r13)`), report the u32 at
    SDA_BASE + offset — usually a pointer to an engine global. Look up its target in
    funcs.txt for named identification. Also cross-references KNOWN_SDA1 for the global's
    OWN name (e.g. r13-0x6048 = gpMarDirector, regardless of what it currently points at)."""
    va  = (SDA_BASE + offset_signed) & 0xFFFFFFFF
    u   = read_u32_be(va)
    fsym = symbol_at(u) if u is not None else None
    gsym = KNOWN_SDA1.get(offset_signed)
    return (va, u, fsym, gsym)

# ── Disasm-lite: catch instructions with r2 or r13 as base register ─────────
# We use a byte-level PPC decoder just for the specific forms we care about, so this
# script has zero runtime deps (no capstone). Covered forms:
#   addi/addis  rD, rA, simm       — opcodes 14 (0x38) / 15 (0x3C)
#   lwz/stw     rD, simm(rA)       — opcodes 32 (0x80) / 36 (0x90)
#   lhz/lha     rD, simm(rA)       — opcodes 40 (0xA0) / 42 (0xA8)
#   lbz/stb     rD, simm(rA)       — opcodes 34 (0x88) / 38 (0x98)
#   lfs/stfs    fD, simm(rA)       — opcodes 48 (0xC0) / 52 (0xD0)
#   lfd/stfd    fD, simm(rA)       — opcodes 50 (0xC8) / 54 (0xD8)
# rA==0 in addi means literal (li); rA==0 in loads is EA=simm (uncommon). We skip those.
# rA==2 → SDA2 reference; rA==13 → SDA1 reference.
MNEMONICS = {
    0x38: "addi",  0x3C: "addis",
    0x80: "lwz",   0x90: "stw",
    0xA0: "lhz",   0xA8: "lha",
    0x88: "lbz",   0x98: "stb",
    0xC0: "lfs",   0xD0: "stfs",
    0xC8: "lfd",   0xD8: "stfd",
}
IS_FLOAT_LOAD = {0xC0, 0xD0, 0xC8, 0xD8}

def decode_d_form(word):
    """Return (mnem, rD, rA, simm) or None if not a supported D-form instruction."""
    op = (word >> 26) & 0x3F
    op_byte = op << 2
    mnem = MNEMONICS.get(op_byte)
    if mnem is None: return None
    rD = (word >> 21) & 0x1F
    rA = (word >> 16) & 0x1F
    simm = word & 0xFFFF
    if simm & 0x8000: simm -= 0x10000  # sign-extend
    return (mnem, rD, rA, simm)

def scan_function(start_va, max_words=256):
    """Yield (va, word, decode) for each instruction; stop at first `blr` (0x4E800020)."""
    sda1_refs, sda2_refs = {}, {}
    va = start_va
    for _ in range(max_words):
        w = read_u32_be(va)
        if w is None: break
        if w == 0x4E800020:  # blr
            break
        dec = decode_d_form(w)
        if dec is not None:
            mnem, rD, rA, simm = dec
            if rA == 2:
                sda2_refs.setdefault(simm, []).append((va, mnem, rD, IS_FLOAT_LOAD.__contains__(w >> 26 << 2)))
            elif rA == 13:
                sda1_refs.setdefault(simm, []).append((va, mnem, rD))
        va += 4
    return sda1_refs, sda2_refs

# ── CLI ─────────────────────────────────────────────────────────────────────
def cmd_scan(addr, n=None):
    print(f"# SDA scan of function at 0x{addr:08x}")
    print(f"#   SDA_BASE  (r13) = 0x{SDA_BASE:08x}")
    print(f"#   SDA2_BASE (r2)  = 0x{SDA2_BASE:08x}")
    sda1_refs, sda2_refs = scan_function(addr, n or 256)

    if sda2_refs:
        print(f"\n## SDA2 constants (r2 references, likely f32/f64 math literals)")
        for simm in sorted(sda2_refs):
            va, u, f32, f64 = resolve_sda2(simm)
            uses = sda2_refs[simm]
            mnems = ",".join(sorted({m for (_, m, _, _) in uses}))
            f32_str = f"{f32:.6g}"       if f32  is not None else "?"
            f64_str = f"{f64:.6g}"       if f64  is not None else "?"
            u_str   = f"0x{u:08x}"       if u    is not None else "?"
            print(f"  r2 - 0x{-simm & 0xFFFF:04x}  @0x{va:08x}  "
                  f"u32={u_str}  f32={f32_str}  f64={f64_str}  ({mnems}, {len(uses)} use)")

    if sda1_refs:
        print(f"\n## SDA1 globals (r13 references, likely pointers to engine singletons)")
        for simm in sorted(sda1_refs):
            va, u, fsym, gsym = resolve_sda1(simm)
            uses = sda1_refs[simm]
            mnems = ",".join(sorted({m for (_, m, _) in uses}))
            u_str = f"0x{u:08x}" if u is not None else "<sbss:0 at rest>"
            fsym_str = f"  → {fsym}" if fsym else ""
            gsym_str = f"  [= {gsym}]" if gsym else "  [unknown]"
            print(f"  r13 - 0x{-simm & 0xFFFF:04x}  @0x{va:08x}{gsym_str}  ({mnems}, {len(uses)} use){fsym_str}")

    if not sda1_refs and not sda2_refs:
        print("\n(no SDA1 or SDA2 references found in this function)")

def cmd_sda2_one(offset):
    va, u, f32, f64 = resolve_sda2(offset)
    u_str = f"0x{u:08x}" if u is not None else "?"
    print(f"SDA2[-0x{-offset & 0xFFFF:04x}]  @0x{va:08x}  u32={u_str}  f32={f32}  f64={f64}")

def cmd_sda1_one(offset):
    va, u, fsym, gsym = resolve_sda1(offset)
    u_str = f"0x{u:08x}" if u is not None else "<sbss:0 at rest>"
    gsym_str = f" [= {gsym}]" if gsym else " [unknown]"
    fsym_str = f" → {fsym}" if fsym else ""
    print(f"SDA1[-0x{-offset & 0xFFFF:04x}]  @0x{va:08x}{gsym_str}  u32={u_str}{fsym_str}")

def usage():
    print(__doc__)
    sys.exit(1)

if __name__ == "__main__":
    args = sys.argv[1:]
    if not args: usage()
    if args[0] == "--sda2":
        if len(args) != 2: usage()
        off = int(args[1], 0)
        if off > 0: off = -off  # convenience: `--sda2 0x76a8` == `--sda2 -0x76a8`
        cmd_sda2_one(off)
    elif args[0] == "--sda1":
        if len(args) != 2: usage()
        off = int(args[1], 0)
        if off > 0: off = -off
        cmd_sda1_one(off)
    else:
        addr = int(args[0], 0)
        n    = int(args[1], 0) if len(args) >= 2 else None
        cmd_scan(addr, n)
