#!/usr/bin/env python3
"""Recover C++ vtable addresses from the US (GMSE01) DOL.

WHY THIS EXISTS
---------------
A recomp override that touches actor fields must first answer "is this guest object a
JDrama::TActor?", because `TViewObj*` is what the dispatch funnel hands it and not every
TViewObj is an actor. Writing a transform to `guest + 0x10` of a non-actor corrupts whatever
that object keeps there. The sound test is the guest vtable pointer at +0x00 against the set
of TActor-derived vtables.

That set is not in any reference file we have: `reference/sms_gmse01_funcs.txt` is US but
carries FUNCTIONS ONLY (zero `__vt__` symbols), while the 1,508 vtable symbols live in the JP
symbol data — and the recomp runs US.

HOW
---
A vtable is a run of big-endian pointers into .text. The US function entry addresses ARE
known, so a run of words that all land on known US function entries is a vtable candidate, and
each slot resolves to a NAME from the same list. A candidate is TActor-derived when it carries
at least one method whose mangled name belongs to class JDrama::TActor (`Q26JDrama6TActor`) —
an inherited, un-overridden slot.

WHAT IT REFUSES TO DO
---------------------
It does not print a bare list. Its primary output is COVERAGE, because the failure mode here is
silent: a class whose vtable is not recovered is an actor that never interpolates, and unlike
the decomp's virtual dispatch nothing will complain. A result is only usable as "recovered N of
M, and here are the M-N that were not". Missing inputs are a hard error, never an empty result.

LIMITS IT DECLARES ITSELF
-------------------------
* A subclass that overrides EVERY TActor method carries no TActor-owned slot and cannot be
  detected by this signal. Such classes appear in the `--audit` miss list, not silently.
* Slots pointing at functions absent from the US list (weak/inlined) break a run; MIN_SLOTS is
  deliberately low and merged runs are reported with their length so truncation is visible.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_DOL = REPO / "scratch" / "bin" / "sms.dol"
DEFAULT_FUNCS = REPO / "reference" / "sms_gmse01_funcs.txt"

# A run shorter than this is more likely a coincidental pointer table than a vtable.
MIN_SLOTS = 3

TACTOR_TAG = "Q26JDrama6TActor"


def die(msg):
    print(f"us_vtables: {msg}", file=sys.stderr)
    sys.exit(2)


def load_dol(path):
    """Return (sections, data) where sections is a list of (kind, file_off, addr, size)."""
    if not path.exists():
        die(f"DOL not found: {path} — searched NOTHING, this is not an empty result")
    raw = path.read_bytes()
    if len(raw) < 0x100:
        die(f"DOL too small ({len(raw)} bytes): {path}")
    offs = struct.unpack_from(">18I", raw, 0x00)
    addrs = struct.unpack_from(">18I", raw, 0x48)
    sizes = struct.unpack_from(">18I", raw, 0x90)
    sections = []
    for i in range(18):
        if sizes[i] == 0:
            continue
        kind = "text" if i < 7 else "data"
        sections.append((kind, offs[i], addrs[i], sizes[i]))
    if not sections:
        die("DOL header lists no non-empty sections — refusing to report")
    return sections, raw


def load_funcs(path):
    """address -> symbol name, from the US function list."""
    if not path.exists():
        die(f"US function list not found: {path} — searched NOTHING")
    funcs = {}
    pat = re.compile(r"^([0-9a-fA-F]{8})\s+(\S+)")
    for line in path.read_text(errors="replace").splitlines():
        m = pat.match(line)
        if m:
            funcs[int(m.group(1), 16)] = m.group(2)
    if not funcs:
        die(f"parsed 0 functions from {path} — the format must have changed; refusing")
    return funcs


def text_ranges(sections):
    return [(a, a + s) for (k, _o, a, s) in sections if k == "text"]


def scan(sections, raw, funcs, text):
    """Find runs of consecutive words that are all valid .text pointers.

    NOT "runs of KNOWN function entries" — that was the first version and it was wrong in a way
    that looked right. Many virtuals are absent from the US list (weak/inlined), so a run keyed on
    known entries STARTS at the first slot that happens to resolve, which is generally in the
    MIDDLE of the vtable. The address it reported was therefore not the vtable base, and a guest
    vptr — which points at the base — would never match it. The allowlist would have been 100%
    false-negative while looking fully populated.

    Keying on "is this a plausible .text pointer" finds the whole vtable, unknown slots included,
    so the run start IS the base. Known-function names are still used to CLASSIFY it.
    """
    def in_text(w):
        return any(lo <= w < hi for lo, hi in text) and (w & 3) == 0

    cands = []
    for kind, off, addr, size in sections:
        if kind != "data":
            continue
        n = size // 4
        words = struct.unpack_from(f">{n}I", raw, off)
        i = 0
        while i < n:
            if in_text(words[i]):
                j = i
                while j < n and in_text(words[j]):
                    j += 1
                if j - i >= MIN_SLOTS:
                    # MWCC vtable layout: two zero words (offset-to-top, typeinfo -- both zero
                    # with RTTI off) and the object's vptr points AT THEM, with the function
                    # pointers starting at vptr+8. Reporting the first function word was an
                    # off-by-8 that made every recovered address miss: a runtime dump of 133
                    # live vptrs matched ZERO candidates until this was corrected. Requiring the
                    # header also filters out ordinary function-pointer tables, which have none.
                    if i >= 2 and words[i - 1] == 0 and words[i - 2] == 0:
                        cands.append({"addr": addr + (i - 2) * 4,
                                      "slots": [words[k] for k in range(i, j)]})
                i = j
            else:
                i += 1
    return cands


CLASS_RE = re.compile(r"__(?:Q\d+(?:\d+\w+?)*?)?(\d+)([A-Za-z_]\w*)F")


def owning_classes(name):
    """Class names embedded in an MWCC-mangled symbol (`__8TFishoidF`, `Q26JDrama6TActor`).

    NESTED NAMES NEED THEIR OWN RULE. `Q<k>` introduces k qualified components, each `<len><ident>`:
    `Q26JDrama12TPolarCamera` is JDrama::TPolarCamera. Reading it with a plain `<len><ident>` scan
    takes the `2` of `Q2` and the `6` of `6JDrama` as the single length `26`, yielding the 26-char
    string 'JDrama12TPolarCameraFUlPQ2' — a class name that matches nothing.

    The consequence was silent and total: EVERY JDrama-namespaced class was invisible to the tagger,
    so no camera (`JDrama::TPolarCamera`, `JDrama::TLookAtCamera`) could ever be recognised, while
    non-namespaced classes like `TMirrorCamera` parsed correctly and made the tagger look healthy.
    A coverage audit scored 95.9% throughout, because it uses a different matcher.
    """
    out = set()
    i = 0
    while i < len(name):
        # Qualified name: Q<k> then k components.
        if name[i] == "Q" and i + 1 < len(name) and name[i + 1].isdigit():
            k = int(name[i + 1])
            j = i + 2
            comps = []
            for _ in range(k):
                m = re.match(r"(\d+)", name[j:])
                if not m:
                    break
                n = int(m.group(1))
                start = j + len(m.group(1))
                ident = name[start:start + n]
                if len(ident) != n:
                    break
                comps.append(ident)
                j = start + n
            if len(comps) == k:
                out.update(comps)          # every component, so JDrama::TActor tags on TActor
                i = j
                continue
        m = re.match(r"(\d+)([A-Za-z_]\w*)", name[i:])
        if m:
            n = int(m.group(1))
            start = i + len(m.group(1))
            ident = name[start:start + n]
            if len(ident) == n:
                out.add(ident)
                i = start + n
                continue
        i += 1
    return out


def classify(cands, funcs, actor_classes=None):
    """Tag a vtable as TActor-derived.

    NOT by looking for a `Q26JDrama6TActor` slot — that was the first rule and a runtime dump of
    133 live vptrs falsified it: only 4 matched, while objects the decomp had already PROVEN are
    TActors (滝つぼ, バルーンヘルプ, マップ) went untagged. The JSG* methods that carry the
    TActor tag live in the SECONDARY vtable (the JStage::TActor base), so that rule tagged
    secondary tables — never the primary one a vptr points at.

    The primary vtable instead carries the TNameRef/TViewObj/TPlacement/TActor chain, whose slots
    belong to the class itself or an ancestor. So the sound test is: does any resolved slot belong
    to a class the DECOMP says is TActor or derives from it?
    """
    actor_classes = actor_classes or set()
    for c in cands:
        names = [funcs.get(a, "") for a in c["slots"]]
        c["names"] = names
        c["resolved"] = sum(1 for a in c["slots"] if a in funcs)
        owners = set()
        for nm in names:
            if nm:
                owners |= owning_classes(nm)
        c["owners"] = owners
        c["is_tactor"] = bool(owners & actor_classes)
    return cands


def decomp_tactor_classes(root="TActor"):
    """Class names the DECOMP says derive from `root`.

    Transitive closure over `class X : public Y` in the decomp headers. This is the
    denominator the recovered set is scored against -- without it, "found 300 vtables"
    cannot be told apart from "found 300 of 900".

    The root is a PARAMETER because the right one depends on the FIELD being written, not on which
    class feels canonical. The interpolation snapshot writes mPosition at +0x10, which belongs to
    JDrama::TPlacement -- so anchoring the allowlist on TActor excluded every TPlacement that is not
    an actor, and cameras (`class TCamera : public TPlacement`) are exactly that. It also excluded
    them SILENTLY: a camera simply never appeared in the table, which reads identically to a camera
    that does not move.

    Choosing a wider root is only safe field by field. mRotation at +0x30 is a TActor field;
    TCamera's own layout ends at 0x30 (mFlag 0x24, mNear 0x28, mFar 0x2C), so writing a rotation
    there would corrupt whatever follows the object. Hence two separate lists rather than one wider
    one: TActor vtables (position AND rotation are safe) and TPlacement-but-not-TActor vtables
    (position ONLY).
    """
    inc = REPO / "decomp" / "sms" / "include"
    if not inc.is_dir():
        die(f"decomp headers not found at {inc} — cannot compute the coverage DENOMINATOR")
    base_of = {}
    pat = re.compile(r"\bclass\s+(\w+)\s*:\s*public\s+([\w:]+)")
    files = list(inc.rglob("*.hpp")) + list(inc.rglob("*.h"))
    if not files:
        die(f"no headers under {inc} — refusing to report a coverage number of unknown basis")
    for f in files:
        for m in pat.finditer(f.read_text(errors="replace")):
            base_of.setdefault(m.group(1), set()).add(m.group(2).split("::")[-1])
    derived, changed = {root}, True
    while changed:
        changed = False
        for cls, bases in base_of.items():
            if cls not in derived and (bases & derived):
                derived.add(cls)
                changed = True
    derived.discard(root)
    return derived, len(files)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", type=Path, default=DEFAULT_DOL)
    ap.add_argument("--funcs", type=Path, default=DEFAULT_FUNCS)
    ap.add_argument("--audit", action="store_true",
                    help="score the recovered set against the decomp's TActor hierarchy")
    ap.add_argument("--emit-header", type=Path,
                    help="write a C++ header with the sorted TActor-derived vtable addresses")
    ap.add_argument("--selftest", action="store_true",
                    help="feed a case that MUST produce a positive, and fail if it does not")
    ap.add_argument("--root", default="TActor",
                    help="hierarchy root to tag against (TActor, TPlacement, ...). Pick it from the "
                         "FIELD being written: mPosition@+0x10 is TPlacement's, mRotation@+0x30 is "
                         "TActor's")
    ap.add_argument("--exclude-root", default=None,
                    help="drop vtables also derived from this root, to emit a disjoint list "
                         "(e.g. --root TPlacement --exclude-root TActor = placements that are NOT "
                         "actors, for which ONLY position may be written)")
    ap.add_argument("--symbol", default="kTActorVtables",
                    help="C++ array name for --emit-header")
    args = ap.parse_args()

    sections, raw = load_dol(args.dol)
    funcs = load_funcs(args.funcs)
    actor_classes, _nfiles = decomp_tactor_classes(args.root)
    actor_classes = actor_classes | {args.root}
    cands = classify(scan(sections, raw, funcs, text_ranges(sections)), funcs, actor_classes)
    tactor = [c for c in cands if c["is_tactor"]]

    if args.exclude_root:
        excl, _ = decomp_tactor_classes(args.exclude_root)
        excl = excl | {args.exclude_root}
        excl_cands = classify(scan(sections, raw, funcs, text_ranges(sections)), funcs, excl)
        excl_addrs = {c["addr"] for c in excl_cands if c["is_tactor"]}
        before = len(tactor)
        tactor = [c for c in tactor if c["addr"] not in excl_addrs]
        # Say what was removed. A disjoint list that silently came out empty, or barely smaller than
        # the one it was carved from, means the two roots did not separate the way the caller
        # assumed -- and an unremarked count is how that goes unnoticed.
        print(f"  disjoint filter  : {before} tagged under {args.root}, "
              f"{before - len(tactor)} also under {args.exclude_root}, {len(tactor)} remain")

    if args.selftest:
        # MUST-PASS: TActor's own methods are in the US list, so at least one candidate has to
        # carry a TActor-owned slot. Zero means the scan or the tag is broken, not that the
        # game has no actors.
        ok = len(tactor) > 0 and len(funcs) > 1000 and len(cands) > 50
        print(f"selftest: funcs={len(funcs)} candidates={len(cands)} tactor-tagged={len(tactor)}")
        if not ok:
            print("selftest FAILED — the scan cannot detect a case that must be present")
            return 1
        print("selftest PASSED")
        return 0

    print(f"US vtable scan: {args.dol.name}")
    print(f"  sections scanned : {sum(1 for s in sections if s[0]=='data')} data "
          f"({sum(s[3] for s in sections if s[0]=='data')} bytes)")
    print(f"  known US funcs   : {len(funcs)}")
    print(f"  vtable candidates: {len(cands)}  (runs of >={MIN_SLOTS} valid .text pointers)")
    print(f"  TActor-tagged    : {len(tactor)}  (carry a slot owned by TActor or a decomp-derived class)")
    print("  NOT DETECTABLE by this signal: a subclass overriding EVERY TActor method carries")
    print("  no TActor-owned slot. Use --audit to see which decomp classes went unmatched.")

    if args.audit:
        derived, nfiles = decomp_tactor_classes(args.root)
        # Match a class to a candidate by any slot whose mangled name embeds the class name.
        matched = set()
        for c in cands:
            for nm in c["names"]:
                for cls in derived:
                    if f"{len(cls)}{cls}" in nm or f"__{cls}F" in nm:
                        matched.add(cls)
        missing = sorted(derived - matched)
        print()
        print(f"COVERAGE (denominator from {nfiles} decomp headers)")
        print(f"  decomp classes deriving from JDrama::{args.root} : {len(derived)}")
        print(f"  of those, matched to a recovered vtable      : {len(matched)}"
              f"  ({100.0*len(matched)/len(derived):.1f}%)")
        print(f"  UNMATCHED (would silently never interpolate) : {len(missing)}")
        for cls in missing[:40]:
            print(f"    - {cls}")
        if len(missing) > 40:
            print(f"    ... and {len(missing)-40} more")

    if args.emit_header:
        addrs = sorted(c["addr"] for c in tactor)
        with args.emit_header.open("w") as f:
            f.write("// GENERATED by tools/re/us_vtables.py -- do not edit.\n")
            f.write(f"// US (GMSE01) vtable addresses carrying a JDrama::{args.root}-owned slot.\n")
            f.write("// Coverage is NOT complete; see `us_vtables.py --audit`.\n")
            f.write("#pragma once\n#include <cstdint>\n\n")
            f.write(f"inline constexpr uint32_t {args.symbol}[] = {{\n")
            for a in addrs:
                f.write(f"    0x{a:08x}u,\n")
            f.write("};\n")
        print(f"\nwrote {args.emit_header} ({len(addrs)} vtables)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
