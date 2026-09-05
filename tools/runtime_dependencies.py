#!/usr/bin/env python3
"""Deploy and verify the runtime DLLs resolved by CMake's imported-target graph."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import unittest
from pathlib import Path


def runtime_sources(manifest: Path, required: list[Path]) -> tuple[Path, ...]:
    sources = tuple(
        Path(line) for line in manifest.read_text(encoding="utf-8").splitlines() if line
    )
    names: set[str] = set()
    for source in sources:
        if (
            not source.is_absolute()
            or not source.is_file()
            or source.suffix.lower() != ".dll"
        ):
            raise ValueError(f"invalid runtime DLL source: {source}")
        name = source.name.casefold()
        if name in names:
            raise ValueError(f"runtime DLL basename collision: {source.name}")
        names.add(name)
    for source in required:
        if source not in sources:
            raise ValueError(
                f"required imported DLL absent from CMake runtime manifest: {source}"
            )
    return sources


def digest(path: Path) -> bytes:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").digest()


def deploy(
    manifest: Path, destination: Path, required: list[Path], *, check: bool
) -> int:
    sources = runtime_sources(manifest, required)
    if not destination.is_dir():
        raise ValueError(f"executable directory missing: {destination}")
    for source in sources:
        target = destination / source.name
        matches = target.is_file() and digest(target) == digest(source)
        if check and not matches:
            raise ValueError(
                f"runtime DLL missing or stale beside executable: {target}"
            )
        if not check and not matches:
            shutil.copy2(source, target)
    return len(sources)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--required", type=Path, action="append", default=[])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        from runtime_dependencies_test import RuntimeDependenciesTest

        suite = unittest.defaultTestLoader.loadTestsFromTestCase(
            RuntimeDependenciesTest
        )
        return (
            0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
        )
    if args.manifest is None or args.destination is None:
        parser.error("--manifest and --destination are required")
    try:
        count = deploy(args.manifest, args.destination, args.required, check=args.check)
    except (OSError, ValueError) as error:
        parser.exit(1, f"runtime deployment refused: {error}\n")
    print(f"runtime DLLs {'verified' if args.check else 'deployed'}: {count}/{count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
