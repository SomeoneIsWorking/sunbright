#!/usr/bin/env python3
"""Check that source paths named in the LIVE docs actually exist.

WHY. On 2026-08-12 a sweep of `docs/60fps/` — the document a session reads to find the 60fps code —
found nine paths that resolved to nothing. Some were under-qualified (written relative to a root
that is not this repo's), some named files deleted months earlier, and one paragraph described a
tool as DONE, with a switch and a filename, where neither the switch nor the file exists anywhere.
A reader following any of them finds nothing and cannot tell which case they are in.

The project already gates env-switch names in CLAUDE.md, `docs/` and run scripts
(`tools/diag_registry.py`). Nothing gated PATHS, which is how `tools/oracle/capture.sh` came to
look for its Dolphin binary in an uninitialised submodule and silently fall back to a stock build
for months. This closes that.

WHAT IT DELIBERATELY DOES NOT CHECK.

  * **Archive directories.** `docs/re_notes/` and `docs/port/` are historical RE notes describing a
    past state; a dead path there is the note doing its job, exactly as a retired instrument should
    still name the file it lived in. They are SKIPPED, and the skip is REPORTED — a check that
    quietly narrows its own scope is how "clean" stops meaning anything.
  * **Paths marked dead in place.** A live document may legitimately name a deleted file when the
    point is the history — `docs/60fps/README.md` keeps the losing rows of a three-way design
    comparison, because deleting them would delete the reasoning. Such a path is exempt when the
    same line marks it: `(DELETED)`, `— DELETED`, `is GONE`, `no longer exists`, `since deleted`.
    The marker is what makes it honest, so the marker is what grants the exemption.
  * **Whether a path that exists is the RIGHT one.** It cannot know that. It says so.

  doc_paths.py              # check; exit 1 if a live doc names a missing path
  doc_paths.py --selftest   # prove it fires, and prove each exemption works
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Documents that describe the code as it IS. Everything else under docs/ is archive.
LIVE_ROOTS = ("AGENTS.md", "CLAUDE.md", "docs/README.md", "docs/codemap.md", "docs/app",
              "docs/60fps", "docs/graphics", "docs/audio", "docs/info")
ARCHIVE = ("docs/re_notes", "docs/port", "debug_journal")

PATH_RE = re.compile(r"`([A-Za-z0-9_][\w./-]*\.(?:cpp|h|hpp|py|sh|glsl|tsv))`")

# A section header that marks history: everything after it is describing the past.
HISTORY_RE = re.compile(
    r"^>?\s*#{1,6}\s+(path[s]? (?:in this document|corrected)|retired|superseded|history|"
    r"provenance|was|previously)\b", re.IGNORECASE | re.MULTILINE)

# Inline "this is dead, or lives elsewhere, and I am saying so" markers, checked on the path's own
# line. DELETED and GONE are matched CASE-SENSITIVELY, in capitals: the exemption should cost a
# deliberate act, and a lowercase "deleted" in flowing prose should not silently excuse a path.
#
# The second alternative covers paths that are real but belong to ANOTHER repository — Dolphin's
# UCodes/Zelda.cpp, for instance. Naming one is not rot as long as the document says whose it is;
# what misleads is a foreign path presented as if it were ours.
DEAD_INLINE = re.compile(
    r"\bDELETED\b|\bGONE\b"
    r"|no longer exists?"
    r"|not (?:in )?this repo|in the Dolphin fork|in the fork\b|another repo")


def live_docs() -> list[Path]:
    out: list[Path] = []
    for root in LIVE_ROOTS:
        p = REPO / root
        if p.is_file():
            out.append(p)
        elif p.is_dir():
            out.extend(sorted(f for f in p.rglob("*.md")))
    return [f for f in out if not any(str(f.relative_to(REPO)).startswith(a) for a in ARCHIVE)]


def scan(text: str) -> list[str]:
    """Missing paths in the live portion of a document, excluding ones marked dead in place."""
    m = HISTORY_RE.search(text)
    live = text[: m.start()] if m else text
    missing = []
    for line in live.splitlines():
        if DEAD_INLINE.search(line):
            continue
        for hit in PATH_RE.findall(line):
            if "/" not in hit:
                continue
            if not (REPO / hit).exists():
                missing.append(hit)
    return missing


def check() -> int:
    docs = live_docs()
    bad: dict[str, list[str]] = {}
    for f in docs:
        miss = scan(f.read_text(errors="replace"))
        if miss:
            bad[str(f.relative_to(REPO))] = sorted(set(miss))

    for d, miss in sorted(bad.items()):
        print(f"  {d}")
        for m in miss:
            print(f"      {m}")

    print(f"\ndoc_paths: scanned {len(docs)} live document(s).")
    print(f"  documents naming a missing path .... {len(bad)}")
    print(f"  SKIPPED as archive (dead paths there are the note doing its job): "
          f"{', '.join(ARCHIVE)}")
    print("  NOT COVERED: whether a path that exists is the RIGHT one, paths written without "
          "backticks, and anything outside the live roots above.")
    if bad:
        print("\n  Fix by qualifying the path, by locating where the code MOVED (find it by its "
              "hook address or a symbol it defines, not by guessing a rename), or — when it is "
              "genuinely gone and the history is the point — by marking it (DELETED) on the same "
              "line, which exempts it.")
    return 1 if bad else 0


def selftest() -> int:
    ok = True

    cases = [
        ("a missing path is caught",
         "See `tools/does_not_exist_xyz.py` for details.\n", ["tools/does_not_exist_xyz.py"]),
        ("an existing path is not flagged",
         "See `tools/docs/doc_paths.py` for details.\n", []),
        ("a path marked (DELETED) in place is exempt",
         "`overrides/gone.cpp` (DELETED) held the old design.\n", []),
        ("a path in an 'is GONE' sentence is exempt",
         "That file `overrides/gone.cpp` is GONE — the replay moved.\n", []),
        ("a (DELETED; recoverable from git) marker is exempt",
         "`runtime/gone.h` (DELETED; recoverable from git at `9283f44^`)\n", []),
        ("a trailing ', DELETED)' marker is exempt",
         "`a/b.h` (formerly `overrides/gone.cpp`, DELETED)\n", []),
        ("a path declared to live in another repo is exempt",
         "`UCodes/Zelda.cpp` (in the Dolphin fork, not this repo)\n", []),
        ("a LOWERCASE 'deleted' does NOT exempt — the marker must be deliberate",
         "The file `tools/does_not_exist_xyz.py` was deleted at some point.\n",
         ["tools/does_not_exist_xyz.py"]),
        ("a missing path under a history header is exempt",
         "Live text.\n\n## Paths in this document\n\nWas `overrides/gone.cpp`.\n", []),
        ("a bare filename with no directory is ignored",
         "See `main.cpp`.\n", []),
    ]
    for name, text, expect in cases:
        got = scan(text)
        if got == expect:
            print(f"  PASS  {name}")
        else:
            print(f"  FAIL  {name}: got {got}, expected {expect}")
            ok = False

    if not live_docs():
        print("  FAIL  live_docs() found NOTHING to scan — a check with no corpus reports clean "
              "for the wrong reason")
        ok = False
    else:
        print(f"  PASS  live corpus is non-empty ({len(live_docs())} document(s))")

    print("selftest:", "all checks passed" if ok else "FAILURES ABOVE")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    sys.exit(selftest() if a.selftest else check())
