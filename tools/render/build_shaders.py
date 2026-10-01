#!/usr/bin/env python3
"""Regenerate or verify every embedded native-render SPIR-V header."""

from __future__ import annotations

import argparse
from pathlib import Path

from shader_manifest import SHADERS
from shader_pipeline import compile_header, header_matches, write_header

REPO = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO / "build" / "shaders"


def verify_or_write(check: bool) -> int:
    stale: list[str] = []
    for shader in SHADERS:
        expected = compile_header(shader, REPO, BUILD_DIR)
        destination = REPO / shader.header
        if check:
            if not header_matches(destination, expected):
                stale.append(shader.header)
            continue
        write_header(destination, expected)
        print(f"wrote {shader.header}")
    if stale:
        for path in stale:
            print(f"stale shader header: {path}")
        print("run: uv run --frozen python tools/render/build_shaders.py")
        return 1
    if check:
        print(f"shader-build: {len(SHADERS)} shaders compile, validate, and match")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    return verify_or_write(arguments.check)


if __name__ == "__main__":
    raise SystemExit(main())
