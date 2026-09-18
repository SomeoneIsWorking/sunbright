#!/usr/bin/env python3
"""Enforce Sunbright's source ownership and size boundaries."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
# Every first-party source root the size limit applies to. `title-adapter` was added to this list
# the same day the module landed, because a root that is absent here is not measured at all -- the
# reported file count simply stays where it was, which reads like nothing changed.
SOURCE_ROOTS = (
    "native-render",
    "sms-boot",
    "src",
    "title-adapter",
    "tools/gcnport_boot",
)
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


MATERIAL_FAMILY_ROSTER = "native-render/src/j3d_material_family.cpp"
MATERIAL_CLASSIFIER = re.compile(r"\bclassify_j3d_\w+_material\b")


def unreached_material_families(headers: dict[str, str], roster: str) -> list[str]:
    """Material classifiers the shared family rule never calls.

    A classifier that nothing calls is invisible: it compiles, its own tests pass, and the draws it
    was written to accept refuse as unsupported anyway. `classify_j3d_effect_material` sat that way
    and cost 10,248 of GMSE01's draws, which is why this is checked rather than remembered.
    """
    declared = {
        name
        for source in headers.values()
        for name in MATERIAL_CLASSIFIER.findall(source)
    }
    # A call site, not a mention: a name that appears only in a comment or in a longer identifier
    # is not a caller, and counting it as one is how this check would quietly stop checking.
    called = {
        name
        for name in declared
        if re.search(rf"\b{re.escape(name)}\s*\(", roster)
    }
    return sorted(declared - called)


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
    roster_path = REPO / MATERIAL_FAMILY_ROSTER
    if not roster_path.is_file():
        print(f"structure: REFUSES: the material family roster {MATERIAL_FAMILY_ROSTER} is missing")
        return 1
    orphans = unreached_material_families(
        load_sources(REPO / "native-render" / "include"), roster_path.read_text()
    )
    for name in orphans:
        print(f"structure: {name} is declared but never called by {MATERIAL_FAMILY_ROSTER}")
    for path, lines, limit in bad_sizes:
        print(f"structure: {path}: {lines} lines, limit {limit}")
    for path, label in [*boundary_bad, *product_bad]:
        print(f"structure: {path}: forbidden {label}")
    total_bad = len(bad_sizes) + len(boundary_bad) + len(product_bad) + len(orphans)
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
    assert unreached_material_families(
        {"a.h": "classify_j3d_glow_material(state);\nclassify_j3d_used_material(state);"},
        "classify_j3d_used_material(state, texture, out);",
    ) == ["classify_j3d_glow_material"]
    # A mention is not a call: naming the classifier in a comment, or inside a longer identifier,
    # must still count as unreached.
    assert unreached_material_families(
        {"a.h": "classify_j3d_glow_material(state);"},
        "// see classify_j3d_glow_material\nclassify_j3d_glow_materialX(state);",
    ) == ["classify_j3d_glow_material"]
    assert (
        unreached_material_families(
            {"a.h": "classify_j3d_used_material(state);"},
            "classify_j3d_used_material(state, texture, out);",
        )
        == []
    )
    # The rule has to hold on the real tree, not only on fixtures: this is the check that would
    # have caught the orphaned effect family, so it must pass against the shipping roster.
    if unreached_material_families(
        load_sources(REPO / "native-render" / "include"),
        (REPO / MATERIAL_FAMILY_ROSTER).read_text(),
    ):
        print("FAIL: the shipping material family roster has an unreached classifier")
        return 1
    if not source_files():
        print("FAIL: real-tree discovery measured no files")
        return 1
    print(
        "PASS: source limits, dependency/config/log controls, and the material family\n"
        "roster check each distinguish both answers"
    )
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    raise SystemExit(selftest() if arguments.selftest else check())
