#!/usr/bin/env python3
"""selftest_all.py — run every diagnostic tool's own `--selftest` and fail if any of them does.

WHY THIS FILE EXISTS. Several tools in this repo carry a `--selftest` that feeds a case whose
answer is forced, so the tool can prove it is capable of the OTHER answer. Nothing ran them. A
self-test nobody runs is the same bug one level up: the instrument still reports a clean negative
when it is broken, and now with a reassuring `--selftest` in its docstring.

WHAT IT DOES. Discovers every *.py under tools/ that mentions `--selftest` in its argument
handling, runs it, and prints a table. Discovery is by SOURCE, not by a hardcoded list, so a new
tool with a self-test is covered the day it lands and a tool that LOSES its self-test shows up as a
drop in the denominator rather than silently leaving the suite.

THE NEGATIVE. Zero tools discovered is a FAILURE, not a pass: it means the scan matched nothing,
which is indistinguishable from "everything passed" in any output that prints only failures. The
denominator is always printed.

Usage:
    tools/selftest_all.py            # run them all; exit non-zero if any fails
    tools/selftest_all.py --list     # just say which tools carry a self-test
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def discover():
    found = []
    for root, _dirs, files in os.walk(HERE):
        if '__pycache__' in root:
            continue
        for f in sorted(files):
            if not f.endswith('.py') or f == os.path.basename(__file__):
                continue
            p = os.path.join(root, f)
            try:
                src = open(p, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            # The string must appear somewhere it is being HANDLED, not only in prose: a docstring
            # that mentions --selftest while the tool ignores the flag would otherwise be counted.
            if "'--selftest'" in src or '"--selftest"' in src:
                found.append(p)
    return found


def main():
    tools = discover()
    if not tools:
        print("SELFTEST SUITE REFUSES: discovered 0 tools carrying a --selftest under tools/.")
        print("  Nothing was run. An empty suite prints the same thing as a passing one, so this")
        print("  is reported as a failure.")
        return 1

    if '--list' in sys.argv[1:]:
        for t in tools:
            print(os.path.relpath(t, REPO))
        return 0

    failures = []
    for t in tools:
        rel = os.path.relpath(t, REPO)
        r = subprocess.run([sys.executable, t, '--selftest'], cwd=REPO,
                           capture_output=True, text=True, timeout=300)
        ok = r.returncode == 0
        print(f"{'PASS' if ok else 'FAIL'}  {rel}")
        if not ok:
            failures.append(rel)
            for line in (r.stdout + r.stderr).strip().splitlines():
                print(f"        {line}")

    print()
    print(f"=== {len(tools) - len(failures)} of {len(tools)} self-tests passed ===")
    if failures:
        print("failed: " + ", ".join(failures))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
