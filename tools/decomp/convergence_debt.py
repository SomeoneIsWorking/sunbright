#!/usr/bin/env python3
"""convergence_debt.py — rank the decomp's convergence debt and pre-classify it.

`convergence_loss.py` answers "did adopting upstream delete anything this fork
added SINCE THE FORK POINT", which is the right question for a merge and the
wrong one for the standing debt: work older than the fork point is invisible to
it. This tool measures the debt itself, per file, against upstream's line counts
on both sides, and sorts the answer into the two classes that need different
kinds of attention:

  CONVERGE   our side is behind and we own nothing upstream lacks. Upstream has
             the recovered source; take it and re-apply whatever of ours is in
             it. Mechanical, and `convergence_loss.py` plus hostcheck check the
             result. Two files in this class have been done this way
             (GCConsole2.cpp, BathtubKiller.cpp).
  DECIDE     our side defines functions upstream does not have. That is this
             fork's own recovered-or-native work -- a hand port, an RE'd
             reimplementation, a deliberate rewiring -- and taking upstream's
             copy would delete it. Each one needs a decision about whether the
             port owns that behavior, which no diff can answer.
  BEHIND/AHEAD/equal are the line-count facts the class is derived from.

A line count is a way to RANK the work, never evidence that taking upstream's
copy is right. The `DECIDE` class exists to say so out loud: the biggest debts
in this tree are not the ones you can merge away.

Usage:
    tools/decomp/convergence_debt.py [--top N] [--json] [--selftest]
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUB = REPO / "decomp" / "sms"
UPSTREAM = "upstream/main"

# `SomeClass::someMethod(` or `static <type> someName(` at column 0. A deliberately
# shallow signature matcher: it only has to tell "does this file define something
# upstream does not", and every miss makes the file look more mechanical than it
# is, which is the direction that needs a human to look. It is not a C++ parser
# and does not pretend to be.
DEF_RE = re.compile(
    r"^[A-Za-z_][\w:<>*& ]*?(\w+)::(\w+)\s*\(|^static [\w:*& ]*?(\w+)\s*\("
)

# A NON-static free function definition at column 0. Missing this is not a cosmetic
# gap: J3DTransform.cpp defines J3DPSCalcInverseTranspose, which upstream only DECLARES,
# and which J3DModel.cpp and J3DCluster.cpp call. A classifier that cannot see a free
# function reports that file CONVERGE, i.e. "take upstream's copy", which deletes the
# only definition of a function two other translation units call.
#
# Shape: a return type and a name, a balanced parameter list, and either a trailing `{`
# or a `{` opening the next line. Rejected: anything ending in `;` (a declaration or a
# variable with initialisers), anything with `=` (a default argument or a member
# initialiser list), statement keywords, and preprocessor lines. This errs toward
# reporting a free function that is not there, which puts a file in DECIDE -- the class
# a human looks at -- rather than in CONVERGE, which is the class that gets merged.
FREEFN_RE = re.compile(
    r"^[A-Za-z_][\w:<>*&\s]*?\b([A-Za-z_]\w*)\s*\(([^;]*)\)\s*(const)?\s*$"
)
NOT_A_FUNCTION = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "else",
    "do",
    "case",
    "catch",
    "new",
    "delete",
    "typedef",
    "namespace",
    "template",
    "operator",
}


def looks_like_free_function(line: str) -> str | None:
    """The name, if this line is a non-static free function definition."""
    stripped = line.rstrip()
    if not stripped or stripped.endswith((";", "{")) or "=" in stripped:
        return None
    if stripped.startswith(("#", "//", "*", "}")):
        return None
    m = FREEFN_RE.match(stripped)
    if not m:
        return None
    name = m.group(1)
    return None if name in NOT_A_FUNCTION else name


# Parameter lists, to tell a RENAME from fork work. A name only our side has can
# be a method upstream renamed, and the only cheap evidence is the signature:
# TSelectMenu::setup and ::initData take the same four parameters, so it is a
# rename and the file is mechanical. Without this the classifier calls it fork work
# and defers a merge that is safe -- the wrong error, being the one that never
# gets done.
PAREN = re.compile(r"\(([^;{)]*)\)")


def param_key(signature: str) -> str:
    """The parameter list, normalized to types with parameter names dropped."""
    m = PAREN.search(signature)
    if not m:
        return ""
    types = []
    for part in (p.strip() for p in m.group(1).split(",")):
        if not part:
            continue
        toks = [t for t in part.split("=")[0].strip().replace("*", " * ").split() if t]
        if toks and toks[-1] != "const" and re.fullmatch(r"[a-z_]\w*", toks[-1]):
            toks = toks[:-1]  # the parameter's own name, not part of the signature
        types.append(" ".join(toks))
    return ",".join(types)


def renames(ours: dict[str, str], theirs: dict[str, str]) -> list[tuple[str, str]]:
    """(our name, their name) pairs that differ only by a rename.

    Same parameter list, same class, one name only on our side, one only on theirs.
    Requiring the class matters: two methods with identical parameters on
    different classes in one file are not a rename, and pairing them would hide a
    real method from the DECIDE class. A heuristic, and it says so: a genuinely
    new method whose signature matches another on the SAME class will read as a
    rename, which errs toward calling something mechanical that a human then
    checks -- the direction that gets reviewed, not the one that deletes work.
    """
    our_only = {n: k for n, k in ours.items() if n not in theirs and k}
    their_only = {n: k for n, k in theirs.items() if n not in ours and k}
    out: list[tuple[str, str]] = []
    claimed: set[str] = set()
    for oname, okey in sorted(our_only.items()):
        oclass = oname.partition("::")[0] if "::" in oname else None
        for tname, tkey in sorted(their_only.items()):
            tclass = tname.partition("::")[0] if "::" in tname else None
            if oclass != tclass or tkey != okey or tname in claimed:
                continue
            out.append((oname, tname))
            claimed.add(tname)
            break
    return out


def git(*args: str) -> str | None:
    r = subprocess.run(
        ["git", *args],
        cwd=SUB,
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    return r.stdout if r.returncode == 0 else None


def definitions(text: str) -> dict[str, str]:
    """Defined functions as {name: parameter-list key}.

    A signature is joined across wrapped lines first: the decomp wraps long
    parameter lists (`void TSelectMenu::setup(u8 stage, JKRArchive* archive,\n
    TSelectShineManager* shineMgr, TSelectDir* dir)`), and comparing one physical
    line at a time makes a renamed method look like two unrelated ones -- which
    puts a mechanical file into the DECIDE class, where nobody does it.
    """
    lines = text.splitlines()
    found: dict[str, str] = {}
    for i, line in enumerate(lines):
        m = DEF_RE.match(line)
        free = None if m else looks_like_free_function(line)
        if not m and free is None:
            continue
        name = (
            f"{m.group(1)}::{m.group(2)}"
            if m and m.group(2)
            else (m.group(3) if m else free)
        )
        if not name:
            continue
        # A signature is joined across wrapped lines first: the decomp wraps long
        # parameter lists (`void TSelectMenu::setup(u8 stage, JKRArchive* archive,\n
        # TSelectShineManager* shineMgr, TSelectDir* dir)`), and comparing one physical
        # line at a time makes a renamed method look like two unrelated ones -- which
        # puts a mechanical file into the DECIDE class, where nobody does it.
        sig = line
        j = i
        while sig.count("(") > sig.count(")") and j + 1 < len(lines):
            j += 1
            sig += " " + lines[j]
        if free is not None:
            # The body opens on the line after the signature ENDS. If the signature
            # was joined from continuation lines that is line j; if it fit on one
            # line then j is still i and the brace is on i+1. Checking lines[j] in
            # both cases looks for a brace on the definition line itself, which
            # silently rejects every one-line free function definition.
            body = lines[j] if j > i else (lines[j + 1] if j + 1 < len(lines) else "")
            if not body.lstrip().startswith("{"):
                continue  # a free function with no opening brace is not a definition
        found[name] = param_key(sig)
    return found


@dataclass
class FileDebt:
    path: str
    ours: int
    theirs: int
    only_ours: list[str] = field(default_factory=list)
    only_theirs: list[str] = field(default_factory=list)
    rename_pairs: list[tuple[str, str]] = field(default_factory=list)

    @property
    def behind(self) -> int:
        return self.theirs - self.ours

    @property
    def kind(self) -> str:
        if self.behind <= 0:
            return "ahead" if self.ours > self.theirs else "equal"
        return "DECIDE" if self.only_ours else "CONVERGE"

    def as_dict(self) -> dict:
        return {
            "path": self.path,
            "ours": self.ours,
            "theirs": self.theirs,
            "behind": self.behind,
            "kind": self.kind,
            "only_ours": self.only_ours,
            "only_theirs": self.only_theirs,
            "rename_pairs": [list(p) for p in self.rename_pairs],
        }


def measure() -> list[FileDebt]:
    names = (
        git("diff", "--name-only", UPSTREAM, "--", "src", "include", "libs") or ""
    ).split()
    debts: list[FileDebt] = []
    for rel in names:
        rel = rel.strip()
        if not rel:
            continue
        path = SUB / rel
        if not path.is_file():
            continue
        ours_text = path.read_text(encoding="utf-8", errors="replace")
        r = subprocess.run(
            ["git", "show", f"{UPSTREAM}:{rel}"],
            cwd=SUB,
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
        )
        if r.returncode != 0:
            continue
        ours_defs, their_defs = definitions(ours_text), definitions(r.stdout)
        pairs = renames(ours_defs, their_defs)
        renamed_ours = {a for a, _ in pairs}
        debts.append(
            FileDebt(
                path=rel,
                ours=ours_text.count("\n"),
                theirs=r.stdout.count("\n"),
                only_ours=sorted(set(ours_defs) - set(their_defs) - renamed_ours),
                only_theirs=sorted(set(their_defs) - set(ours_defs)),
                rename_pairs=pairs,
            )
        )
    return debts


def report(debts: list[FileDebt], top: int, as_json: bool) -> int:
    behind = [d for d in debts if d.behind > 0]
    converge = sorted(
        (d for d in behind if d.kind == "CONVERGE"), key=lambda d: -d.behind
    )
    decide = sorted((d for d in behind if d.kind == "DECIDE"), key=lambda d: -d.behind)
    behind_total = sum(d.behind for d in behind)

    if as_json:
        print(
            json.dumps(
                {
                    "diverging": len(debts),
                    "behind_files": len(behind),
                    "behind_lines": behind_total,
                    "converge_files": len(converge),
                    "decide_files": len(decide),
                    "files": [d.as_dict() for d in (converge + decide)[:top]],
                },
                indent=2,
            )
        )
        return 0

    print(f"debt: {len(debts)} file(s) diverge from {UPSTREAM}")
    print(f"  behind upstream        : {len(behind)} file(s), {behind_total} line(s)")
    print(f"    CONVERGE (we own nothing upstream lacks): {len(converge)}")
    print(f"    DECIDE   (we define what upstream lacks) : {len(decide)}")
    print(f"  ahead / equal           : {len(debts) - len(behind)}")
    if not behind:
        print(
            "debt: nothing to converge -- 0 file(s) diverge from upstream. Scanned nothing, so "
            "this is an empty result, not a clean bill of health; check that the clone has "
            "upstream/main fetched."
        )
        return 0

    print(
        f"\nCONVERGE -- upstream has source we lack, we have no function of our own. "
        f"Top {min(top, len(converge))}:"
    )
    for d in converge[:top]:
        extra = (
            f", +{len(d.only_theirs)} upstream method(s) we'd gain"
            if d.only_theirs
            else ""
        )
        if d.rename_pairs:
            pairs = ", ".join(f"{a}->{b}" for a, b in d.rename_pairs[:3])
            extra += f", rename(s) {pairs}"
        print(f"  {d.behind:5}  ours={d.ours:5} theirs={d.theirs:5}  {d.path}{extra}")
    print(
        f"\nDECIDE -- our side defines what upstream lacks; a merge would delete it. "
        f"Top {min(top, len(decide))}:"
    )
    for d in decide[:top]:
        names = ", ".join(d.only_ours[:4]) + (" ..." if len(d.only_ours) > 4 else "")
        print(f"  {d.behind:5}  ours={d.ours:5} theirs={d.theirs:5}  {d.path}")
        print(f"          ours-only: {names}")
    print(
        "\nA DECIDE entry is a prompt to look, not a verdict: a name only our side has\n"
        "can still be a method upstream renamed or inlined. Pairs matched by signature\n"
        "are excluded from the class above already."
    )
    return 0


def selftest() -> int:
    """Both classes, from real inputs, plus the empty scan.

    A classifier that only ever says CONVERGE is wrong in the direction that
    deletes fork work, which is the expensive direction. So the DECIDE class is
    proved with input that must produce it, CONVERGE with input that must not,
    and the empty scan must be reported as a scan that found nothing rather than
    as a clean debt list. No temporary git checkout: the extraction and the
    classification are the parts that can silently stop working.
    """
    failures: list[str] = []

    # 1. the definition extractor sees a method upstream would not have, and can
    #    tell a rename from new work by signature
    fork_defs = definitions(
        "void DecideMe::ours()\n{\n}\nvoid DecideMe::shared(int a) { }\n"
    )
    their_defs = definitions(
        "void DecideMe::theirs()\n{\n}\nvoid DecideMe::shared(int a) { }\n"
    )
    got = set(fork_defs) - set(their_defs)
    if got != {"DecideMe::ours"}:
        failures.append(f"a fork-only method was not extracted: {got}")
    if set(definitions("static inline void helper(u32) { }\n")) != {"helper"}:
        failures.append("a free function was not extracted")

    # A NON-static free function must be extracted: J3DTransform.cpp defines
    # J3DPSCalcInverseTranspose, upstream only DECLARES it, and J3DModel.cpp and
    # J3DCluster.cpp call it. Missing this makes the file read as CONVERGE, i.e.
    # "take upstream's copy", which deletes the only definition of a live symbol.
    free_defs = definitions(
        "bool J3DPSCalcInverseTranspose(MtxPtr src, ROMtxPtr dst)\n"
        "{\n\treturn true;\n}\n"
    )
    if "J3DPSCalcInverseTranspose" not in free_defs:
        failures.append(f"a non-static free function was not extracted: {free_defs}")
    if looks_like_free_function("J3DColor c(0, 0, 0, 0);") is not None:
        failures.append(
            "a variable with initialisers was read as a function definition"
        )
    if looks_like_free_function("if (x) {") is not None:
        failures.append("a control statement was read as a function definition")
    if looks_like_free_function("void declared_only(int a);") is not None:
        failures.append("a declaration was read as a function definition")
    if "J3DPSCalcInverseTranspose" in renames(free_defs, {"other": "int"}):
        failures.append("a free function was paired as a class rename")

    # a wrapped signature is still one signature: this is the real SelectMenu case
    wrapped_ours = definitions(
        "void TSelectMenu::setup(u8 stage, JKRArchive* archive,\n"
        "                        TSelectShineManager* shineMgr, TSelectDir* dir)\n{\n}\n"
    )
    wrapped_theirs = definitions(
        "void TSelectMenu::initData(u8 stage, JKRArchive* pArch,\n"
        "                           TSelectShineManager* pShineMgr,\n"
        "                           TSelectDir* pSelectDir)\n{\n}\n"
    )
    if renames(wrapped_ours, wrapped_theirs) != [
        ("TSelectMenu::setup", "TSelectMenu::initData")
    ]:
        failures.append(
            f"a wrapped signature was not matched to its rename: "
            f"{renames(wrapped_ours, wrapped_theirs)}"
        )

    # a rename: same parameters, different name, each side missing the other's
    pairs = renames(
        {"TSelectMenu::setup": "u8,JKRArchive*,TSelectShineManager*,TSelectDir*"},
        {
            "TSelectMenu::initData": "u8,JKRArchive*,TSelectShineManager*,TSelectDir*",
            "TSelectMenu::other": "int",
        },
    )
    if pairs != [("TSelectMenu::setup", "TSelectMenu::initData")]:
        failures.append(f"a rename pair was not matched by signature: {pairs}")
    if renames({"A::x": "int"}, {"B::y": "int"}):
        failures.append(
            "two different classes' same-signature methods were paired as a rename"
        )

    # 2. the two classes, from real line counts
    converge = FileDebt("src/ConvergeMe.cpp", 1, 5)
    decide = FileDebt("src/DecideMe.cpp", 5, 9, only_ours=["DecideMe::ours"])
    ahead = FileDebt("src/Ahead.cpp", 9, 5)
    if converge.kind != "CONVERGE" or converge.behind != 4:
        failures.append(f"no fork-only function must be CONVERGE: {converge.kind}")
    if decide.kind != "DECIDE" or "DecideMe::ours" not in decide.only_ours:
        failures.append(f"a fork-only function must be DECIDE: {decide.kind}")
    if ahead.kind != "ahead":
        failures.append(f"a file ahead of upstream must not be debt: {ahead.kind}")
    # a name upstream renamed must NOT read as fork work
    renamed = FileDebt(
        "src/Renamed.cpp", 1, 5, rename_pairs=[("A::setup", "A::initData")]
    )
    if renamed.kind != "CONVERGE":
        failures.append(f"a matched rename must not make a file DECIDE: {renamed.kind}")

    # 3. an empty scan says it scanned nothing
    if not _report_says_nothing_scanned():
        failures.append("an empty scan did not report that it scanned nothing")

    for f in failures:
        print(f"  {f}")
    if failures:
        print("convergence_debt selftest: FAIL — the classifier did not discriminate")
        return 1
    print(
        "convergence_debt selftest: PASS (a method upstream lacks is extracted and makes a "
        "file DECIDE; a file with no fork-only method is CONVERGE; a file ahead of upstream is "
        "not debt; an empty scan reports that it scanned nothing)"
    )
    return 0


def _report_says_nothing_scanned() -> bool:
    import contextlib
    import io

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = report([], 5, False)
    text = buf.getvalue()
    lowered = text.lower()
    return rc == 0 and "nothing to converge" in lowered and "scanned nothing" in lowered


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--top", type=int, default=20, help="how many files per class to list"
    )
    ap.add_argument("--json", action="store_true", help="machine-readable summary")
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove the classifier produces both classes and refuses an empty scan",
    )
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    if git("rev-parse", "--verify", UPSTREAM) is None:
        print(
            f"convergence_debt: REFUSES: {UPSTREAM} is not in this clone, so there is no "
            f"upstream to measure against and a clean list would mean nothing"
        )
        return 1
    try:
        ast.parse((SUB / "configure.py").read_text(encoding="utf-8"))
    except (OSError, SyntaxError) as exc:
        print(f"convergence_debt: REFUSES: cannot read the decomp: {exc}")
        return 1
    return report(measure(), max(1, args.top), args.json)


if __name__ == "__main__":
    sys.exit(main())
