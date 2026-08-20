#!/usr/bin/env python3
"""Ratchet first-party source-file size at the host application boundary.

The default limit is 1,200 lines. Existing files above that limit are explicit legacy ratchets:
they may shrink but never grow, and files at 2,000+ lines remain critical extraction territory.

  tools/structure_check.py             check the real tree
  tools/structure_check.py --selftest  prove over-limit and boundary cases disagree
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOT_LIMITS = {
    "sms-boot": 1200,
    "sms-recomp": 1200,
    "tools/recompiler": 1200,
}
EXCLUDED_ROOTS = {
    "sms-recomp/generated",
    "sms-recomp/runtime/shaders",
}
FILE_LIMITS = {
    "sms-recomp/frame_interp/subframe_legacy.cpp": 2951,
    "sms-recomp/runtime/devices/dev_gxfifo.cpp": 1968,
    "sms-recomp/runtime/render/native_render.cpp": 1708,
    "sms-recomp/runtime/render/scene.cpp": 1210,
}


def source_files() -> dict[str, int]:
    measured: dict[str, int] = {}
    for relative, limit in ROOT_LIMITS.items():
        root = REPO / relative
        if not root.is_dir():
            measured[relative + "/<MISSING>"] = limit + 1
            continue
        for path in sorted(root.rglob("*")):
            relative_path = str(path.relative_to(REPO))
            if any(
                relative_path == excluded or relative_path.startswith(excluded + "/")
                for excluded in EXCLUDED_ROOTS
            ):
                continue
            if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                measured[relative_path] = len(path.read_text(errors="replace").splitlines())
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
        "sms-recomp/ui/boundary.cpp": 1200,
        "sms-recomp/app/too_large.cpp": 1201,
        "sms-recomp/frame_interp/subframe_legacy.cpp": 2951,
        "sms-recomp/runtime/devices/dev_gxfifo.cpp": 1969,
    }
    got = violations(sample)
    expected = [
        ("sms-recomp/app/too_large.cpp", 1201, 1200),
        ("sms-recomp/runtime/devices/dev_gxfifo.cpp", 1969, 1968),
    ]
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
