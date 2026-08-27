#!/usr/bin/env python3
"""Native-preview policy owner behind the stable ``./run-render.sh`` shim."""

from __future__ import annotations

import math
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from gpu_watch import MAX_GUARD_TIMEOUT_SECS, run_guarded
from radv_hang_trace import configure_radv_hang_environment, radv_hang_enabled

LAUNCH_TOOLS = Path(__file__).resolve().parents[1] / "launch"
sys.path.insert(0, str(LAUNCH_TOOLS))
from arguments import REPO, parse_arguments  # noqa: E402


@dataclass(frozen=True)
class NativeInvocation:
    environment: dict[str, str]
    runner_args: list[str]


AB_BOOLEAN_KEYS = ("SBR_AB", "SBR_AB_SELFTEST", "SBR_ABLATE")
AB_POSITIVE_INTEGER_KEYS = ("SBR_AB_EVERY", "SBR_AB_AT")
MAX_AB_INTEGER = (1 << 31) - 1


def validate_ab_environment(environment: dict[str, str]) -> None:
    for name in AB_BOOLEAN_KEYS:
        value = environment.get(name)
        if value in (None, ""):
            environment.pop(name, None)
            continue
        if value not in ("0", "1"):
            raise ValueError(f"{name} must be 0 or 1")
    for name in AB_POSITIVE_INTEGER_KEYS:
        value = environment.get(name)
        if value in (None, ""):
            environment.pop(name, None)
            continue
        if not value.isascii() or not value.isdecimal():
            raise ValueError(f"{name} must be a positive decimal integer")
        parsed = int(value, 10)
        if not 0 < parsed <= MAX_AB_INTEGER:
            raise ValueError(f"{name} must be in [1,{MAX_AB_INTEGER}]")
        environment[name] = str(parsed)


def safe_ab_handoff(environment: dict[str, str]) -> dict[str, str]:
    handoff = {"SUNBRIGHT_SAFE_AB_HANDOFF": "1"}
    for name in (*AB_BOOLEAN_KEYS, *AB_POSITIVE_INTEGER_KEYS):
        suffix = name.removeprefix("SBR_")
        present = name in environment
        handoff[f"SUNBRIGHT_SAFE_{suffix}_SET"] = "1" if present else "0"
        handoff[f"SUNBRIGHT_SAFE_{suffix}"] = environment.get(name, "")
    return handoff


def parse_invocation(
    arguments: list[str], inherited: dict[str, str]
) -> NativeInvocation:
    environment, runner_args = parse_arguments(arguments, inherited)
    defaults = {
        "SB_TURBO": "1",
        "SB_MAX_PRESENT_HZ": "60",
        "SBR_RUN_SECS": "330",
    }
    for name, value in defaults.items():
        if not environment.get(name):
            environment[name] = value
    environment.update(
        {
            "SB_HEADLESS": "1",
            "SBR_MUTE": "1",
            "SBR_RENDERER": "native",
            "SBR_FASTBOOT": "1",
            "SBR_STAGE": "1",
            "SBR_SCENARIO": "0",
            "SBR_J3D_CAPTURE": "1",
            "SBR_TEX": "1",
        }
    )
    validate_ab_environment(environment)
    configure_radv_hang_environment(environment)
    return NativeInvocation(environment, runner_args)


def validated_limits(environment: dict[str, str]) -> tuple[float, float]:
    timeout = float(environment["SBR_RUN_SECS"])
    present_hz = float(environment["SB_MAX_PRESENT_HZ"])
    if not math.isfinite(timeout) or not 0 < timeout <= MAX_GUARD_TIMEOUT_SECS:
        raise ValueError(
            f"SBR_RUN_SECS must be finite and in (0,{MAX_GUARD_TIMEOUT_SECS:g}]"
        )
    if not math.isfinite(present_hz) or not 0 < present_hz <= 60:
        raise ValueError("SB_MAX_PRESENT_HZ must be finite and in (0,60]")
    return timeout, present_hz


def run(invocation: NativeInvocation) -> int:
    if invocation.environment.get("SBR_RENDER_APPROVED") != "1":
        print(
            "[run-render] REFUSING TO START — SBR_RENDER_APPROVED=1 is not set.",
            file=sys.stderr,
        )
        return 1
    try:
        timeout, _present_hz = validated_limits(invocation.environment)
    except ValueError as exc:
        print(f"[run-render] {exc}", file=sys.stderr)
        return 4
    os.environ.clear()
    os.environ.update(invocation.environment)
    if radv_hang_enabled(os.environ):
        print(
            "[run-render] RADV hang diagnostics ENABLED: driver synchronization may mask "
            "the timing defect being investigated."
        )
    os.environ.update(
        {
            "SUNBRIGHT_SAFE_RUN": "1",
            "SUNBRIGHT_SAFE_RENDERER": "native",
            "SUNBRIGHT_SAFE_HEADLESS": "1",
            "SUNBRIGHT_SAFE_MUTE": "1",
            "SUNBRIGHT_SAFE_MAX_PRESENT_HZ": os.environ["SB_MAX_PRESENT_HZ"],
            "SUNBRIGHT_SAFE_J3D_CAPTURE": "1",
            "SUNBRIGHT_SAFE_TEX": "1",
            "SUNBRIGHT_SAFE_FASTBOOT": "1",
            "SUNBRIGHT_SAFE_STAGE": "1",
            "SUNBRIGHT_SAFE_SCENARIO": "0",
            "SUNBRIGHT_SAFE_RADV_DEBUG": os.environ.get("RADV_DEBUG", ""),
        }
    )
    os.environ.update(safe_ab_handoff(os.environ))
    result = run_guarded(
        [str(REPO / "run-recomp.sh"), *invocation.runner_args], timeout
    )
    print(f"[run-render] guarded game exit={result.returncode}")
    return result.returncode


def selftest() -> int:
    hostile = parse_invocation(
        ["SBR_RENDERER=aurora", "SB_HEADLESS=0", "SBR_TEX=0", "SBR_STAGE=9"],
        {"SBR_RENDER_APPROVED": "1"},
    )
    for name, expected in (
        ("SBR_RENDERER", "native"),
        ("SB_HEADLESS", "1"),
        ("SBR_MUTE", "1"),
        ("SBR_TEX", "1"),
        ("SBR_J3D_CAPTURE", "1"),
        ("SBR_STAGE", "1"),
        ("SBR_SCENARIO", "0"),
    ):
        assert hostile.environment[name] == expected
    for invalid in ("nan", "inf", "0", "601"):
        environment = dict(hostile.environment, SBR_RUN_SECS=invalid)
        try:
            validated_limits(environment)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe native timeout accepted: {invalid}")
    assert parse_invocation(["--", "A=B"], {}).runner_args == ["A=B"]
    radv = parse_invocation(
        ["SBR_RADV_HANG_DIAG=1"],
        {"SBR_RENDER_APPROVED": "1", "RADV_DEBUG": "zerovram"},
    )
    assert radv.environment["RADV_DEBUG"] == "zerovram,hang"
    assert radv_hang_enabled(radv.environment)

    for name in AB_BOOLEAN_KEYS:
        for invalid in ("yes", "-1", "2", " 1"):
            try:
                parse_invocation([f"{name}={invalid}"], {"SBR_RENDER_APPROVED": "1"})
            except ValueError:
                pass
            else:
                raise AssertionError(f"hostile A/B boolean accepted: {name}={invalid!r}")
    for name in AB_POSITIVE_INTEGER_KEYS:
        for invalid in ("nan", "-1", "0", "1x", str(MAX_AB_INTEGER + 1)):
            try:
                parse_invocation([f"{name}={invalid}"], {"SBR_RENDER_APPROVED": "1"})
            except ValueError:
                pass
            else:
                raise AssertionError(f"hostile A/B integer accepted: {name}={invalid!r}")

    explicit_ab = parse_invocation(
        [
            "SBR_AB=1",
            "SBR_AB_EVERY=7",
            "SBR_AB_SELFTEST=1",
            "SBR_ABLATE=0",
            "SBR_AB_AT=13",
        ],
        {"SBR_RENDER_APPROVED": "1"},
    )
    for invocation in (explicit_ab, parse_invocation([], {"SBR_RENDER_APPROVED": "1"})):
        test_environment = dict(invocation.environment)
        test_environment["SUNBRIGHT_SAFE_RUN"] = "1"
        test_environment.update(safe_ab_handoff(test_environment))
        result = subprocess.run(
            [str(REPO / "run-recomp.sh"), "--selftest-safe-ab-handoff"],
            env=test_environment,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise AssertionError(
                "post-.env A/B handoff selftest failed:\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
    print("run-render policy selftest PASS")
    print(
        "  approval remains required; hostile renderer/capture/headless inputs are forced safe"
    )
    print("  non-finite/unbounded timeouts and separator parsing controls pass")
    print("  allowlisted A/B values and explicit-unset state survive hostile post-capture input")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        invocation = parse_invocation(sys.argv[1:], dict(os.environ))
    except ValueError as exc:
        print(f"[run-render] {exc}", file=sys.stderr)
        return 4
    return run(invocation)


if __name__ == "__main__":
    raise SystemExit(main())
