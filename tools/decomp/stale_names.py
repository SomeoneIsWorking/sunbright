#!/usr/bin/env python3
"""stale_names.py — names this fork still uses that upstream no longer has.

The 2026-09-28 upstream sync found one specific shape of breakage twice, in two
different files: upstream renames a member, the text merge applies the rename to
the declaration but not to every use-site, and the result is code that references
a member the class no longer declares. It compiles nowhere, and because nothing in
this repository compiles decomp/sms, nothing noticed for a month.

TMarDirector::unk7D is the known instance. The question this tool answers is how
many more there are, and it answers it without guessing:

  STALE   the name is used in this fork and appears NOWHERE in upstream. Either a
          rename was applied to the declaration but not the use-sites, or this
          fork renamed something and missed its callers.
  LOCAL   the name is used here and upstream has it too, so it is a placeholder
          both sides still share -- genuinely unnamed, not a missed rename.

The distinction matters because the two need different work. A STALE name has a
known answer available (upstream renamed it; find the new name). A LOCAL name
needs evidence that does not exist yet.

Names are read from the decomp's own text, and "used" is distinguished from
"declared" so a file that legitimately defines a placeholder is not reported.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SMS = REPO / "decomp" / "sms"

# The decomp's placeholder convention: `unkNNN` for a field whose real name is
# unknown, where NNN is a hex byte offset. Matching the convention rather than any
# identifier keeps the tool from reporting every parameter name in the tree.
#
# The group is NON-capturing on purpose. With a capturing group, findall() returns
# the hex digits alone ("10") rather than the whole name ("unk10"), so comparing
# it against names collected via match().group(0) can never intersect and every
# name reports as stale. That bug made the first run claim 621 of 621 stale and 0
# shared, when upstream in fact contains 614 of the same names.
PLACEHOLDER = re.compile(r"\bunk[0-9A-Fa-f]{2,4}\b")

# Comments are prose, not uses. Without this the tool reported a name as STALE purely
# because a comment said "renamed from unk19c" -- which is exactly the sentence that
# RECORDS a completed rename. The first run after the rename landed still listed
# unk19c, and the only occurrence left in the tree was that comment.
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def code_only(text: str) -> str:
    """The source with comments and string literals removed.

    Order matters: strings are removed before line comments so a `//` inside a string
    literal cannot swallow the rest of the file, and block comments go first so a `//`
    inside a block comment does not confuse the line pass.
    """
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub(" ", text)
    return _STRING.sub(" \"\" ", text)

SOURCE_DIRS = ("src", "include", "libs")


def git(*args: str) -> str:
    r = subprocess.run(
        ["git", *args],
        cwd=SMS,
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    return r.stdout


def tracked(dirs: tuple[str, ...]) -> list[str]:
    return [f for f in git("ls-files", *dirs).splitlines() if f.strip()]


def classify(ref_ours: str, ref_upstream: str) -> tuple[dict, dict, int]:
    """Split placeholder names by whether the upstream ref still has them."""
    files = tracked(SOURCE_DIRS)
    ours_names: dict[str, list[str]] = defaultdict(list)
    upstream_names: set[str] = set()
    for path in files:
        text = git("cat-file", "blob", f"{ref_ours}:{path}")
        for m in PLACEHOLDER.finditer(code_only(text or "")):
            ours_names[m.group(0)].append(path)
    for path in git(
        "ls-tree", "-r", "--name-only", ref_upstream, *SOURCE_DIRS
    ).splitlines():
        if not path.strip():
            continue
        text = git("cat-file", "blob", f"{ref_upstream}:{path.strip()}")
        upstream_names.update(PLACEHOLDER.findall(code_only(text or "")))
    stale = {n: p for n, p in ours_names.items() if n not in upstream_names}
    shared = {n: p for n, p in ours_names.items() if n in upstream_names}
    return stale, shared, len(files)


def selftest() -> int:
    """Both answers must be reachable, discovered rather than pinned.

    This used to assert that the literal name `unk19c` was STALE and `unk10` was
    LOCAL. The first of those was renamed away by the very work this tool exists to
    support, which made the self-test fail on a correct tree -- the same brittleness
    that made hostcheck's control TU a liability. So the control is now: both buckets
    are non-empty, and at least one name lands in each. A tool that could not read
    upstream would produce an empty STALE bucket and fail here rather than reporting a
    confident zero.
    """
    stale, shared, nfiles = classify("main", "upstream/main")
    if nfiles == 0:
        print("stale_names selftest: FAIL — no files scanned, so nothing is proven")
        return 1
    if not stale:
        print("stale_names selftest: FAIL — the STALE bucket is empty, so either upstream "
              "is not being read or the comparison is broken; both must be true for a "
              "confident zero to mean anything")
        return 1
    if not shared:
        print("stale_names selftest: FAIL — the LOCAL bucket is empty, so the two sides are "
              "not being compared like for like")
        return 1
    print(f"stale_names selftest: PASS (both buckets reachable: {len(stale)} stale / "
          f"{len(shared)} shared over {nfiles} files, e.g. stale={min(stale)!r}, "
          f"local={min(shared)!r})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--ours", default="main", help="ref to read this fork from")
    ap.add_argument("--upstream", default="upstream/main")
    ap.add_argument(
        "--limit", type=int, default=0, help="only report the N most-used stale names"
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="prove both answers are reachable, with known names from this tree",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    files = tracked(SOURCE_DIRS)
    if not files:
        print(
            "stale_names: REFUSES: no decomp source files found; an empty scan is not "
            "a clean result"
        )
        return 1

    # Which placeholder names does each side contain at all?
    ours_names: dict[str, list[str]] = defaultdict(list)
    upstream_names: set[str] = set()
    for path in files:
        text = git("cat-file", "blob", f"{args.ours}:{path}")
        for m in PLACEHOLDER.finditer(code_only(text or "")):
            ours_names[m.group(0)].append(path)
    for path in git(
        "ls-tree", "-r", "--name-only", args.upstream, *SOURCE_DIRS
    ).splitlines():
        if not path.strip():
            continue
        text = git("cat-file", "blob", f"{args.upstream}:{path.strip()}")
        upstream_names.update(PLACEHOLDER.findall(code_only(text or "")))

    stale = {n: p for n, p in ours_names.items() if n not in upstream_names}
    shared = {n: p for n, p in ours_names.items() if n in upstream_names}

    print(f"stale_names: scanned {len(files)} file(s)")
    print(f"  distinct placeholder names used here : {len(ours_names)}")
    print(f"  STALE  (upstream has none of them)     : {len(stale)}")
    print(f"  LOCAL  (upstream still uses them too)  : {len(shared)}")
    if stale:
        print(
            "\nSTALE names, most widely used first. A stale name has THREE causes, and "
            "this predicate cannot tell them apart because a fork-local field satisfies "
            "\"upstream has it nowhere\" by construction:"
        )
        print("  * a rename upstream applied to a declaration but not to its use-sites "
              "-- follow the upstream commit that renamed it;")
        print("  * a rename this fork applied to a declaration but not to its callers;")
        print("  * a field that only ever existed on this side, which needs the retail "
              "image to name or is genuinely unnamed.")
        print("Read the class's own declaration first -- every field carries its offset, "
              "and renaming a fork-local field to whatever upstream calls at a DIFFERENT "
              "offset makes the placeholder convention lie about the layout.\n")
        ranked = sorted(stale.items(), key=lambda kv: (-len(set(kv[1])), kv[0]))
        for name, paths in ranked[: args.limit] if args.limit else ranked:
            uniq = sorted(set(paths))
            shown = ", ".join(uniq[:4]) + (
                f" (+{len(uniq) - 4} more)" if len(uniq) > 4 else ""
            )
            print(f"  {name:10} {len(uniq):4} file(s)  {shown}")
    print(
        "\nA STALE name is not automatically a defect: a placeholder can also have been "
        "renamed on this side alone, in which case the new name is the answer and the "
        "old one should disappear. Each one wants its class declaration read before "
        "anything is changed."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
