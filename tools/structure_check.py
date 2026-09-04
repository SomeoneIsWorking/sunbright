#!/usr/bin/env python3
"""Enforce Sunbright's source ownership and size boundaries."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("native-render", "sms-boot", "src")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
DEFAULT_LIMIT = 1200
FILE_LIMITS = {
    # Pre-existing native-render work crossed the default boundary before this migration.
    # It may shrink but cannot grow.
    "native-render/tests/semantic_2d_pass_gpu_test.cpp": 1209,
}
NATIVE_RENDER_FORBIDDEN = {
    "Aurora/Dolphin/GX include": re.compile(
        r"^\s*#\s*include\s*[<\"].*(?:aurora|dolphin|gx/)", re.MULTILINE
    ),
    "guest/platform renderer identifier": re.compile(
        r"\b(?:Sbr[A-Z]\w*|GX[A-Z_]\w*|sbr_render_tris)\b"
    ),
}
PRODUCT_FORBIDDEN = {
    "environment read outside config owner": re.compile(r"\b(?:std::)?getenv\s*\("),
    "direct stderr/stdout write outside logger owner": re.compile(
        r"\b(?:std::(?:cerr|clog|cout)|fprintf\s*\(\s*(?:stderr|stdout)|"
        r"vfprintf\s*\(\s*(?:stderr|stdout)|fputs\s*\([^,]+,\s*(?:stderr|stdout)|"
        r"printf\s*\(|dprintf\s*\(\s*2\s*,|write\s*\(\s*2\s*,)"
    ),
}
PRODUCT_ALLOWED_PATHS = {
    "environment read outside config owner": ("sms-boot/runtime/config.cpp",),
    "direct stderr/stdout write outside logger owner": (
        "sms-boot/runtime/watchdog.cpp",
    ),
}


def source_files(root: Path = REPO) -> dict[str, int]:
    measured: dict[str, int] = {}
    for relative_root in SOURCE_ROOTS:
        directory = root / relative_root
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                relative = path.relative_to(root).as_posix()
                measured[relative] = len(path.read_text(errors="replace").splitlines())
    return measured


def size_violations(measured: dict[str, int]) -> list[tuple[str, int, int]]:
    return sorted(
        (path, lines, FILE_LIMITS.get(path, DEFAULT_LIMIT))
        for path, lines in measured.items()
        if lines > FILE_LIMITS.get(path, DEFAULT_LIMIT)
    )


def pattern_violations(
    sources: dict[str, str],
    patterns: dict[str, re.Pattern[str]],
    allowed_paths: dict[str, tuple[str, ...]] | None = None,
) -> list[tuple[str, str]]:
    allowed_paths = allowed_paths or {}
    return sorted(
        (path, label)
        for path, source in sources.items()
        for label, pattern in patterns.items()
        if path not in allowed_paths.get(label, ())
        if pattern.search(source)
    )


def load_sources(directory: Path) -> dict[str, str]:
    if not directory.is_dir():
        return {}
    return {
        path.relative_to(REPO).as_posix(): path.read_text(errors="replace")
        for path in sorted(directory.rglob("*"))
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    }


def load_product_sources(root: Path = REPO) -> dict[str, str]:
    return {
        path: source
        for path, source in load_sources(root / "sms-boot").items()
        if "/tests/" not in path
    }


def check() -> int:
    measured = source_files()
    bad_sizes = size_violations(measured)
    boundary_bad = pattern_violations(
        load_sources(REPO / "native-render"), NATIVE_RENDER_FORBIDDEN
    )
    product_bad = pattern_violations(
        load_product_sources(), PRODUCT_FORBIDDEN, PRODUCT_ALLOWED_PATHS
    )
    for path, lines, limit in bad_sizes:
        print(f"structure: {path}: {lines} lines, limit {limit}")
    for path, label in [*boundary_bad, *product_bad]:
        print(f"structure: {path}: forbidden {label}")
    total_bad = len(bad_sizes) + len(boundary_bad) + len(product_bad)
    print(f"structure: measured {len(measured)} source files; {total_bad} violation(s)")
    return 1 if total_bad else 0


def selftest() -> int:
    measured = {
        "src/app/at_limit.cpp": 1200,
        "src/app/too_large.cpp": 1201,
        "native-render/tests/semantic_2d_pass_gpu_test.cpp": 1209,
    }
    assert size_violations(measured) == [("src/app/too_large.cpp", 1201, 1200)]
    assert pattern_violations(
        {
            "native-render/good.cpp": "struct Mesh {};",
            "native-render/bad.cpp": "GXBlendMode x;",
        },
        NATIVE_RENDER_FORBIDDEN,
    ) == [("native-render/bad.cpp", "guest/platform renderer identifier")]
    assert pattern_violations(
        {
            "sms-boot/good.cpp": 'sb_errorf("runtime", "%s", message);',
            "sms-boot/bad.cpp": "std::cerr << message;",
            "sms-boot/runtime/config.cpp": "std::getenv(name);",
            "sms-boot/other/environment.cpp": "std::getenv(name);",
            "sms-boot/runtime/watchdog.cpp": "write(2, message, size);",
            "sms-boot/runtime/wrong_signal.cpp": 'dprintf(2, "broken");',
            "sms-boot/serialize.cpp": 'std::fprintf(file, "%u", value);',
        },
        PRODUCT_FORBIDDEN,
        PRODUCT_ALLOWED_PATHS,
    ) == [
        ("sms-boot/bad.cpp", "direct stderr/stdout write outside logger owner"),
        ("sms-boot/other/environment.cpp", "environment read outside config owner"),
        (
            "sms-boot/runtime/wrong_signal.cpp",
            "direct stderr/stdout write outside logger owner",
        ),
    ]
    if not source_files():
        print("FAIL: real-tree discovery measured no files")
        return 1
    print(
        "PASS: source limits and dependency/config/log controls distinguish both answers"
    )
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    raise SystemExit(selftest() if arguments.selftest else check())
