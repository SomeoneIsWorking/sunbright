#!/usr/bin/env python3
"""Reject reintroduction of Sunbright's retired execution surfaces."""

from __future__ import annotations

import argparse
import subprocess
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RETIRED_ROOTS = ("sms-" + "recomp/", "tools/" + "re" + "compiler/")
RETIRED_FILES = {
    "play.sh",
    "run-decomp.sh",
    "run-" + "recomp.sh",
    "run-render.sh",
}
CONTENT_NEEDLES = (
    "sms-" + "recomp",
    "re" + "compiler",
    "run-" + "recomp.sh",
    "sunbright-" + "recomp",
    "static " + "recomp",
    "static-" + "recomp",
    "static product",
    "offline-" + "translat",
    "offline " + "translat",
    "emitted " + "guest",
    "generated " + "guest",
    "generated/functions" + ".h",
    "recomp" + "_raw",
    "call_" + "ppc",
    "dolphin_" + "hook.cpp",
    "disable_" + "recomp",
    "recomp " + "run",
    "recomp-" + "era",
    "recomp " + "runtime",
    "recomp-" + "gx",
    "recompiled " + "product",
    "recompiled " + "status",
    "recompiled " + "function",
    "extern/dolphin/",
    "xenon" + "recomp",
    "/" + "tmp/",
    "dus" + "klight",
)


@dataclass(frozen=True)
class Finding:
    path: str
    reason: str


def tracked_paths(root: Path = REPO) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return sorted(path for path in result.stdout.splitlines() if path)


def path_finding(relative: str) -> Finding | None:
    if any(relative.startswith(prefix) for prefix in RETIRED_ROOTS):
        return Finding(relative, "retired execution root")
    if relative in RETIRED_FILES:
        return Finding(relative, "retired launcher")
    if relative.endswith(".sh") and relative != "run.sh":
        return Finding(relative, "non-Python project script")
    return None


def content_findings(relative: str, source: str) -> list[Finding]:
    folded = source.casefold()
    return [
        Finding(relative, retired_reason(needle))
        for needle in CONTENT_NEEDLES
        if needle in folded
    ]


def retired_reason(needle: str) -> str:
    return f"retired selector/reference {needle!r}"


def read_text(path: Path) -> str | None:
    payload = path.read_bytes()
    if b"\0" in payload:
        return None
    return payload.decode(errors="replace")


def inspect(paths: list[str], root: Path = REPO) -> list[Finding]:
    findings: list[Finding] = []
    for relative in paths:
        path = root / relative
        if not path.exists():
            continue
        path_issue = path_finding(relative)
        if path_issue is not None:
            findings.append(path_issue)
            continue
        if relative.startswith(("extern/", "decomp/sms/", "scratch/", "build/")):
            continue
        if not path.is_file() or relative == "tools/migration_boundary.py":
            continue
        source = read_text(path)
        if source is not None:
            findings.extend(content_findings(relative, source))
    return sorted(set(findings), key=lambda item: (item.path, item.reason))


def check() -> int:
    paths = tracked_paths()
    findings = inspect(paths)
    for finding in findings:
        print(f"migration-boundary: {finding.path}: {finding.reason}")
    print(
        f"migration-boundary: scanned {len(paths)} first-party paths; {len(findings)} violation(s)"
    )
    return 1 if findings else 0


def selftest() -> int:
    got = [
        issue
        for relative in (
            "run.sh",
            "tools/healthy.py",
            "tools/legacy.sh",
            "sms-" + "recomp/runtime.cpp",
        )
        if (issue := path_finding(relative)) is not None
    ]
    got.extend(content_findings("tools/config.py", 'cache = "/' + 'tmp/game"'))
    got.extend(content_findings("README.md", "Follow Dus" + "klight"))
    got.extend(
        content_findings(
            "docs/old.md",
            "A static "
            + "recomp used Xenon"
            + "Recomp, generated/functions"
            + ".h, call_"
            + "ppc, and DISABLE_"
            + "RECOMP",
        )
    )
    got.extend(content_findings("docs/oracle.md", "use extern/dolphin/build"))
    got.extend(
        content_findings(".claude/commands/old.md", "offline-" + "translated product")
    )
    got = sorted(got, key=lambda item: (item.path, item.reason))
    expected = [
        Finding(".claude/commands/old.md", retired_reason("offline-" + "translat")),
        Finding("README.md", retired_reason("dus" + "klight")),
        Finding("docs/old.md", retired_reason("call_" + "ppc")),
        Finding("docs/old.md", retired_reason("disable_" + "recomp")),
        Finding("docs/old.md", retired_reason("generated/functions" + ".h")),
        Finding("docs/old.md", retired_reason("static " + "recomp")),
        Finding("docs/old.md", retired_reason("xenon" + "recomp")),
        Finding("docs/oracle.md", retired_reason("extern/dolphin/")),
        Finding("sms-" + "recomp/runtime.cpp", "retired execution root"),
        Finding("tools/config.py", retired_reason("/" + "tmp/")),
        Finding("tools/legacy.sh", "non-Python project script"),
    ]
    if got != expected:
        print(f"FAIL: got {got}, expected {expected}")
        return 1
    print(
        "PASS: planted retired root, shell, vocabulary, temp path, and old guide rejected; "
        "controls accepted"
    )
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    raise SystemExit(selftest() if arguments.selftest else check())
