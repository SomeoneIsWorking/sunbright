#!/usr/bin/env python3
"""Ratchet source-file size at the host application boundary.

The new Dusklight-shaped app/ and ui/ modules must remain cohesive modules, not become replacement
god files. The host entry point is included because its only job is composition.

  tools/structure_check.py             check the real tree
  tools/structure_check.py --selftest  prove over-limit and boundary cases disagree
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOT_LIMITS = {
    "sms-recomp/app": 800,
    "sms-recomp/host": 800,
    "sms-recomp/ui": 800,
}
FILE_LIMITS = {}


def source_files() -> dict[str, int]:
    measured: dict[str, int] = {}
    for relative, limit in ROOT_LIMITS.items():
        root = REPO / relative
        if not root.is_dir():
            measured[relative + "/<MISSING>"] = limit + 1
            continue
        for path in sorted(root.iterdir()):
            if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                measured[str(path.relative_to(REPO))] = len(path.read_text(errors="replace").splitlines())
    for relative in FILE_LIMITS:
        path = REPO / relative
        measured[relative] = len(path.read_text(errors="replace").splitlines()) if path.is_file() else -1
    return measured


def limit_for(path: str) -> int | None:
    if path in FILE_LIMITS:
        return FILE_LIMITS[path]
    for root, limit in ROOT_LIMITS.items():
        if path.startswith(root + "/"):
            return limit
    return None


def violations(measured: dict[str, int]) -> list[tuple[str, int, int]]:
    bad = []
    for path, lines in measured.items():
        limit = limit_for(path)
        if limit is not None and (lines < 0 or lines > limit):
            bad.append((path, lines, limit))
    return sorted(bad)


def check() -> int:
    measured = source_files()
    bad = violations(measured)
    for path, lines, limit in bad:
        actual = "missing" if lines < 0 else f"{lines} lines"
        print(f"structure: {path}: {actual}, limit {limit}")
    print(f"structure: measured {len(measured)} source files; {len(bad)} violation(s)")
    return 1 if bad else 0


def selftest() -> int:
    sample = {
        "sms-recomp/ui/boundary.cpp": 800,
        "sms-recomp/app/too_large.cpp": 801,
        "sms-recomp/host/main.cpp": 800,
    }
    got = violations(sample)
    expected = [("sms-recomp/app/too_large.cpp", 801, 800)]
    if got != expected:
        print(f"FAIL: got {got}, expected {expected}")
        return 1
    if not source_files():
        print("FAIL: real-tree discovery measured no files")
        return 1
    print("PASS: the boundary passes, one extra line fails, and real-tree discovery is non-empty")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    sys.exit(selftest() if args.selftest else check())
