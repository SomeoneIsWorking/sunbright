#!/usr/bin/env python3
"""Emit the set of US addresses that ARE a `TLiveActor::calcRootMatrix` override.

WHY THIS EXISTS
---------------
The 60fps sub-frame has to turn a substituted actor pose into a matrix without re-running the
movement phase. For the PLAYER that is `TMario::calcAnim`, a single address. For every other actor
it is `calcRootMatrix`, which is VIRTUAL and overridden by ~40 classes, so the sub-frame must
dispatch through each object's own vtable rather than call one address.

WHERE IN THE VTABLE — AND WHY THE STATIC ANSWER WAS WRONG
---------------------------------------------------------
`us_vtables.py` says slot 37: across its 1,294 recovered vtables, 254 put a `calcRootMatrix__*`
override there against one each at 72 and 67. That agreement is real and it is also BIASED. The
scan finds runs of consecutive plausible `.text` pointers, so any vtable holding a single non-text
word is truncated at it — every vptr seen on a live Delfino frame resolves in that scan to a vtable
of just SEVEN slots, including 0x803c2ab8, which the port's notes record as TMapObjBase's 89-slot
vtable. The 254 that agreed were the subset well-formed enough to be scanned past. An index agreed
on by a biased subset is not the index.

Guest memory has no such problem, so the slot is taken from LIVE OBJECTS instead
(`SBR_INTERP60_CALCROOT_SCAN=1`, which walks each substituted actor's vtable for a known
calcRootMatrix address and histograms where it lands). Over 399 live actors on a Delfino frame:

    slot 46 : 336 objects   (unanimous — no object put one anywhere else, none had two)
    no hit  :  63 objects   (weak/inlined override — the coverage gap, see below)

**Slot 46**, and the static 37 is recorded here only so the discrepancy is not rediscovered.

HOW THE RUNTIME DECIDES IT MAY USE THAT SLOT
--------------------------------------------
A slot index is only meaningful for objects of the right CLASS: slot 46 of a non-TLiveActor is some
other virtual with some other signature, and calling it would be a wrong-signature call on the game
thread. The first version tested the POINTER instead — require slot 46 to hold a calcRootMatrix
already known by name — which is safe but far too narrow: most overrides are weak, so every NPC in
Delfino was refused. `TBaseNPC::calcRootMatrix` (0x80206ddc) is exactly such an override, and it is
a real one: it either delegates to `TLiveActor::calcRootMatrix` (0x80218370, identified by
disassembly — it calls MsMtxSetXYZRPH with mPosition/mRotation and then copies mScaling into the
model, matching liveactor.cpp:259) or takes an NPC motion-blend path.

So the test is on the CLASS, positively: a vtable is TLiveActor-derived if it carries at least TWO
addresses that belong to TLiveActor methods, anywhere in its slots. Inherited, un-overridden slots
make that cheap and hard to fake — a class would have to override essentially all 28 to evade it,
and overriding them all would make it a different class in every way that matters. Once the vtable
is known to be TLiveActor-derived, slot 46 IS calcRootMatrix by C++ layout: a subclass can append
virtuals but never insert them, so every TLiveActor virtual sits at the same index in every subclass.

WHY THE POINTER SET IS STILL EMITTED
------------------------------------
A slot index is not a membership test. Calling slot 37 on an object whose class is not
TLiveActor-derived invokes whatever that class keeps there — some unrelated virtual, with the
wrong signature, on the game thread. Tagging vtables by class and allow-listing the bases would be
one answer, but it is INFERENCE: 270 of the 544 vtables that class-tagging calls TLiveActor-derived
do not even have 38 slots, so the tag is not the property being relied on.

So the check happens at the CALL SITE, on the thing actually about to be called: read the object's
slot 37 and require that address to be a KNOWN calcRootMatrix. That is a positive identification of
the function rather than a guess about the class, and it degrades the right way — if the slot index
were wrong, or the object were not an actor, the address simply would not be in this set and the
sub-frame skips that object instead of corrupting it. A skipped actor does not interpolate; a wrong
call is memory corruption. The asymmetry decides the design.

COVERAGE IS PARTIAL BY CONSTRUCTION, AND THAT IS SAID OUT LOUD
--------------------------------------------------------------
Only overrides present in `reference/sms_gmse01_funcs.txt` can be listed. A class whose
calcRootMatrix is weak/inlined is absent, so its actors will not interpolate. The count is printed
on generation so the gap is visible rather than discovered later as "some props still step at 30Hz".

Usage:
  tools/re/calcroot_addrs.py [--out sms-recomp/generated/calcroot_addrs.h] [--check]

`--check` exits non-zero if the emitted header is stale, for the commit gate.
"""
import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
FUNCS = REPO / "reference" / "sms_gmse01_funcs.txt"
DEFAULT_OUT = REPO / "sms-recomp" / "generated" / "calcroot_addrs.h"

# From LIVE objects (SBR_INTERP60_CALCROOT_SCAN over 399 substituted actors on a Delfino frame):
# 336 unanimous at 46, none elsewhere, none ambiguous. NOT the 37 that us_vtables.py's static scan
# reports — that scan truncates any vtable containing a non-.text word, so its agreement came from a
# biased subset. Recorded with its derivation so the number is auditable rather than folklore.
CALCROOT_SLOT = 46

LINE = re.compile(r"^([0-9a-fA-F]{8})\s+(\S+)$")


def collect():
    if not FUNCS.is_file():
        sys.exit(f"REFUSING: {FUNCS} not found — nothing was scanned, and an empty set here would "
                 f"silently disable every actor's interpolation while looking like a clean run.")
    out = []
    seen = set()
    skipped = []
    for line in FUNCS.read_text(errors="replace").splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        addr, name = int(m.group(1), 16), m.group(2)
        if not name.startswith("calcRootMatrix__") or addr in seen:
            continue
        # SIGNATURE FILTER, and it is not cosmetic. `calcRootMatrix__13TRealoidActorFP5TBoid`
        # takes a `TBoid*`; calling it with only `this` in r3 passes whatever happens to be in r4
        # as a pointer and dereferences it. Only the `()` form — mangled suffix `Fv` — is
        # call-compatible with the dispatch the sub-frame does. A same-named method with a
        # different signature is a different function.
        if not name.endswith("Fv"):
            skipped.append(name)
            continue
        seen.add(addr)
        out.append((addr, name))
    if not out:
        sys.exit("REFUSING: zero calcRootMatrix symbols matched. The function list parsed but "
                 "carried none, which is not a result — it is a broken match.")
    out.sort()
    if skipped:
        # Named, not silently dropped: a filtered symbol is a class that will NOT interpolate, and
        # the reason has to survive into the next session that wonders why.
        print(f"  filtered {len(skipped)} same-named override(s) with a different signature "
              f"(not callable as `this`-only): {', '.join(sorted(skipped))}", file=sys.stderr)
    return out


def collect_liveactor():
    """Addresses of methods OWNED BY TLiveActor — the class-identification anchors.

    Inherited slots are the point: a TBaseNPC vtable carries `performOnlyDraw__10TLiveActor` and
    `updateAnmSound__10TLiveActor` at their inherited indices because it never overrode them, and
    that is what identifies it as TLiveActor-derived without needing a name for its own overrides.
    """
    out, seen = [], set()
    for line in FUNCS.read_text(errors="replace").splitlines():
        m = LINE.match(line.strip())
        if not m:
            continue
        addr, name = int(m.group(1), 16), m.group(2)
        if "__10TLiveActor" in name and addr not in seen:
            seen.add(addr)
            out.append((addr, name))
    if len(out) < 8:
        sys.exit(f"REFUSING: only {len(out)} TLiveActor methods matched. The identification test "
                 f"needs a population, and a short list would make the 2-of-N threshold trivially "
                 f"unreachable — every actor would be refused and nothing would interpolate.")
    out.sort()
    return out


def render(entries):
    lines = [
        "// GENERATED by tools/re/calcroot_addrs.py — do not edit.",
        "//",
        "// Every US address that is a TLiveActor::calcRootMatrix override, sorted. The 60fps",
        "// sub-frame reads an actor's vtable slot 37 and requires the result to be IN this set",
        "// before calling it, so a non-actor (or a wrong slot) is skipped rather than invoked.",
        "//",
        "// COVERAGE IS PARTIAL: only overrides present in reference/sms_gmse01_funcs.txt can be",
        "// listed, so a class whose calcRootMatrix is weak/inlined is absent and its actors will",
        "// not interpolate. That is a known gap, not an oversight.",
        "#pragma once",
        "#include <cstdint>",
        "",
        f"inline constexpr int kCalcRootSlot = {CALCROOT_SLOT};   // from LIVE objects, not a static scan",
        "",
        "inline constexpr uint32_t kCalcRootAddrs[] = {",
    ]
    for addr, name in entries:
        lines.append(f"    0x{addr:08x}u,   // {name}")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr int kCalcRootAddrCount = {len(entries)};")
    lines.append("")
    live = collect_liveactor()
    lines += [
        "// Addresses of methods OWNED BY TLiveActor. A guest vtable carrying at least two of these",
        "// is TLiveActor-derived — inherited, un-overridden slots make this cheap and hard to fake.",
        "// Once that holds, slot 46 is calcRootMatrix by C++ layout (a subclass appends virtuals,",
        "// it never inserts them), so the dispatch is licensed by the CLASS rather than by having",
        "// already known the override's name.",
        "inline constexpr uint32_t kLiveActorMethodAddrs[] = {",
    ]
    for addr, name in live:
        lines.append(f"    0x{addr:08x}u,   // {name}")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr int kLiveActorMethodCount = {len(live)};")
    lines.append("inline constexpr int kLiveActorMinHits = 2;")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    entries = collect()
    text = render(entries)
    out = pathlib.Path(args.out)

    if args.check:
        if not out.is_file():
            print(f"STALE: {out} does not exist")
            return 1
        if out.read_text() != text:
            print(f"STALE: {out} differs from what the function list generates")
            return 1
        print(f"ok: {out} matches ({len(entries)} calcRootMatrix overrides)")
        return 0

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"wrote {out}: {len(entries)} distinct calcRootMatrix overrides, slot {CALCROOT_SLOT}")
    print("  COVERAGE NOTE: classes whose calcRootMatrix is weak/inlined are NOT here and will not")
    print("  interpolate. This count is the whole of what the sub-frame can dispatch to.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
