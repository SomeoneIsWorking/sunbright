#!/usr/bin/env python3
"""selftest_all.py — run every diagnostic tool's own `--selftest` and fail if any of them does.

WHY THIS FILE EXISTS. Several tools in this repo carry a `--selftest` that feeds a case whose
answer is forced, so the tool can prove it is capable of the OTHER answer. Nothing ran them. A
self-test nobody runs is the same bug one level up: the instrument still reports a clean negative
when it is broken, and now with a reassuring `--selftest` in its docstring.

WHAT IT DOES. Discovers every *.py under tools/ that mentions `--selftest` in its argument
handling, reads its literal `SELFTEST_REQUIREMENTS` declaration without importing it, runs every
applicable test, and prints the selected, skipped, and discovered denominators. Discovery is by
SOURCE, not by a hardcoded list, so a new tool with a self-test is covered the day it lands and a
tool that LOSES its self-test shows up as a drop in the denominator rather than silently leaving
the suite. A tool without a declaration is portable and asset-free. Supported requirements are
`linux`, `macos`, `windows`, and `game-image`; unknown or non-literal declarations refuse the run.

THE NEGATIVE. Zero tools discovered is a FAILURE, not a pass: it means the scan matched nothing,
which is indistinguishable from "everything passed" in any output that prints only failures. The
denominator is always printed.

Usage:
    tools/selftest_all.py                 # run every host-applicable test
    tools/selftest_all.py --asset-free    # omit explicit game-image requirements
    tools/selftest_all.py --require NAME  # run exactly one declared requirement class
    tools/selftest_all.py --list          # list tests and declared requirements
"""

import argparse
import ast
import os
import platform
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KNOWN_REQUIREMENTS = {"game-image", "linux", "macos", "windows"}
PLATFORM_REQUIREMENTS = {"linux", "macos", "windows"}


@dataclass(frozen=True)
class Selftest:
    path: Path
    requirements: frozenset[str]


def _requirements(tree: ast.Module, path: Path) -> frozenset[str]:
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if not any(
            isinstance(target, ast.Name) and target.id == "SELFTEST_REQUIREMENTS"
            for target in node.targets
        ):
            continue
        value = ast.literal_eval(node.value)
        if not isinstance(value, (tuple, list, set, frozenset)) or not all(
            isinstance(item, str) for item in value
        ):
            raise ValueError(
                f"{path}: SELFTEST_REQUIREMENTS must be a literal collection of strings"
            )
        requirements = frozenset(value)
        unknown = requirements - KNOWN_REQUIREMENTS
        if unknown:
            raise ValueError(
                f"{path}: unknown self-test requirement(s): {', '.join(sorted(unknown))}"
            )
        return requirements
    return frozenset()


def _host_platform() -> str:
    system = platform.system().lower()
    return {"darwin": "macos"}.get(system, system)


def discover() -> list[Selftest]:
    found: list[Selftest] = []
    for root, _dirs, files in os.walk(HERE):
        if "__pycache__" in root:
            continue
        for f in sorted(files):
            if not f.endswith(".py") or f == os.path.basename(__file__):
                continue
            p = os.path.join(root, f)
            with open(p, encoding="utf-8", errors="replace") as source_file:
                src = source_file.read()
            # The string must appear somewhere it is being HANDLED, not only in prose: a docstring
            # that mentions --selftest while the tool ignores the flag would otherwise be counted.
            if "'--selftest'" in src or '"--selftest"' in src:
                path = Path(p)
                found.append(Selftest(path, _requirements(ast.parse(src), path)))
    return found


def _skip_reason(test: Selftest, asset_free: bool, required: str | None) -> str | None:
    if required is not None and required not in test.requirements:
        return f"does not require {required}"
    platform_requirements = test.requirements & PLATFORM_REQUIREMENTS
    host = _host_platform()
    if platform_requirements and host not in platform_requirements:
        return f"requires {'/'.join(sorted(platform_requirements))}; host is {host}"
    if asset_free and "game-image" in test.requirements:
        return "requires the user-supplied game image"
    return None


def main(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--asset-free",
        action="store_true",
        help="run only self-tests that do not require the user-supplied game image",
    )
    parser.add_argument(
        "--require",
        choices=sorted(KNOWN_REQUIREMENTS),
        help="run only self-tests declaring this requirement",
    )
    parser.add_argument("--list", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.asset_free and arguments.require == "game-image":
        parser.error("--asset-free and --require game-image are mutually exclusive")

    tools = discover()
    if not tools:
        print(
            "SELFTEST SUITE REFUSES: discovered 0 tools carrying a --selftest under tools/."
        )
        print(
            "  Nothing was run. An empty suite prints the same thing as a passing one, so this"
        )
        print("  is reported as a failure.")
        return 1

    if arguments.list:
        for t in tools:
            requirements = ",".join(sorted(t.requirements)) or "portable,asset-free"
            print(f"{os.path.relpath(t.path, REPO)} [{requirements}]")
        return 0

    failures = []
    skipped: list[tuple[str, str]] = []
    selected = 0
    for test in tools:
        rel = os.path.relpath(test.path, REPO)
        reason = _skip_reason(test, arguments.asset_free, arguments.require)
        if reason is not None:
            skipped.append((rel, reason))
            print(f"SKIP  {rel} ({reason})")
            continue
        selected += 1
        r = subprocess.run(
            [sys.executable, test.path, "--selftest"],
            cwd=REPO,
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
        ok = r.returncode == 0
        print(f"{'PASS' if ok else 'FAIL'}  {rel}")
        if not ok:
            failures.append(rel)
            for line in (r.stdout + r.stderr).strip().splitlines():
                print(f"        {line}")

    print()
    print(
        f"=== {selected - len(failures)} of {selected} selected self-tests passed; "
        f"{len(skipped)} skipped from {len(tools)} discovered ==="
    )
    if not selected:
        print("failed: the selected requirement/platform set contains 0 runnable self-tests")
        return 1
    if failures:
        print("failed: " + ", ".join(failures))
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
