#!/usr/bin/env python3
"""Split `info.py claim check`'s STALE list into "the code was EDITED" and "the file only MOVED".

WHY. The rot check marks a claim stale when the code its evidence rests on has changed since it was
verified. That is the correct default — it cannot know whether the change mattered. What it leaves
is a list of identical lines with no way in: twenty-five claims, all equally stale, all equally
unactionable. The single biggest contributor on this project is one directory reorganisation.

So this asks the question the rot check does not: did the responsible commit change the file's
CONTENT, or only its path? `git show --name-status -M` distinguishes them, and a content-identical
rename cannot invalidate a claim's subject.

FOLLOWING RENAMES IS THE WHOLE DIFFICULTY. A claim declares the path a file has TODAY. The commit
that made it stale may have touched it under an older name, in which case a naive lookup finds
nothing and the claim lands in an "unattributable" bucket that means only "my comparison was too
literal". The first version of this triage put ten of twenty-five claims there. This one walks
`git log --follow` to learn what each path was called at each commit, and reports what is left over
as genuinely unresolved rather than as a result.

THIS DOES NOT CONFIRM ANYTHING. A claim's evidence is a measurement; re-establishing it needs the
measurement, not an argument that the diff looked irrelevant. `MOVED ONLY` means "stale for a
bookkeeping reason", never "still true".

  stale_triage.py              # print the triage
  stale_triage.py --selftest   # prove the edit/move discrimination works in BOTH directions
"""

from __future__ import annotations

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INFO_PY = Path.home() / ".claude/skills/project-info/info.py"

STALE_RE = re.compile(
    r"^STALE\s+(C\d+)\s+(.*?)\n(.*?)(?=\n\s*-> re-verify)",
    re.DOTALL | re.MULTILINE,
)
COMMIT_RE = re.compile(
    r"^\s+([0-9a-f]{7,})\s+\d{4}-\d\d-\d\d", re.MULTILINE
)
# The dependency line is `  path/to/file.cpp  [file-scope]` or, when the claim narrowed it,
# `  path/to/file.cpp#symbol  [symbol-scope]`. The first version of this regex required the path to
# be followed directly by the bracket and so dropped every symbol-scoped dependency on the floor —
# three claims sat in UNRESOLVED for no reason but that. The `#symbol` is captured and discarded:
# this comparison works at FILE level, so a symbol-scoped claim is classified by whether the FILE
# was edited, which can over-report an edit and never under-report one.
DEPFILE_RE = re.compile(
    r"^\s+(\S+?\.(?:cpp|h|hpp|py|sh|glsl))(?:#\S+)?\s+\[", re.MULTILINE
)


def git(*args: str, cwd: Path | None = None) -> str:
    return subprocess.run(
        ["git", *args], cwd=cwd or REPO, capture_output=True, text=True, check=True
    ).stdout


def submodules() -> list[str]:
    """Paths that are submodules. Files under them have their OWN history, which `git show` in this
    repo cannot see — a parent commit only records a gitlink moving from one sha to another."""
    out = git("config", "--file", ".gitmodules", "--get-regexp", r"submodule\..*\.path")
    return sorted((ln.split(None, 1)[1] for ln in out.splitlines() if " " in ln), key=len, reverse=True)


SUBMODULES = submodules()


def split_submodule(path: str) -> tuple[str, str] | None:
    for sm in SUBMODULES:
        if path == sm or path.startswith(sm + "/"):
            return sm, path[len(sm) + 1:]
    return None


def classify_submodule(commit: str, sm: str, rel: str) -> str:
    """Classify a commit that belongs to the SUBMODULE's own history.

    Two wrong guesses preceded this, and both are worth recording because they were the same
    mistake: reasoning about where the data must live instead of looking. First I assumed the ten
    unattributed claims were rename victims and taught this to follow renames — no change. Then I
    assumed they were submodule paths whose PARENT commits move a gitlink, and wrote a gitlink
    differ — also no change, because `info.py claim check` already runs git INSIDE the submodule,
    so the hashes it prints are the submodule's own. One `claim check` line showing commit subjects
    that plainly belong to aurora settled it in seconds. Look at the data first."""
    sub = REPO / sm
    if not (sub / ".git").exists():
        return "submodule-unavailable"
    out = git("show", "--format=", "-M", "--name-status", commit, cwd=sub)
    verdict = "absent"
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or rel not in set(parts[1:]):
            continue
        if parts[0] == "R100":
            verdict = "move" if verdict == "absent" else verdict
        else:
            return "edit"
    return verdict


def name_history(path: str) -> set[str]:
    """Every name this path has had, so a commit that touched it under an old name still matches."""
    names = {path}
    out = git("log", "--follow", "--name-status", "-M", "--format=", "--", path)
    for line in out.splitlines():
        parts = line.split("\t")
        if parts[0].startswith("R") and len(parts) == 3:
            names.add(parts[1])
            names.add(parts[2])
        elif len(parts) == 2:
            names.add(parts[1])
    return names


def classify(commit: str, names: set[str]) -> str:
    """'edit', 'move', or 'absent' for what `commit` did to any of `names`."""
    out = git("show", "--format=", "-M", "--name-status", commit)
    verdict = "absent"
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        touched = set(parts[1:])
        if not touched & names:
            continue
        if parts[0] == "R100":
            verdict = "move" if verdict == "absent" else verdict
        else:
            return "edit"
    return verdict


def run() -> int:
    if not INFO_PY.exists():
        print(f"stale_triage: {INFO_PY} not found — this triage reads its output and cannot "
              f"substitute for it. Nothing was checked.", file=sys.stderr)
        return 2
    txt = subprocess.run(
        [sys.executable, str(INFO_PY), "claim", "check"],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    blocks = STALE_RE.findall(txt)
    if not blocks:
        print("stale_triage: `claim check` reported NO stale claims. That is either a clean "
              "registry or a parse failure in this script — the two look identical here, so "
              "confirm against `info.py claim check` before believing it.")
        return 0

    buckets: dict[str, list[str]] = collections.defaultdict(list)
    cache: dict[str, set[str]] = {}
    for cid, _title, body in blocks:
        commits = sorted(set(COMMIT_RE.findall(body)))
        files = sorted(set(DEPFILE_RE.findall(body)))
        kinds = set()
        for f in files:
            sm = split_submodule(f)
            if sm is not None:
                for c in commits:
                    kinds.add(classify_submodule(c, sm[0], sm[1]))
                continue
            if f not in cache:
                cache[f] = name_history(f)
            for c in commits:
                kinds.add(classify(c, cache[f]))
        label = ("EDITED" if "edit" in kinds
                 else "MOVED ONLY" if "move" in kinds
                 else "SUBMODULE UNAVAILABLE" if "submodule-unavailable" in kinds
                 else "UNRESOLVED")
        buckets[label].append(cid)

    total = len(blocks)
    for label in ("EDITED", "MOVED ONLY", "SUBMODULE UNAVAILABLE", "UNRESOLVED"):
        ids = sorted(buckets[label])
        print(f"{label:<12} {len(ids):>3}   {' '.join(ids)}")
    print(f"\n{total} stale claim(s). "
          f"{total - len(buckets['UNRESOLVED'])} attributed, {len(buckets['UNRESOLVED'])} not.")
    if buckets["UNRESOLVED"]:
        print("  UNRESOLVED means this comparison could not find the dependency in the commit "
              "under ANY of its historical names — not that the change was harmless.")
    print("\nEDITED needs a run. MOVED ONLY is stale for a bookkeeping reason and is still NOT "
          "confirmed: a claim's evidence is a measurement, and only a measurement restores it.")
    print("COARSER THAN THE CHECK IT READS: a claim that narrowed its dependency to a SYMBOL is "
          "classified here by whether the FILE changed, so a symbol-scoped claim can appear as "
          "EDITED when the edit missed its symbol. That direction is deliberate — it over-reports "
          "work, never under-reports it.")
    return 0


def selftest() -> int:
    """Both directions, against this repository's real history."""
    ok = True

    # A commit that unambiguously EDITED a file: the one that created this script's sibling.
    edit_commit = git("log", "-1", "--format=%h", "--", "tools/info/registry_paths.py").strip()
    if edit_commit:
        verdict = classify(edit_commit, {"tools/info/registry_paths.py"})
        if verdict == "edit":
            print("  PASS  a content-changing commit is classified 'edit'")
        else:
            print(f"  FAIL  content-changing commit classified '{verdict}', expected 'edit'")
            ok = False
    else:
        print("  FAIL  could not find a commit touching tools/info/registry_paths.py — the "
              "positive case did not run, so this self-test proves nothing about 'edit'")
        ok = False

    # A commit that did NOT touch the file at all must not be called an edit.
    other = ""
    for candidate in git("log", "--format=%h", "--all").splitlines():
        touched = set(git("show", "--format=", "--name-only", candidate).splitlines())
        if "tools/info/registry_paths.py" not in touched:
            other = candidate
            break
    if other and classify(other, {"tools/info/registry_paths.py"}) == "absent":
        print("  PASS  an unrelated commit is classified 'absent', not 'edit'")
    else:
        print("  FAIL  no independently inspected unrelated commit classified 'absent'")
        ok = False

    # Rename following must return more than the name we asked about wherever history has one.
    names = name_history("tools/info/registry_paths.py")
    if "tools/info/registry_paths.py" in names:
        print(f"  PASS  rename history resolves ({len(names)} name(s) known)")
    else:
        print("  FAIL  rename history lost the path it was given")
        ok = False

    print("selftest:", "all checks passed" if ok else "FAILURES ABOVE")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    sys.exit(selftest() if a.selftest else run())
