#!/usr/bin/env python3
"""dataref.py — name the code that references a guest DATA address.

addr2sym.py answers "what function is this CODE address in". The opposite question comes up just as
often and had no tool: a diagnostic prints a data pointer -- an OSThreadQueue a thread is asleep on,
a manager singleton, a global table -- and naming it means finding the instructions that build that
address and reading which function they belong to.

    python3 tools/re/dataref.py 0x800000e4 0x8040e10c
    python3 tools/re/dataref.py --window 32 0x800000e4
    python3 tools/re/dataref.py --selftest

A PowerPC 32-bit absolute address is built in two instructions: `lis rX, hi` then a displacement
form (`addi`, `ori`, or any D-form load/store) against that register. This scans every executable
section for such pairs and reports each hit as `function+offset`, so the answer is a name rather
than another address.

An address inside the small-data areas is not built that way at all: the ABI reserves r13 and r2 as
bases and every such global is one displacement from one of them. Those are scanned too, against the
bases `gmse01_sda.py` owns, so an SDA global gets the same answer as any other address rather than a
pointer at another tool.

One honest limit, reported rather than hidden. Register tracking is linear within a section: a `lis`
is remembered until that register is written again, and a branch into the middle of a pair is not
modelled, so a hit is evidence and a miss is not proof of absence. The r13/r2 forms have no such
limit, being one instruction each.
"""
import argparse
import bisect
import os
import struct
import sys

from gmse01_sda import SDA2_BASE, SDA_BASE, SDA_REGION_END, SDA_REGION_START

SELFTEST_REQUIREMENTS = ("game-image",)

# Two addresses whose answer is known, one reached each way, and one that nothing can reference.
# A scan that has lost either form still finds the other, so a test with only one of them passes
# while half the tool is dead.
SELFTEST_LIS_TARGET = 0x800000E4  # __OSCurrentThread, built with lis+lwz by the OS thread code
SELFTEST_LIS_SYMBOL = "OSGetCurrentThread"
SELFTEST_SMALL_DATA_TARGET = 0x8040E10C  # gpMarioPos, read through r13 by every chasing actor
SELFTEST_SMALL_DATA_SYMBOL = "TAnimalBird"
# One byte past gpMarioPos, inside the small-data region. This is the discriminating negative: a
# displacement scan that rounded, or that reported the whole word a reference lands in, would claim
# the 398 references above for this address too.
SELFTEST_ABSENT_TARGET = 0x8040E10D

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DOL = os.path.join(ROOT, "scratch", "bin", "sms.dol")
DEFAULT_FUNCS = os.path.join(ROOT, "reference", "sms_gmse01_funcs.txt")

# A DOL carries 7 text sections followed by 11 data sections; only the text ones hold instructions.
TEXT_SECTIONS = 7
TOTAL_SECTIONS = 18
MAX_FUNCTION_SPAN = 0x4000

# D-form opcodes whose immediate completes an address started by `lis`. addi/addic/ori cover pointer
# construction; the load/store forms cover a direct access to the global without materialising it.
DISPLACEMENT_OPCODES = {
    14: "addi", 12: "addic", 13: "addic.", 24: "ori",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu",
    38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau",
    44: "sth", 45: "sthu", 48: "lfs", 50: "lfd", 52: "stfs", 54: "stfd",
}


class DolImage:
    """The executable sections of a DOL, addressed by guest virtual address."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            raw = handle.read()
        if len(raw) < 0x100:
            raise ValueError(f"{path} is too small to be a DOL")
        offsets = struct.unpack(">18I", raw[0:72])
        addresses = struct.unpack(">18I", raw[72:144])
        sizes = struct.unpack(">18I", raw[144:216])
        self.text = []
        for index in range(TOTAL_SECTIONS):
            if sizes[index] == 0 or index >= TEXT_SECTIONS:
                continue
            body = raw[offsets[index]:offsets[index] + sizes[index]]
            self.text.append((addresses[index], body))
        if not self.text:
            raise ValueError(f"{path} has no executable sections")


def load_symbols(path):
    symbols = {}
    if not os.path.exists(path):
        return symbols, []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                symbols[int(parts[0], 16)] = parts[1]
            except ValueError:
                continue
    return symbols, sorted(symbols)


def name_of(symbols, sorted_addresses, address):
    if address in symbols:
        return symbols[address]
    index = bisect.bisect_right(sorted_addresses, address) - 1
    if index >= 0 and address - sorted_addresses[index] < MAX_FUNCTION_SPAN:
        return f"{symbols[sorted_addresses[index]]}+{address - sorted_addresses[index]:#x}"
    return f"{address:#010x} (no enclosing symbol)"


# The two reserved bases, and how a hit against each is named.
SMALL_DATA_BASES = ((13, SDA_BASE, "r13"), (2, SDA2_BASE, "r2"))


def find_references(image, target, window):
    """Every instruction in the image that forms `target`.

    A hit is either a `lis`+displacement pair or one small-data displacement from r13 or r2.
    Returns (hits, instructions_scanned); a hit is (use_address, mnemonic, formed_by), where
    `formed_by` names the `lis` that started the pair or the base register that stood in for it.
    """
    hits = []
    scanned = 0
    for base, body in image.text:
        pending = {}
        for position in range(0, len(body) - 3, 4):
            word = struct.unpack_from(">I", body, position)[0]
            address = base + position
            scanned += 1
            opcode = word >> 26
            destination = (word >> 21) & 0x1F
            source = (word >> 16) & 0x1F
            immediate = word & 0xFFFF
            if opcode == 15 and source == 0:  # lis rD, imm  (addis rD, r0, imm)
                pending[destination] = (immediate << 16, address)
                continue
            mnemonic = DISPLACEMENT_OPCODES.get(opcode)
            if mnemonic is not None and source != 0:
                for register, small_data_base, label in SMALL_DATA_BASES:
                    if source != register:
                        continue
                    signed = immediate - 0x10000 if immediate >= 0x8000 else immediate
                    if (small_data_base + signed) & 0xFFFFFFFF == target:
                        hits.append((address, mnemonic, label))
            if mnemonic is not None and source in pending:
                high, lis_address = pending[source]
                if address - lis_address <= window * 4:
                    signed = immediate - 0x10000 if immediate >= 0x8000 else immediate
                    formed = (high + (immediate if mnemonic == "ori" else signed)) & 0xFFFFFFFF
                    if formed == target:
                        hits.append((address, mnemonic, f"{lis_address:#010x}"))
            # Any instruction that writes a register invalidates the `lis` held there. D-form and
            # X-form differ in which field is the destination, so treat both as clobbering.
            if opcode in DISPLACEMENT_OPCODES or opcode == 15:
                pending.pop(destination, None)
            elif opcode == 31:
                pending.pop(source, None)
    return hits, scanned


def selftest(image, symbols, sorted_addresses, window):
    """Feed the scan one case of each form that MUST come out positive, and one that must not."""
    failures = []
    for target, expected, form in ((SELFTEST_LIS_TARGET, SELFTEST_LIS_SYMBOL, "lis pair"),
                                   (SELFTEST_SMALL_DATA_TARGET, SELFTEST_SMALL_DATA_SYMBOL,
                                    "small data")):
        hits, _ = find_references(image, target, window)
        names = [name_of(symbols, sorted_addresses, use) for use, _, _ in hits]
        matched = sum(1 for name in names if expected in name)
        print(f"selftest: {target:#010x} ({form}) -> {len(hits)} reference(s), "
              f"{matched} naming {expected}")
        if matched == 0:
            failures.append(f"{form} references to {target:#010x} are not being found")

    hits, scanned = find_references(image, SELFTEST_ABSENT_TARGET, window)
    print(f"selftest: {SELFTEST_ABSENT_TARGET:#010x} (absent) -> {len(hits)} reference(s) "
          f"in {scanned} instructions")
    if hits:
        failures.append(f"{SELFTEST_ABSENT_TARGET:#010x} is one byte past a global that is read "
                        "398 times and must not inherit those references")
    if scanned < 100000:
        failures.append(f"only {scanned} instructions were scanned; the image did not load")

    for failure in failures:
        print(f"selftest FAILED — {failure}")
    if failures:
        return 1
    print("selftest PASSED")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("addresses", nargs="*", help="guest data addresses, hex")
    parser.add_argument("--selftest", action="store_true",
                        help="scan cases whose answer is known and fail if any comes out wrong")
    parser.add_argument("--dol", default=DEFAULT_DOL)
    parser.add_argument("--funcs", default=DEFAULT_FUNCS)
    parser.add_argument("--window", type=int, default=16,
                        help="max instructions between the lis and its use (default 16)")
    args = parser.parse_args()

    if not os.path.exists(args.dol):
        print(f"dataref: no DOL at {args.dol}", file=sys.stderr)
        return 2
    image = DolImage(args.dol)
    symbols, sorted_addresses = load_symbols(args.funcs)
    if not symbols:
        print(f"dataref: no symbols at {args.funcs}; hits will be addresses only", file=sys.stderr)

    if args.selftest:
        return selftest(image, symbols, sorted_addresses, args.window)
    if not args.addresses:
        print("dataref: give at least one guest data address, or --selftest", file=sys.stderr)
        return 2

    for text in args.addresses:
        target = int(text, 16)
        hits, scanned = find_references(image, target, args.window)
        print(f"{target:#010x}: {len(hits)} reference(s) in {scanned} instructions")
        for use_address, mnemonic, formed_by in hits:
            print(f"  {use_address:#010x} {mnemonic:<6} (from {formed_by})  "
                  f"{name_of(symbols, sorted_addresses, use_address)}")
        if not hits:
            where = ("inside the small-data region, so a reference would be one displacement from "
                     "r13 or r2 and this scan covers those"
                     if SDA_REGION_START <= target < SDA_REGION_END
                     else "outside the small-data region")
            print(f"  none found; {target:#010x} is {where}. The address may instead be computed at "
                  "runtime -- a heap object, or an offset from a base this scan does not model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
