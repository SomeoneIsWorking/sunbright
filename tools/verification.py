"""Reusable command runner for Sunbright's asset-free verification gate."""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Step:
    name: str
    command: tuple[str, ...]


def python_step(name: str, path: str, *arguments: str) -> Step:
    return Step(name, (sys.executable, path, *arguments))


def gate_steps() -> tuple[Step, ...]:
    return (
        Step(
            "Python quality",
            (
                "ruff",
                "check",
                "tools/cpp_quality.py",
                "tools/migration_boundary.py",
                "tools/structure_check.py",
                "tools/verification.py",
                "tools/verify.py",
                "tools/verify_re.py",
                "tools/selftest_all.py",
                "tools/runtime_dependencies.py",
                "tools/runtime_dependencies_test.py",
                "tools/info/registry_paths.py",
                "tools/info/stale_triage.py",
                "tools/render/build_shaders.py",
                "tools/render/shader_manifest.py",
                "tools/render/shader_pipeline.py",
                "tools/render/shader_toolchain.py",
            ),
        ),
        python_step("pinned shader toolchain", "tools/render/shader_toolchain.py"),
        python_step(
            "asset-free instrument self-tests", "tools/selftest_all.py", "--asset-free"
        ),
        python_step("migration boundary", "tools/migration_boundary.py"),
        python_step("source structure", "tools/structure_check.py"),
        python_step("live documentation paths", "tools/docs/doc_paths.py"),
        python_step("registry paths", "tools/info/registry_paths.py"),
        python_step("shader provenance", "tools/render/build_shaders.py", "--check"),
        python_step("decomp symbol tool", "decomp/sms/tools/symbol_demangle_test.py"),
        Step(
            "runtime deployment formatting",
            (
                "ruff",
                "format",
                "--check",
                "tools/runtime_dependencies.py",
                "tools/runtime_dependencies_test.py",
            ),
        ),
        Step(
            "configure",
            (
                "cmake",
                "-S",
                ".",
                "-B",
                "build",
                "-G",
                "Ninja",
                f"-DPython3_EXECUTABLE={sys.executable}",
            ),
        ),
        Step("build", ("cmake", "--build", "build")),
        Step("tests", ("ctest", "--test-dir", "build", "--output-on-failure")),
        python_step("C++ format and lint", "tools/cpp_quality.py", "--all-built"),
    )


def run_steps(root: Path, steps: tuple[Step, ...]) -> int:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    for step in steps:
        print(f"\n== {step.name} ==", flush=True)
        result = subprocess.run(step.command, cwd=root, env=environment, check=False)
        if result.returncode:
            print(f"verification stopped: {step.name} exited {result.returncode}")
            return result.returncode
    print(f"\nverification passed: {len(steps)} asset-free steps")
    return 0
