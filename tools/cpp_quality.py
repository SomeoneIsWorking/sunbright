#!/usr/bin/env python3
"""Check changed first-party C++ with clang-format and clang-tidy.

The default candidate set is the union of staged, unstaged, and untracked files. Vendored,
decompiled, generated, build, and scratch trees are excluded. Explicit paths may be supplied when a
caller wants to check a fixed set.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


REPO = Path(__file__).resolve().parents[1]
# These are the repository's documented, current CMake build trees. Keep the legacy trees after
# them so an older local configuration remains usable, but never let it shadow the shipping build's
# command for the same translation unit.
BUILD_DIRECTORIES = (
    REPO / "build",
    REPO / "build-recomp",
    REPO / "build-sms-recomp",
    REPO / "scratch" / "build-recompiler",
)
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
EXCLUDED_PREFIXES = (
    "build/",
    "build-",
    "decomp/",
    "extern/",
    "scratch/",
    "sms-recomp/generated/",
)
GENERATED_SUFFIXES = ("_spv.h",)


def run_git(*args: str) -> list[str]:
    result = subprocess.run(
        ["git", *args], cwd=REPO, check=True, capture_output=True, text=True
    )
    return [line for line in result.stdout.splitlines() if line]


def is_first_party_cpp(relative: str) -> bool:
    normalized = relative.replace(os.sep, "/")
    return (
        Path(normalized).suffix in CPP_SUFFIXES
        and not normalized.startswith(EXCLUDED_PREFIXES)
        and not normalized.endswith(GENERATED_SUFFIXES)
    )


def changed_files() -> list[str]:
    names = set(run_git("diff", "--cached", "--name-only", "--diff-filter=ACMR"))
    names.update(run_git("diff", "--name-only", "--diff-filter=ACMR"))
    names.update(run_git("ls-files", "--others", "--exclude-standard"))
    return sorted(name for name in names if is_first_party_cpp(name) and (REPO / name).is_file())


def compile_databases() -> dict[str, tuple[dict[str, object], Path]]:
    commands: dict[str, tuple[dict[str, object], Path]] = {}
    found = False
    for build in BUILD_DIRECTORIES:
        database_path = build / "compile_commands.json"
        if not database_path.is_file():
            continue
        found = True
        entries = json.loads(database_path.read_text())
        for entry in entries:
            source = Path(str(entry["file"])).resolve()
            try:
                relative = str(source.relative_to(REPO))
            except ValueError:
                continue
            commands.setdefault(relative, (entry, build))
    if not found:
        expected = ", ".join(str(path.relative_to(REPO) / "compile_commands.json") for path in BUILD_DIRECTORIES)
        raise RuntimeError(f"no compile database found; expected one of: {expected}")
    return commands


def command_uses_clang(entry: dict[str, object]) -> bool:
    arguments = entry.get("arguments")
    executable = str(arguments[0]) if isinstance(arguments, list) and arguments else ""
    if not executable:
        executable = str(entry.get("command", "")).split(maxsplit=1)[0]
    return "clang++" in Path(executable).name


def check(paths: list[str]) -> int:
    if not paths:
        print("cpp-quality: no changed first-party C/C++ files")
        return 0

    format_command = ["clang-format", "--dry-run", "--Werror", *paths]
    formatted = subprocess.run(format_command, cwd=REPO)
    if formatted.returncode:
        print("cpp-quality: clang-format failed; run clang-format -i on the named files")
        return formatted.returncode

    sources = [path for path in paths if Path(path).suffix in SOURCE_SUFFIXES]
    if not sources:
        print(f"cpp-quality: clang-format passed for {len(paths)} file(s); no translation units")
        return 0

    try:
        commands = compile_databases()
    except (RuntimeError, json.JSONDecodeError) as error:
        print(f"cpp-quality: {error}")
        return 2

    missing = [source for source in sources if source not in commands]
    non_clang = [
        source
        for source in sources
        if source in commands and not command_uses_clang(commands[source][0])
    ]
    if missing:
        print("cpp-quality: translation units missing from compile_commands.json:")
        for source in missing:
            print(f"  {source}")
        return 2
    if non_clang:
        print("cpp-quality: translation units were not configured with clang++:")
        for source in non_clang:
            print(f"  {source}")
        return 2

    sources_by_build: dict[Path, list[str]] = {}
    for source in sources:
        sources_by_build.setdefault(commands[source][1], []).append(source)
    for build, build_sources in sorted(sources_by_build.items()):
        tidy = subprocess.run(
            ["clang-tidy", "-p", str(build), "--quiet", *build_sources], cwd=REPO
        )
        if tidy.returncode:
            print(f"cpp-quality: clang-tidy failed for {build.relative_to(REPO)}")
            return tidy.returncode
    print(
        f"cpp-quality: clang-format passed for {len(paths)} file(s); "
        f"clang-tidy passed for {len(sources)} translation unit(s)"
    )
    return 0


def selftest() -> int:
    if is_first_party_cpp("sms-recomp/runtime/shaders/example_spv.h"):
        print("FAIL: generated SPIR-V header was classified as first-party source")
        return 1
    if not is_first_party_cpp("sms-recomp/runtime/render/example.h"):
        print("FAIL: ordinary first-party header was excluded with generated SPIR-V headers")
        return 1

    scratch = REPO / "scratch"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cpp-quality-selftest-", dir=scratch) as directory:
        bad = Path(directory) / "bad.cpp"
        good = Path(directory) / "good.cpp"
        bad.write_text("int  main(){return 0;}\n")
        formatted_source = subprocess.run(
            ["clang-format", str(bad)], cwd=REPO, check=True, capture_output=True, text=True
        ).stdout
        good.write_text(formatted_source)
        bad_result = subprocess.run(
            ["clang-format", "--dry-run", "--Werror", str(bad)],
            cwd=REPO,
            capture_output=True,
        )
        good_result = subprocess.run(
            ["clang-format", "--dry-run", "--Werror", str(good)],
            cwd=REPO,
            capture_output=True,
        )
    if bad_result.returncode == 0 or good_result.returncode != 0:
        print(
            "FAIL: clang-format control did not distinguish malformed and formatted source "
            f"(bad={bad_result.returncode}, good={good_result.returncode})"
        )
        return 1
    print("PASS: generated SPIR-V headers are excluded without excluding ordinary headers")
    print("PASS: clang-format rejects malformed source and accepts its formatted control")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.paths:
        paths = []
        for path in args.paths:
            resolved = Path(path).resolve()
            try:
                relative = str(resolved.relative_to(REPO))
            except ValueError:
                continue
            if resolved.is_file() and is_first_party_cpp(relative):
                paths.append(relative)
        paths = sorted(set(paths))
    else:
        paths = changed_files()
    return check(paths)


if __name__ == "__main__":
    sys.exit(main())
