#!/usr/bin/env python3
"""Reject reintroduction of Sunbright's retired execution surfaces."""

from __future__ import annotations

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
    # "extern/dolphin/" bans reviving a direct top-level Dolphin path the retired static-recomp
    # product once used. It must not also ban the unrelated, sanctioned "extern/gcnport/extern/
    # dolphin/" path: that is gcnport's OWN pinned fork, nested three levels deep inside the new
    # shared runtime framework, not a revival of the old top-level path.
    scrubbed = folded.replace("gcnport/extern/dolphin/", "")
    return [
        Finding(relative, retired_reason(needle))
        for needle in CONTENT_NEEDLES
        if needle in (scrubbed if needle == "extern/dolphin/" else folded)
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


if __name__ == "__main__":
    raise SystemExit(check())
