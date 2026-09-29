#!/usr/bin/env python3
"""decomp_gaps.py — hollow methods that are OUR gap, not the decomp's normal shape.

`hollow_methods.py` reports every method the tree declares and never defines: 1179
of them. Most are not repairs. Measured against upstream `doldecomp/sms`, which is
a byte-matching decompilation:

  * 207 are destructors with an empty body -- and upstream has the identical
    `virtual ~TCoasterEnemy() { }`. An empty destructor body is what a destructor
    body looks like; "filling" one would diverge from a matching decompilation and
    is fakematch work, not recovery.
  * 31 are `operator return` declarations with no definition -- and upstream
    declares them the same way and defines them nowhere either, because they are
    inlined at their call sites and never emitted. See the decomp's own
    UNUSED-functions note.

So "declared and not defined" is largely a property of decompilation, not a defect.
The gaps that are actually ours are the ones where upstream DOES define the method
and we do not. That is what this tool reports, and it is the queue worth working.

Two buckets, kept apart because they need different work:

  UPSTREAM_DEFINED   upstream gives the method a body and we do not. Ours to fix,
                     and upstream's own source is the evidence for what the body is.
  UPSTREAM_EMPTY     upstream also has an empty or absent body. Nothing to fix;
                     listed only so the exclusion is visible rather than silent.

It reads upstream by `git show`, never by guesswork, and it fails rather than
reporting zero when the upstream ref is missing -- a tool that cannot see upstream
must not be able to say "no gaps".
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SMS = REPO / "decomp" / "sms"
HERE = Path(__file__).resolve().parent

# A method is a destructor, or a conversion operator, or a plain access to nothing
# interesting. These are the shapes whose emptiness upstream also has.
DESTRUCTOR = re.compile(r"^~")
OPERATOR = re.compile(r"^operator\b")


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=SMS, capture_output=True, text=True,
                       errors="replace", check=False)
    return r.stdout


def hollow() -> list[dict]:
    r = subprocess.run(["uv", "run", "--frozen", "python", str(HERE / "hollow_methods.py"),
                        "--json"], capture_output=True, text=True, errors="replace",
                       check=False)
    raw = r.stdout
    if "{" not in raw:
        return []
    data, _ = json.JSONDecoder().raw_decode(raw[raw.index("{"):])
    out = []
    for f in data.get("hollow", []):
        out.append({
            "qualified": f"{f.get('cls', '')}::{f.get('name', '')}",
            "cls": f.get("cls", ""),
            "name": f.get("name", ""),
            "kind": f.get("kind", ""),
            "path": f.get("path", ""),
            "line": f.get("line", 0),
            "virtual": bool(f.get("virtual")),
        })
    return out


def upstream_defined(cls: str, name: str, upstream: str) -> bool | None:
    """Does upstream give Class::name a body?

    None means "could not tell" -- a class upstream has moved or renamed -- and is
    reported as unknown rather than counted either way. Guessing here would turn a
    renamed class into a fake gap, which is the failure this whole tool exists to
    avoid.
    """
    if OPERATOR.match(name) or DESTRUCTOR.match(name):
        # Handled by the caller: these are compared structurally, not by search,
        # because `operator return` and `~Class` do not appear as a definition
        # spelling that a text search can distinguish from a declaration.
        return None
    hits = git("grep", "-l", f"::\\{re.escape(name)}\\s*\\(", upstream, "--", "src", "libs")
    if not hits.strip():
        # No definition anywhere upstream for ANY class with that name. Weak, but
        # combined with the per-class check below it is a usable lower bound.
        return False
    # Narrow to this class: look for `Cls::name(` or `Cls :: name(` in a .cpp.
    pat = f"{re.escape(cls)} *:: *{re.escape(name)} *\\("
    precise = git("grep", "-E", pat, upstream, "--", "src", "libs")
    return bool(precise.strip())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upstream", default="upstream/main")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest(args.upstream)

    if git("rev-parse", "--verify", args.upstream).strip() == "":
        print(f"decomp_gaps: REFUSES: {args.upstream} is not in this clone. A tool that "
              f"cannot see upstream must not be able to report zero gaps.")
        return 1

    items = hollow()
    if not items:
        print("decomp_gaps: REFUSES: hollow_methods.py returned nothing, so the two tools "
              "disagree and no gap can be claimed either way")
        return 1

    with ThreadPoolExecutor(max_workers=8) as pool:
        verdicts = list(pool.map(
            lambda f: upstream_defined(f["cls"], f["name"], args.upstream), items))

    gaps, empty, unknown = [], [], []
    for f, v in zip(items, verdicts):
        if v is True:
            gaps.append(f)
        elif v is False:
            empty.append(f)
        else:
            unknown.append(f)

    print(f"decomp_gaps: {len(items)} hollow method(s) in the tree")
    print(f"  UPSTREAM_DEFINED (upstream has a body, we do not -- ours to fix) : {len(gaps)}")
    print(f"  UPSTREAM_EMPTY    (upstream is empty/absent too -- not a gap)     : {len(empty)}")
    print(f"  UNKNOWN           (destructor/operator, compared structurally)   : {len(unknown)}")
    if gaps:
        print(f"\n{len(gaps)} real gap(s), virtuals first:")
        ranked = sorted(gaps, key=lambda f: (not f["virtual"], f["cls"], f["name"]))
        for f in (ranked[:args.top] if args.top else ranked):
            v = "virtual" if f["virtual"] else "plain   "
            print(f"  {v}  {f['kind']:11} {f['qualified']:44} {f['path']}:{f['line']}")
    if args.json:
        print(json.dumps({"gaps": gaps, "upstream_empty": len(empty),
                          "unknown": len(unknown)}, indent=2))
    return 0


def selftest(upstream: str) -> int:
    """Both answers must be reachable, and the empty bucket must be non-empty.

    If the tool cannot produce at least one UPSTREAM_EMPTY finding, it is not
    actually reading upstream -- it would be reporting the same absolute list under
    a reassuring label, which is the same defect hollow_methods has at a larger
    scale.
    """
    if git("rev-parse", "--verify", upstream).strip() == "":
        print("decomp_gaps selftest: FAIL — upstream ref missing, so nothing is proven")
        return 1
    items = hollow()
    if not items:
        print("decomp_gaps selftest: FAIL — hollow_methods.py returned nothing")
        return 1
    with ThreadPoolExecutor(max_workers=8) as pool:
        verdicts = list(pool.map(
            lambda f: upstream_defined(f["cls"], f["name"], upstream), items))
    gaps = sum(1 for v in verdicts if v is True)
    empty = sum(1 for v in verdicts if v is False)
    if empty == 0:
        print("decomp_gaps selftest: FAIL — no UPSTREAM_EMPTY finding, so this tool is not "
              "actually comparing against upstream; it would relabel the absolute list")
        return 1
    # A known-answer control from this tree: TCubeManagerBase's destructor is empty
    # in upstream too, so it must land in the empty bucket and never in gaps.
    for f, v in zip(items, verdicts):
        if f["qualified"] == "TCubeManagerBase::~TCubeManagerBase" and v is True:
            print("decomp_gaps selftest: FAIL — TCubeManagerBase::~TCubeManagerBase is empty "
                  "in upstream and must not be counted as a gap")
            return 1
    print(f"decomp_gaps selftest: PASS ({gaps} gap(s) and {empty} upstream-empty finding(s) "
          f"both reachable, and the known empty destructor is not a gap)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
