#!/usr/bin/env python3
"""Find registry entries that point at source files this repo does not have.

WHY. `docs/info/instruments/` and `docs/info/claims/` exist so a later session consults them
INSTEAD of searching. That inverts the cost of a wrong path: an absent entry sends someone to grep,
while an entry marked `trusted` beside a file that is not there reads as "the check is gone", and
the check gets rebuilt from scratch or quietly skipped. Four entries were in that state on
2026-08-12 — two from a directory rename, two from a subsystem deleted months earlier.

WHAT IT WILL NOT DO. It does not flag a path that appears only in a section describing what an
entry USED to be. A retired instrument should name the file it lived in; a correction note should
name the path it was corrected from. Both are the entry doing its job. The first version of this
check had no such notion, immediately re-flagged the two entries it had just been used to fix, and
so would have taught its next reader to ignore it — which is the failure mode this whole registry
is meant to prevent.

The rule is positional, not lexical: paths are read from the part of an entry that says where the
instrument IS — everything before the first section header whose title marks history
(`## Path corrected`, `## Retired`, `## Superseded`, `## History`, `## Was`).

NEGATIVE OUTPUT. A clean run prints what it scanned and what it could not see, because "no dead
paths" and "I matched nothing" are otherwise the same line. Entries naming no source path at all
are reported separately: they are not clean, they are outside this check's reach.

  registry_paths.py              # check, exit 1 if any entry names a missing file
  registry_paths.py --selftest   # prove it fires: a planted dead path MUST be caught,
                                 #   and a dead path in a history section must NOT be
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGISTRIES = ("docs/info/instruments", "docs/info/claims")

# Section headers that mark an entry's own history. Paths under these are describing the past.
HISTORY_RE = re.compile(
    r"^##\s+(path corrected|retired|superseded|history|was|previously|corrected)\b",
    re.IGNORECASE | re.MULTILINE,
)

SRC_RE = re.compile(
    r"\b((?:tools|sms-boot|native-render|extern|tests|decomp|docs)/[\w./-]+"
    r"\.(?:cpp|h|hpp|py|sh|glsl|tsv|md))\b"
)

FIELD_RE = re.compile(r"^(id|status):\s*(\S+)", re.MULTILINE)

# Statuses that mean the entry is a RECORD of something dead. Such an entry SHOULD name the file it
# lived in — that is how a reader knows what was retired — so a missing path there is not a defect.
# Only entries someone would still act on are flagged; anything else trains the reader to ignore
# the output, which is the same failure as not checking at all.
DEAD_STATUS = {"distrusted", "superseded", "falsified", "retired", "dead", "obsolete"}


def live_part(text: str) -> str:
    """The portion of an entry that describes what the thing IS, not what it was."""
    m = HISTORY_RE.search(text)
    return text[: m.start()] if m else text


def scan_entry(text: str) -> tuple[str, str, list[str], list[str]]:
    fields = dict(FIELD_RE.findall(text))
    paths = sorted(set(SRC_RE.findall(live_part(text))))
    missing = [p for p in paths if not (REPO / p).exists()]
    return fields.get("id", "?"), fields.get("status", "?"), paths, missing


def check(verbose: bool = True) -> int:
    entries = 0
    pathless: list[str] = []
    dead_ok: list[str] = []
    bad: list[tuple[str, str, str, list[str]]] = []
    for reg in REGISTRIES:
        d = REPO / reg
        if not d.is_dir():
            print(f"registry_paths: {reg} DOES NOT EXIST — this check scanned nothing there, "
                  f"which is not the same as finding nothing wrong.", file=sys.stderr)
            return 2
        for f in sorted(d.glob("*.md")):
            entries += 1
            iid, status, paths, missing = scan_entry(f.read_text(errors="replace"))
            if not paths:
                pathless.append(f"{iid} ({f.name})")
            if missing and status.lower() not in DEAD_STATUS:
                bad.append((iid, status, f.name, missing))
            elif missing:
                dead_ok.append(iid)

    for iid, status, name, missing in bad:
        print(f"  {iid} [{status}]  names {len(missing)} missing file(s): {', '.join(missing)}"
              f"   ({name})")

    if verbose:
        print(f"\nregistry_paths: scanned {entries} entr(y/ies) across {', '.join(REGISTRIES)}.")
        print(f"  entries naming a missing source file .... {len(bad)}")
        print(f"  retired/superseded entries naming dead code (EXPECTED, not flagged) .... "
              f"{len(dead_ok)}{': ' + ', '.join(dead_ok) if dead_ok else ''}")
        print(f"  entries naming NO source path at all .... {len(pathless)}  <- OUTSIDE this "
              f"check; they are unchecked, not clean")
        if pathless:
            print(f"      {', '.join(pathless[:12])}{' ...' if len(pathless) > 12 else ''}")
        print("  NOT COVERED: whether a path that exists is the RIGHT one, whether the entry's "
              "claim is still true, and anything named only in prose without a file extension.")
    return 1 if bad else 0


def selftest() -> int:
    """Both classes. A check only ever run against the clean case proves nothing."""
    ok = True

    planted_live = (
        "---\nid: I999\nstatus: trusted\n---\n\n"
        "## What\n\nLives in tools/does_not_exist_xyz.py and is wonderful.\n"
    )
    _, _, _, missing = scan_entry(planted_live)
    if missing == ["tools/does_not_exist_xyz.py"]:
        print("  PASS  a dead path in the LIVE section is caught")
    else:
        print(f"  FAIL  a dead path in the LIVE section was NOT caught (missing={missing})")
        ok = False

    planted_history = (
        "---\nid: I998\nstatus: trusted\n---\n\n"
        "## What\n\nLives in tools/info/registry_paths.py.\n\n"
        "## Path corrected 2026-08-12\n\nRecorded as tools/does_not_exist_xyz.py, which moved.\n"
    )
    _, _, _, missing = scan_entry(planted_history)
    if missing == []:
        print("  PASS  a dead path under a history header is correctly IGNORED")
    else:
        print(f"  FAIL  a dead path under a history header was flagged (missing={missing}) — "
              f"this is the false positive that made the first version useless")
        ok = False

    planted_dead = (
        "---\nid: I996\nstatus: superseded\n---\n\n"
        "## What\n\nLived in tools/does_not_exist_xyz.py, which is gone.\n"
    )
    _, status, _, missing = scan_entry(planted_dead)
    if missing and status.lower() in DEAD_STATUS:
        print("  PASS  a superseded entry naming dead code is seen but NOT flagged")
    else:
        print(f"  FAIL  superseded-entry handling wrong (status={status}, missing={missing})")
        ok = False

    real = REPO / "tools/info/registry_paths.py"
    relative = real.relative_to(REPO).as_posix()
    _, _, paths, missing = scan_entry(
        f"---\nid: I997\n---\n\n## What\n\nSee {relative}.\n"
    )
    if paths and not missing:
        print("  PASS  an existing path is not flagged")
    else:
        print(f"  FAIL  an existing path was flagged or not seen (paths={paths}, missing={missing})")
        ok = False

    print("selftest:", "all checks passed" if ok else "FAILURES ABOVE")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    sys.exit(selftest() if a.selftest else check())
