#!/usr/bin/env python3
"""Verify the effective Sunbright compile profile from CMake's emitted commands."""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path


REPRESENTATIVES = {
    "recomp": {
        "generated guest": "/sms-recomp/generated/functions_",
        "host runtime": "/sms-recomp/host/main.cpp",
        "Aurora GPU": "/extern/aurora/lib/webgpu/gpu.cpp",
    },
    "decomp": {
        "native game": "/sms-boot/staged_sms_src/System/Application.cpp",
        "host runtime": "/sms-boot/main.cpp",
        "Aurora GPU": "/extern/aurora/lib/webgpu/gpu.cpp",
    },
}


def command_tokens(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    if not isinstance(command, str):
        return []
    return shlex.split(command)


def inspect_profile(
    entries: list[dict[str, object]], build_type: str, runtime: str
) -> list[str]:
    errors: list[str] = []
    selected: dict[str, list[str]] = {}
    for name, fragment in REPRESENTATIVES[runtime].items():
        match = next((entry for entry in entries if fragment in str(entry.get("file", ""))), None)
        if match is None:
            errors.append(f"missing representative compile command: {name}")
            continue
        selected[name] = command_tokens(match)

    if build_type.casefold() == "debug":
        for name, tokens in selected.items():
            if "-O2" not in tokens and "/O2" not in tokens:
                errors.append(f"Debug {name} is not optimized at O2")
            if "-DNDEBUG" in tokens or "/DNDEBUG" in tokens:
                errors.append(f"Debug {name} unexpectedly defines NDEBUG")
            if not any(token == "-g" or token.startswith("-g") or token in {"/Zi", "/Z7"} for token in tokens):
                errors.append(f"Debug {name} has no compiler debug information")

    aurora = selected.get("Aurora GPU", [])
    if aurora:
        if not any(
            token in {"-DAURORA_GPU_DIAGNOSTICS_STANDARD", "-DAURORA_GPU_DIAGNOSTICS_FULL"}
            for token in aurora
        ):
            errors.append("Aurora GPU diagnostics are not enabled")
        if "-DAURORA_GFX_DEBUG_GROUPS" not in aurora:
            errors.append("Aurora GPU debug groups are not enabled")
    return errors


def read_build_type(cache_path: Path) -> str:
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:STRING="):
            return line.partition("=")[2]
    raise ValueError(f"CMAKE_BUILD_TYPE is absent from {cache_path}")


def check_build(build_dir: Path) -> list[str]:
    compile_commands = build_dir / "compile_commands.json"
    cache = build_dir / "CMakeCache.txt"
    if not compile_commands.is_file():
        return [f"missing {compile_commands}"]
    if not cache.is_file():
        return [f"missing {cache}"]
    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        return [f"{compile_commands} does not contain a JSON list"]
    files = [str(entry.get("file", "")) for entry in entries]
    if any("/sms-recomp/generated/functions_" in file for file in files):
        runtime = "recomp"
    elif any("/sms-boot/staged_sms_src/System/Application.cpp" in file for file in files):
        runtime = "decomp"
    else:
        return ["compile commands contain neither the recomp nor decomp runtime representative"]
    return inspect_profile(entries, read_build_type(cache), runtime)


def synthetic_entries() -> list[dict[str, object]]:
    common = ["clang++", "-g", "-O2"]
    return [
        {"file": "/repo/sms-recomp/generated/functions_80003100.cpp", "arguments": common},
        {"file": "/repo/sms-recomp/host/main.cpp", "arguments": common},
        {
            "file": "/repo/extern/aurora/lib/webgpu/gpu.cpp",
            "arguments": common
            + ["-DAURORA_GPU_DIAGNOSTICS_STANDARD", "-DAURORA_GFX_DEBUG_GROUPS"],
        },
    ]


def selftest() -> int:
    valid = synthetic_entries()
    if inspect_profile(valid, "Debug", "recomp"):
        print("FAIL: valid optimized Debug control was rejected")
        return 1

    controls: list[tuple[str, list[dict[str, object]], str]] = []

    no_optimization = synthetic_entries()
    no_optimization[0]["arguments"] = ["clang++", "-g"]
    controls.append(("unoptimized guest", no_optimization, "not optimized"))

    no_diagnostics = synthetic_entries()
    no_diagnostics[2]["arguments"] = ["clang++", "-g", "-O2"]
    controls.append(("missing GPU diagnostics", no_diagnostics, "diagnostics are not enabled"))

    ndebug = synthetic_entries()
    ndebug[1]["arguments"] = ["clang++", "-g", "-O2", "-DNDEBUG"]
    controls.append(("NDEBUG in Debug", ndebug, "unexpectedly defines NDEBUG"))

    missing = synthetic_entries()[:2]
    controls.append(("missing representative", missing, "missing representative"))

    for name, entries, expected in controls:
        errors = inspect_profile(entries, "Debug", "recomp")
        if not any(expected in error for error in errors):
            print(f"FAIL: {name} control was not detected: {errors}")
            return 1

    print("PASS: build-profile checker accepts the valid profile and rejects four broken controls")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-sms-recomp"))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    errors = check_build(args.build_dir)
    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        return 1
    print(f"PASS: {args.build_dir} uses the optimized diagnostic build policy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
