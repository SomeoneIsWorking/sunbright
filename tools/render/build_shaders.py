#!/usr/bin/env python3
"""Regenerate or verify every embedded native-render SPIR-V header."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from shader_manifest import SHADERS, Shader
from shader_pipeline import compile_header, header_matches, render_header

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
        destination.write_text(expected)
        print(f"wrote {shader.header}")
    if stale:
        for path in stale:
            print(f"stale shader header: {path}")
        print("run: uv run --frozen python tools/render/build_shaders.py")
        return 1
    if check:
        print(f"shader-build: {len(SHADERS)} shaders compile, validate, and match")
    return 0


def selftest() -> int:
    shader = Shader("sample.vert.glsl", "vert", "kSample", "sample_spv.h")
    expected = render_header(shader, struct.pack("<II", 0x07230203, 7))
    if "kSample[] = {0x07230203,0x00000007};" not in expected:
        print("FAIL: known SPIR-V words were not serialized deterministically")
        return 1
    try:
        render_header(shader, b"bad")
    except ValueError:
        pass
    else:
        print("FAIL: misaligned SPIR-V was accepted")
        return 1
    if header_matches(REPO / "does-not-exist_spv.h", expected):
        print("FAIL: missing header reported current")
        return 1
    print(
        "PASS: deterministic header, misalignment refusal, and missing-header negative"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    if arguments.selftest:
        return selftest()
    return verify_or_write(arguments.check)


if __name__ == "__main__":
    raise SystemExit(main())
