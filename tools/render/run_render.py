#!/usr/bin/env python3
"""Native-preview policy owner behind the stable ``./run-render.sh`` shim."""

from __future__ import annotations

import math
import os
import sys
from dataclasses import dataclass

from gpu_watch import MAX_GUARD_TIMEOUT_SECS, run_guarded
from run_safe import REPO, parse_arguments


@dataclass(frozen=True)
class NativeInvocation:
    environment: dict[str, str]
    runner_args: list[str]


def parse_invocation(arguments: list[str], inherited: dict[str, str]) -> NativeInvocation:
    environment, runner_args = parse_arguments(arguments, inherited)
    defaults = {
        "SB_TURBO": "1",
        "SB_MAX_PRESENT_HZ": "60",
        "SBR_RUN_SECS": "330",
    }
    for name, value in defaults.items():
        if not environment.get(name):
            environment[name] = value
    environment.update({
        "SB_HEADLESS": "1",
        "SBR_MUTE": "1",
        "SBR_RENDERER": "native",
        "SBR_FASTBOOT": "1",
        "SBR_STAGE": "1",
        "SBR_SCENARIO": "0",
        "SBR_J3D_CAPTURE": "1",
        "SBR_TEX": "1",
    })
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
        print("[run-render] REFUSING TO START — SBR_RENDER_APPROVED=1 is not set.",
              file=sys.stderr)
        return 1
    try:
        timeout, _present_hz = validated_limits(invocation.environment)
    except ValueError as exc:
        print(f"[run-render] {exc}", file=sys.stderr)
        return 4
    os.environ.clear()
    os.environ.update(invocation.environment)
    os.environ.update({
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
    })
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
        ("SBR_RENDERER", "native"), ("SB_HEADLESS", "1"), ("SBR_MUTE", "1"),
        ("SBR_TEX", "1"), ("SBR_J3D_CAPTURE", "1"), ("SBR_STAGE", "1"),
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
    print("run-render policy selftest PASS")
    print("  approval remains required; hostile renderer/capture/headless inputs are forced safe")
    print("  non-finite/unbounded timeouts and separator parsing controls pass")
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
