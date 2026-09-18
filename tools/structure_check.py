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


MATERIAL_FAMILY_HEADER = "native-render/include/sunbright/native_render/j3d_material_family.h"
MATERIAL_FAMILY_ENUM = re.compile(
    r"enum class J3dMaterialFamily\s*:[^{]*\{(.*?)\}\s*;", re.DOTALL
)
MATERIAL_FAMILY_COUNT = re.compile(
    r"kJ3dMaterialFamilyCount\s*=\s*static_cast<std::size_t>\(\s*J3dMaterialFamily::(\w+)\s*\)"
)


def miscounted_material_family(header: str) -> str | None:
    """The family the refusal set cannot hold, or None when the count covers every family.

    The refusal set is an array sized from one named enumerator. `TexturedEffect` was appended after
    that enumerator, so the count stayed one short and a `TexturedEffect` refusal wrote past the end
    of the array - silently, in a real measurement run. The count has to name the final enumerator,
    and nothing in the language enforces that, so it is enforced here.
    """
    enum = MATERIAL_FAMILY_ENUM.search(header)
    if enum is None:
        return "REFUSES: no J3dMaterialFamily enumeration to measure"
    names = re.findall(r"^\s*(\w+)\s*,", enum.group(1), re.MULTILINE)
    if not names:
        return "REFUSES: the J3dMaterialFamily enumeration named no families"
    counted = MATERIAL_FAMILY_COUNT.search(header)
    if counted is None:
        return "REFUSES: no kJ3dMaterialFamilyCount derived from a named family"
    if counted.group(1) == names[-1]:
        return None
    return (
        f"kJ3dMaterialFamilyCount counts up to {counted.group(1)}, "
        f"but {names[-1]} is the last family"
    )


ENUM_DEFINITION = re.compile(r"enum class (\w+)\s*:[^{;]*\{([^}]*)\}\s*;", re.DOTALL)
ENUM_UPPER_BOUND = re.compile(r"<=\s*(\w+)::(\w+)")
COMMENT_OR_LITERAL = re.compile(
    r"""(//[^\n]*)|(/\*.*?\*/)|("(?:[^"\\\n]|\\.)*")|('(?:[^'\\\n]|\\.)*')""",
    re.DOTALL,
)


def without_comments(source: str) -> str:
    """The source with its comments blanked out, leaving string and character literals alone.

    A prose comma inside an enumeration's comment would otherwise read as an enumerator separator,
    which is exactly how the first draft of the bound rule below lost the member it was written to
    find. Newlines are preserved so nothing downstream sees lines merge.
    """

    def blank(match: re.Match[str]) -> str:
        comment = match.group(1) or match.group(2)
        if comment is None:
            return match.group(0)
        return "".join("\n" if character == "\n" else " " for character in comment)

    return COMMENT_OR_LITERAL.sub(blank, source)


def enum_orders(sources: dict[str, str]) -> dict[str, list[str]]:
    """Every scoped enumeration and its enumerators, in declaration order."""
    orders: dict[str, list[str]] = {}
    for source in sources.values():
        for name, body in ENUM_DEFINITION.findall(without_comments(source)):
            members = [member.split("=")[0].strip() for member in body.split(",")]
            named = [member for member in members if re.fullmatch(r"\w+", member)]
            if named:
                orders[name] = named
    return orders


def stale_enum_bounds(sources: dict[str, str]) -> list[tuple[str, str, str, str]]:
    """Range checks written as `value <= Enum::Member` where Member is no longer the last one.

    Twice in one session a value was appended to an enumeration and a hand-written bound naming the
    previous last member silently excluded it: once for the material-family refusal array, once for
    ModelBlendMode, where the new mode made every draw using it fail validation and be refused by
    the renderer's own sink. A bound of this shape is a range check, so it has to name the final
    member; if a check genuinely means "one of the first few", it cannot be written this way.
    """
    orders = enum_orders(sources)
    stale = []
    for path, source in sorted(sources.items()):
        for enum, member in ENUM_UPPER_BOUND.findall(without_comments(source)):
            members = orders.get(enum)
            if members is None or member not in members or member == members[-1]:
                continue
            stale.append((path, enum, member, members[-1]))
    return stale


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
    header_path = REPO / MATERIAL_FAMILY_HEADER
    if not header_path.is_file():
        print(f"structure: REFUSES: the material family header {MATERIAL_FAMILY_HEADER} is missing")
        return 1
    miscount = miscounted_material_family(header_path.read_text())
    if miscount is not None:
        print(f"structure: {MATERIAL_FAMILY_HEADER}: {miscount}")
    native_render = load_sources(REPO / "native-render")
    stale_bounds = stale_enum_bounds(native_render)
    for path, enum, member, last in stale_bounds:
        print(
            f"structure: {path}: bound <= {enum}::{member} stops short of {enum}::{last}"
        )
    for path, lines, limit in bad_sizes:
        print(f"structure: {path}: {lines} lines, limit {limit}")
    for path, label in [*boundary_bad, *product_bad]:
        print(f"structure: {path}: forbidden {label}")
    total_bad = (
        len(bad_sizes)
        + len(boundary_bad)
        + len(product_bad)
        + len(orphans)
        + (1 if miscount is not None else 0)
        + len(stale_bounds)
    )
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
    counted_header = """
enum class J3dMaterialFamily : std::uint8_t {
    None,
    UnlitColor,
    LitMaskedToon,
};
constexpr std::size_t kJ3dMaterialFamilyCount =
    static_cast<std::size_t>(J3dMaterialFamily::LitMaskedToon) + 1;
"""
    assert miscounted_material_family(counted_header) is None
    # The exact mistake that shipped: a family appended after the one the count names.
    appended = counted_header.replace("    LitMaskedToon,\n", "    LitMaskedToon,\n    Effect,\n")
    assert miscounted_material_family(appended) == (
        "kJ3dMaterialFamilyCount counts up to LitMaskedToon, but Effect is the last family"
    )
    assert miscounted_material_family("struct Nothing {};").startswith("REFUSES:")
    assert miscounted_material_family(
        counted_header.split("constexpr")[0]
    ).startswith("REFUSES:")
    bound_fixture = {
        "a.h": "enum class Mode : std::uint8_t { First, Second, Third };",
        "b.cpp": "return value <= Mode::Second;",
    }
    assert stale_enum_bounds(bound_fixture) == [("b.cpp", "Mode", "Second", "Third")]
    assert stale_enum_bounds(
        {**bound_fixture, "b.cpp": "return value <= Mode::Third;"}
    ) == []
    # A prose comma in an enumeration's comment is not an enumerator separator.
    commented = {
        "a.h": (
            "enum class Mode : std::uint8_t {\n"
            "    First,\n"
            "    // Second, and what follows it, are separated by prose commas here.\n"
            "    Second,\n"
            "    Third,\n"
            "};"
        ),
        "b.cpp": "return value <= Mode::Second;  // Third is the real end",
    }
    assert enum_orders(commented)["Mode"] == ["First", "Second", "Third"]
    assert stale_enum_bounds(commented) == [("b.cpp", "Mode", "Second", "Third")]
    # An enumeration this tree does not declare is not this rule's business.
    assert stale_enum_bounds({"b.cpp": "return value <= Elsewhere::Second;"}) == []
    real_bounds = stale_enum_bounds(load_sources(REPO / "native-render"))
    if real_bounds:
        print(f"FAIL: the shipping tree has a bound stopping short: {real_bounds}")
        return 1

    # And it must hold on the shipping header, not only on the fixtures.
    real_miscount = miscounted_material_family((REPO / MATERIAL_FAMILY_HEADER).read_text())
    if real_miscount is not None:
        print(f"FAIL: the shipping material family header {real_miscount}")
        return 1
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
        "PASS: source limits, dependency/config/log controls, the material family roster,\n"
        "and enumeration range bounds each distinguish both answers"
    )
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    raise SystemExit(selftest() if arguments.selftest else check())
