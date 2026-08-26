#!/usr/bin/env python3
"""Policy owner behind the stable ``./run-safe.sh`` diagnostic interface."""

from __future__ import annotations

import os
import math
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from gpu_watch import run_guarded


REPO = Path(__file__).resolve().parents[2]
SCRATCH = REPO / "scratch"
DEFAULT_RUNNER = "run-recomp.sh"
APPROVED_RUNNERS = {
    "run-recomp.sh": REPO / "run-recomp.sh",
    "run-decomp.sh": REPO / "run-decomp.sh",
}
MAX_RUN_SECS = 240.0
MAX_PRESENT_HZ = 60.0
MAX_QUIT_AFTER = 10_000


@dataclass(frozen=True)
class Invocation:
    environment: dict[str, str]
    runner_args: list[str]
    renderer_was_forced: bool


def parse_arguments(
    arguments: list[str], inherited: dict[str, str]
) -> tuple[dict[str, str], list[str]]:
    """Parse the shared launcher NAME=VALUE / ``--`` CLI exactly once."""
    environment = dict(inherited)
    runner_args: list[str] = []
    after_separator = False
    for argument in arguments:
        if not after_separator and argument == "--":
            after_separator = True
        elif not after_separator and "=" in argument:
            name, value = argument.split("=", 1)
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None:
                raise ValueError(f"invalid environment assignment: {argument!r}")
            environment[name] = value
        else:
            runner_args.append(argument)
    return environment, runner_args


def parse_invocation(arguments: list[str], inherited: dict[str, str]) -> Invocation:
    """Apply the historical NAME=VALUE / ``--`` run-safe CLI without shell policy."""
    environment, runner_args = parse_arguments(arguments, inherited)

    defaults = {
        "SB_HEADLESS": "1",
        "SB_TURBO": "1",
        "SB_MAX_PRESENT_HZ": "60",
        "SBR_MUTE": "1",
        "SBR_FASTBOOT": "1",
        "SBR_SCENARIO": "0",
        "SBR_QUIT_AFTER": "400",
    }
    for name, value in defaults.items():
        if not environment.get(name):
            environment[name] = value
    renderer_was_forced = (environment.get("SBR_RENDERER") or "aurora") != "aurora"
    environment["SBR_RENDERER"] = "aurora"
    environment["SB_HEADLESS"] = "1"
    environment["SBR_MUTE"] = "1"
    return Invocation(environment, runner_args, renderer_was_forced)


def resolve_runner(environment: dict[str, str]) -> Path:
    runner_name = environment.get("SB_RUNNER") or DEFAULT_RUNNER
    if runner_name not in APPROVED_RUNNERS:
        raise ValueError(
            f"SB_RUNNER={runner_name} is not an approved Aurora runner; choose "
            "run-recomp.sh or run-decomp.sh"
        )
    runner = APPROVED_RUNNERS[runner_name]
    if not runner.is_file() or not os.access(runner, os.X_OK):
        raise ValueError(
            f"SB_RUNNER={runner_name} is not an executable script in {REPO}"
        )
    return runner


def texture_manifest_lines(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith("[texresolve] static ")]


def wants_texture_manifest(environment: dict[str, str]) -> bool:
    return bool(environment.get("SB_DUMP_FRAME"))


def validated_limits(environment: dict[str, str]) -> tuple[float, float, int]:
    timeout_secs = float(environment.get("SB_RUN_SECS") or "240")
    present_hz = float(environment["SB_MAX_PRESENT_HZ"])
    quit_after = int(environment["SBR_QUIT_AFTER"], 10)
    if not math.isfinite(timeout_secs) or not 0 < timeout_secs <= MAX_RUN_SECS:
        raise ValueError(f"SB_RUN_SECS must be finite and in (0,{MAX_RUN_SECS:g}]")
    if not math.isfinite(present_hz) or not 0 < present_hz <= MAX_PRESENT_HZ:
        raise ValueError(f"SB_MAX_PRESENT_HZ must be finite and in (0,{MAX_PRESENT_HZ:g}]")
    if not 0 < quit_after <= MAX_QUIT_AFTER:
        raise ValueError(f"SBR_QUIT_AFTER must be in [1,{MAX_QUIT_AFTER}]")
    return timeout_secs, present_hz, quit_after


def clean_scratch_file(path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(REPO / "tools" / "scratch_clean.py"),
            "--glob",
            path.name,
            str(SCRATCH),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())


def run(invocation: Invocation) -> int:
    os.environ.clear()
    os.environ.update(invocation.environment)
    if invocation.renderer_was_forced:
        print("[run-safe] native renderer input was set. This wrapper runs the Aurora lane; use",
              file=sys.stderr)
        print("           ./run-render.sh for the gated native preview. Forcing "
              "SBR_RENDERER=aurora.", file=sys.stderr)

    display_mode = "headless" if os.environ["SB_HEADLESS"] == "1" else "windowed"
    print(f"[run-safe] present ceiling {os.environ['SB_MAX_PRESENT_HZ']} Hz, {display_mode}, "
          f"no native renderer, cap {os.environ['SBR_QUIT_AFTER']} presents.")

    try:
        runner = resolve_runner(os.environ)
        timeout_secs, _present_hz, _quit_after = validated_limits(os.environ)
    except ValueError as exc:
        print(f"[run-safe] {exc}. Refusing to run anything rather than silently falling back "
              "to a different runtime.", file=sys.stderr)
        return 4
    print(f"[run-safe] runner: {runner.name}")

    # Child launchers source .env for asset paths. They capture and restore this contract after
    # that source so project-local defaults cannot replace the final safety invariants.
    os.environ.update({
        "SUNBRIGHT_SAFE_RUN": "1",
        "SUNBRIGHT_SAFE_RENDERER": "aurora",
        "SUNBRIGHT_SAFE_HEADLESS": "1",
        "SUNBRIGHT_SAFE_MUTE": "1",
        "SUNBRIGHT_SAFE_MAX_PRESENT_HZ": os.environ["SB_MAX_PRESENT_HZ"],
    })

    command = [str(runner), *invocation.runner_args]
    dump_path = os.environ.get("SB_DUMP_FRAME")
    if not wants_texture_manifest(os.environ):
        result = run_guarded(command, timeout_secs)
        print(f"[run-safe] guarded game exit={result.returncode}")
        return result.returncode

    debug_channels = os.environ.get("LUCENT_DEBUG", "")
    selected = {channel for channel in debug_channels.split(",") if channel}
    if "texresolve" not in selected and "all" not in selected:
        os.environ["LUCENT_DEBUG"] = ",".join(filter(None, (debug_channels, "texresolve")))

    SCRATCH.mkdir(parents=True, exist_ok=True)
    descriptor, output_name = tempfile.mkstemp(prefix=".run-safe-out.", dir=SCRATCH)
    os.close(descriptor)
    output_log = Path(output_name)
    try:
        result = run_guarded(command, timeout_secs, output_log=output_log)
        manifest = Path(f"{dump_path}.textures.txt")
        lines = texture_manifest_lines(output_log.read_text(errors="replace"))
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
        if lines:
            single_level = sum("mips=1 " in line for line in lines)
            print(f"[run-safe] texture manifest beside the dump: {manifest} "
                  f"({len(lines)} texture(s), {single_level} single-level)")
        else:
            print("[run-safe] NO texture manifest was captured, so this dump is NOT comparable "
                  "against", file=sys.stderr)
            print("           another one for issue #5. That is a broken capture, not a clean "
                  "run.", file=sys.stderr)
        print(f"[run-safe] guarded game exit={result.returncode}")
        return result.returncode
    finally:
        clean_scratch_file(output_log)


def selftest() -> int:
    clean = parse_invocation([], {})
    assert clean.environment["SBR_RENDERER"] == "aurora"
    assert clean.environment["SB_HEADLESS"] == "1"
    assert clean.environment["SBR_MUTE"] == "1"
    assert clean.environment["SB_MAX_PRESENT_HZ"] == "60"
    assert not clean.renderer_was_forced

    forced = parse_invocation(
        ["SBR_RENDERER=native", "SB_MAX_PRESENT_HZ=30", "rom.rvz", "--", "X=runner-arg"],
        {},
    )
    assert forced.renderer_was_forced
    assert forced.environment["SBR_RENDERER"] == "aurora"
    assert forced.environment["SB_MAX_PRESENT_HZ"] == "30"
    assert forced.runner_args == ["rom.rvz", "X=runner-arg"]

    empty = parse_invocation(["SB_HEADLESS=", "SBR_RENDERER="], {})
    assert empty.environment["SB_HEADLESS"] == "1"
    assert not empty.renderer_was_forced

    hostile = parse_invocation(
        ["SB_HEADLESS=0", "SBR_MUTE=0"], {}
    )
    assert hostile.environment["SB_HEADLESS"] == "1"
    assert hostile.environment["SBR_MUTE"] == "1"
    for rejected in ("run-render.sh", "tools/render/gpu_watch.py", "../outside"):
        try:
            resolve_runner({"SB_RUNNER": rejected})
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe runner was accepted: {rejected}")

    for nonfinite in ("nan", "inf", "-inf"):
        invalid = dict(clean.environment, SB_RUN_SECS=nonfinite)
        try:
            validated_limits(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"non-finite timeout accepted: {nonfinite}")
    for name, value in (
        ("SB_MAX_PRESENT_HZ", "0"), ("SB_MAX_PRESENT_HZ", "61"),
        ("SBR_QUIT_AFTER", "0"), ("SBR_QUIT_AFTER", "10001"),
    ):
        invalid = dict(clean.environment, **{name: value})
        try:
            validated_limits(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe limit accepted: {name}={value}")

    for runner_name in APPROVED_RUNNERS:
        source = APPROVED_RUNNERS[runner_name].read_text(errors="replace")
        capture_at = source.index("_SUNBRIGHT_SAFE_RENDERER_CAPTURE=")
        dotenv_at = source.index('[ -f "$HERE/.env" ]')
        restore_at = source.index('export SBR_RENDERER="$_SUNBRIGHT_SAFE_RENDERER_CAPTURE"')
        assert capture_at < dotenv_at < restore_at

    try:
        parse_invocation(["1BAD=value"], {})
    except ValueError:
        pass
    else:
        raise AssertionError("invalid assignment was accepted")

    sample = "noise\n[texresolve] static first mips=1 x\n[texresolve] dynamic ignored\n"
    assert texture_manifest_lines(sample) == ["[texresolve] static first mips=1 x"]
    assert texture_manifest_lines("known-negative\n") == []
    assert wants_texture_manifest({"SB_DUMP_FRAME": "scratch/frame.rgba"})
    assert not wants_texture_manifest({"SB_DUMP_FRAME": ""})
    assert not wants_texture_manifest({})

    print("run-safe policy selftest PASS")
    print("  default and forced-renderer environment controls preserve the Aurora lane")
    print("  -- separator keeps NAME=VALUE runner arguments out of the environment")
    print("  texture manifest positive and empty negative controls pass")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        invocation = parse_invocation(sys.argv[1:], dict(os.environ))
    except ValueError as exc:
        print(f"[run-safe] {exc}", file=sys.stderr)
        return 4
    return run(invocation)


if __name__ == "__main__":
    raise SystemExit(main())
