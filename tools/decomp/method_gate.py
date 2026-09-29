#!/usr/bin/env python3
"""method_gate.py — the pass/fail a single decompiled-method repair must earn.

One command, both conditions, exit 0 only when both hold:

  1. the named method is no longer hollow -- it has a body that is not `{ }`; and
  2. the translation unit that declares it still compiles, in every build mode.

Both are needed. (1) alone accepts a body that does not compile, which is the
commonest way a hollow method gets "filled" with something that only looks like an
implementation. (2) alone accepts a method whose body was never written and whose
neighbours were broken to make the unit compile.

The name is a regex, because a class can overload and one repair may cover several
overloads. Pass the fully qualified `Class::method` as hollow_methods.py reports
it.

Usage:
    method_gate.py 'TBoxEel::~TBoxEel'        # exit 0 iff filled and still compiling
    method_gate.py --list 'TBoss'             # what currently matches, for building a job

It refuses when the regex matches nothing: a typo in a method name would otherwise
make a gate that can never fail, which is worse than no gate.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOLLOW = HERE / "hollow_methods.py"
HOSTCHECK = HERE / "hostcheck.py"


def findings() -> list[dict]:
    """Every hollow method, qualified as Class::name.

    hollow_methods.py reports the class and the identifier in separate fields and
    the identifier unqualified, so a constructor comes back as just the class
    name. The qualified form is what a regex over "a class's overloads" needs, and
    building it here keeps the gate independent of how that tool spells its output.
    """
    cmd = ["uv", "run", "--frozen", "python", str(HOLLOW), "--json"]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", check=False)
    raw = r.stdout
    if "{" not in raw:
        return []
    data, _ = json.JSONDecoder().raw_decode(raw[raw.index("{"):])
    out = []
    for f in data.get("hollow", []):
        out.append({
            "qualified": f"{f.get('cls', '')}::{f.get('name', '')}",
            "kind": f.get("kind", ""),
            "path": f.get("path", ""),
            "line": f.get("line", 0),
            "virtual": bool(f.get("virtual")),
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pattern", help="regex over the fully qualified method name")
    ap.add_argument("--list", action="store_true",
                    help="print what currently matches and exit 0 unconditionally")
    args = ap.parse_args()

    try:
        rx = re.compile(args.pattern)
    except re.error as exc:
        print(f"method_gate: REFUSES: {args.pattern!r} is not a valid regex: {exc}")
        return 1

    matched = [f for f in findings() if rx.search(f["qualified"])]
    if not matched:
        print(f"method_gate: REFUSES: {args.pattern!r} matches no declared method. A gate "
              f"that matches nothing can never fail, which is worse than no gate: check "
              f"the spelling against `hollow_methods.py --top`.")
        return 1

    if args.list:
        for f in matched:
            kind = f["kind"] + (" virtual" if f["virtual"] else "")
            print(f"{kind:18} {f['qualified']}  {f['path']}:{f['line']}")
        print(f"{len(matched)} match(es)")
        return 0

    still = [f for f in matched if f.get("kind") in ("NO BODY", "EMPTY BODY")]
    units = sorted({Path(f["path"]).stem for f in matched if f.get("path")})
    if still:
        print(f"method_gate: FAIL — {len(still)} of {len(matched)} matching method(s) still "
              f"hollow: {', '.join(f['qualified'] for f in still[:8])}")
        return 1

    # Now the no-regression half. hostcheck is told the specific units so the gate
    # stays fast; a full-tree run per job would serialise the whole swarm.
    cmd = ["uv", "run", "--frozen", "python", str(HOSTCHECK), "--mode", "both", "--jobs", "4"]
    for unit in units:
        cmd += ["--only", Path(unit).stem]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", check=False)
    if r.returncode != 0:
        print(f"method_gate: FAIL — method filled but its unit(s) no longer compile: {units}")
        for line in (r.stdout + r.stderr).splitlines():
            if "error:" in line:
                print(f"  {line.strip()[:140]}")
        return 1

    print(f"method_gate: PASS — {len(matched)} method(s) filled and unit(s) {units} still "
          f"compile in every build mode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
