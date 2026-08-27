#!/usr/bin/env python3
"""Sunbright's guarded default product launcher."""

from __future__ import annotations

import math
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from arguments import REPO, apply_environment_assignment


RENDER_TOOLS = REPO / "tools" / "render"
sys.path.insert(0, str(RENDER_TOOLS))

from gpu_watch import MAX_GUARD_TIMEOUT_SECS, run_guarded  # noqa: E402
from radv_hang_trace import (  # noqa: E402
    configure_radv_hang_environment,
    radv_hang_enabled,
)


SCRATCH = REPO / "scratch"
RUNNERS = {
    "recomp": REPO / "run-recomp.sh",
    "decomp": REPO / "run-decomp.sh",
}
MAX_PRESENT_HZ = 60.0
MAX_QUIT_AFTER = 10_000
TEXTURE_RESOLUTION_LINE = re.compile(
    r"^\[[^]\r\n]+\] \[texresolve\] static "
    r"(?P<width>[1-9][0-9]*)x(?P<height>[1-9][0-9]*) "
    r"mips=(?P<mips>[1-9][0-9]*) fmt=(?P<format>[0-9]+) data=0x[0-9A-Fa-f]+$"
)


@dataclass(frozen=True)
class Invocation:
    environment: dict[str, str]
    runner: str
    runner_args: list[str]
    timeout_secs: float | None
    diagnostic: bool
    renderer_was_forced: bool
    explicit_environment: dict[str, str]
    isolated_environment: bool


def _required(arguments: list[str], index: int, option: str) -> tuple[str, int]:
    if index + 1 >= len(arguments):
        raise ValueError(f"{option} needs a value")
    return arguments[index + 1], index + 2


def _set_default(environment: dict[str, str], name: str, value: str) -> None:
    if not environment.get(name):
        environment[name] = value


def _positive_integer(value: str, option: str, maximum: int | None = None) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"{option} needs a positive integer, got {value!r}") from exc
    if parsed <= 0 or (maximum is not None and parsed > maximum):
        suffix = f" no greater than {maximum}" if maximum is not None else ""
        raise ValueError(f"{option} needs a positive integer{suffix}, got {value!r}")
    return parsed


def _nonnegative_integer(value: str, option: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"{option} needs a non-negative integer, got {value!r}") from exc
    if parsed < 0:
        raise ValueError(f"{option} needs a non-negative integer, got {value!r}")
    return parsed


def _positive_float(value: str, option: str, maximum: float) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ValueError(f"{option} needs a number, got {value!r}") from exc
    if not math.isfinite(parsed) or not 0 < parsed <= maximum:
        raise ValueError(f"{option} must be finite and in (0,{maximum:g}], got {value!r}")
    return parsed


def parse_invocation(arguments: list[str], inherited: dict[str, str]) -> Invocation:
    environment = dict(inherited)
    runner = "recomp"
    rom: str | None = None
    diagnostic = False
    timeout_option: str | None = None
    index = 0
    after_separator = False
    isolated_environment = False
    explicit_names: set[str] = set()

    def set_explicit(name: str, value: str) -> None:
        environment[name] = value
        explicit_names.add(name)

    while index < len(arguments):
        argument = arguments[index]
        if after_separator:
            apply_environment_assignment(environment, argument)
            explicit_names.add(argument.split("=", 1)[0])
            index += 1
            continue
        if argument == "--":
            after_separator = True
            index += 1
            continue
        if argument in ("-h", "--help"):
            raise ValueError("--help must be handled before invocation parsing")
        if argument in ("--60fps", "--fps60"):
            set_explicit("SBR_60FPS", "1")
            index += 1
            continue
        if argument == "--fastboot":
            set_explicit("SBR_FASTBOOT", "1")
            index += 1
            continue
        if argument == "--headless":
            set_explicit("SB_HEADLESS", "1")
            index += 1
            continue
        if argument == "--mute":
            set_explicit("SBR_MUTE", "1")
            index += 1
            continue
        if argument == "--turbo":
            set_explicit("SB_TURBO", "1")
            index += 1
            continue
        if argument == "--diagnostic":
            diagnostic = True
            index += 1
            continue
        if argument == "--isolated-environment":
            isolated_environment = True
            index += 1
            continue
        if argument in ("--stage", "--scenario", "--size", "--rom", "--runtime",
                        "--quit-after", "--run-secs", "--max-present-hz"):
            value, index = _required(arguments, index, argument)
            if argument == "--stage":
                stage = str(_positive_integer(value, argument))
                set_explicit("SBR_STAGE", stage)
                set_explicit("SB_STAGE", stage)
            elif argument == "--scenario":
                scenario = str(_nonnegative_integer(value, argument))
                set_explicit("SBR_SCENARIO", scenario)
                set_explicit("SB_SCENARIO", scenario)
            elif argument == "--size":
                match = re.fullmatch(r"([1-9][0-9]*)x([1-9][0-9]*)", value)
                if match is None:
                    raise ValueError(f"--size wants WIDTHxHEIGHT, got {value!r}")
                width, height = match.groups()
                set_explicit("SB_W", width)
                set_explicit("SB_H", height)
            elif argument == "--rom":
                rom = value
            elif argument == "--runtime":
                if value not in RUNNERS:
                    raise ValueError("--runtime must be 'recomp' or 'decomp'")
                runner = value
            elif argument == "--quit-after":
                set_explicit(
                    "SBR_QUIT_AFTER",
                    str(_positive_integer(value, argument, MAX_QUIT_AFTER)),
                )
            elif argument == "--run-secs":
                timeout_option = value
            else:
                set_explicit("SB_MAX_PRESENT_HZ", value)
            continue
        if "=" in argument:
            apply_environment_assignment(environment, argument)
            explicit_names.add(argument.split("=", 1)[0])
            index += 1
            continue
        raise ValueError(f"unknown option {argument!r}; run ./run.sh --help")

    legacy_runner = environment.get("SB_RUNNER")
    if legacy_runner:
        legacy_map = {"run-recomp.sh": "recomp", "run-decomp.sh": "decomp"}
        if legacy_runner not in legacy_map:
            raise ValueError(
                f"SB_RUNNER={legacy_runner} is not supported; choose run-recomp.sh or run-decomp.sh"
            )
        runner = legacy_map[legacy_runner]
    if runner != "recomp" and not diagnostic:
        raise ValueError("the decomp oracle requires --diagnostic --runtime decomp")
    if isolated_environment and not diagnostic:
        raise ValueError("--isolated-environment requires --diagnostic")
    if isolated_environment and runner != "recomp":
        raise ValueError("--isolated-environment currently supports only the recomp runtime")

    if diagnostic:
        for name, value in (
            ("SB_TURBO", "1"),
            ("SB_MAX_PRESENT_HZ", "60"),
            ("SBR_FASTBOOT", "1"),
            ("SBR_SCENARIO", "0"),
            ("SBR_QUIT_AFTER", "400"),
        ):
            _set_default(environment, name, value)
            explicit_names.add(name)
        environment["SB_HEADLESS"] = "1"
        environment["SBR_MUTE"] = "1"
        explicit_names.update(("SB_HEADLESS", "SBR_MUTE"))

    renderer_was_forced = (environment.get("SBR_RENDERER") or "aurora") != "aurora"
    environment["SBR_RENDERER"] = "aurora"
    configure_radv_hang_environment(environment)

    timeout_text = timeout_option or environment.get("SB_RUN_SECS")
    if diagnostic and not timeout_text:
        timeout_text = "240"
    timeout_secs = (
        _positive_float(timeout_text, "--run-secs", MAX_GUARD_TIMEOUT_SECS)
        if timeout_text
        else None
    )
    if environment.get("SB_MAX_PRESENT_HZ"):
        maximum = MAX_PRESENT_HZ if diagnostic else 240.0
        _positive_float(environment["SB_MAX_PRESENT_HZ"], "--max-present-hz", maximum)
    if environment.get("SBR_QUIT_AFTER"):
        _positive_integer(environment["SBR_QUIT_AFTER"], "--quit-after", MAX_QUIT_AFTER)

    runner_args = [rom] if rom else []
    return Invocation(
        environment,
        runner,
        runner_args,
        timeout_secs,
        diagnostic,
        renderer_was_forced,
        {name: environment[name] for name in sorted(explicit_names)},
        isolated_environment,
    )


def usage() -> str:
    return """Usage: ./run.sh [options] [-- NAME=VALUE ...]

Normal play is windowed, paced, audible, unlimited, and protected by the live GPU watcher.

  --60fps                 use interpolated 60 FPS
  --fastboot              boot File 1 directly into Delfino Plaza
  --stage N               boot a specific stage
  --scenario N            choose the game's zero-based episode number
  --size WIDTHxHEIGHT      set the window size
  --rom PATH              select the user-provided game image
  --headless              suppress the window
  --mute                  silence host audio output
  --turbo                 unpace game logic (GPU submission remains rate-limited)
  --quit-after N          stop after N presents
  --run-secs SECONDS      impose a wall-clock limit
  --max-present-hz HZ     cap GPU submission rate
  --diagnostic            conservative headless/muted/60-Hz/240-second defaults
  --isolated-environment  clear ambient project knobs (diagnostic recomp only)
  --runtime decomp        use the decomp oracle (requires --diagnostic)
  -- NAME=VALUE ...       add exact environment settings after launcher options
"""


def _clean_scratch_file(path: Path) -> None:
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


def _texture_manifest_lines(output: str) -> list[str]:
    normalized = []
    for line in output.splitlines():
        if "[texresolve] static " not in line:
            continue
        match = TEXTURE_RESOLUTION_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"unrecognized texresolve record: {line!r}")
        normalized.append(
            "[texresolve] static "
            f"{match.group('width')}x{match.group('height')} "
            f"mips={match.group('mips')} fmt={match.group('format')}"
        )
    return normalized


def _texture_capture_channels(environment: dict[str, str]) -> str:
    channels = environment.get("SBR_LUCENT_DEBUG", "")
    selected = {channel for channel in channels.split(",") if channel}
    if "texresolve" in selected or "all" in selected:
        return channels
    return ",".join(filter(None, (channels, "texresolve")))


def _arm_child_contract(
    environment: dict[str, str], runner: str, isolated_environment: bool = False
) -> None:
    environment["SUNBRIGHT_SAFE_RUN"] = "1"
    environment["SUNBRIGHT_SAFE_RENDERER"] = "aurora"
    environment["SUNBRIGHT_SAFE_RADV_DEBUG"] = environment.get("RADV_DEBUG", "")
    environment["SUNBRIGHT_SAFE_ISOLATED"] = "1" if isolated_environment else "0"
    for source, destination in (
        ("SB_HEADLESS", "SUNBRIGHT_SAFE_HEADLESS"),
        ("SBR_MUTE", "SUNBRIGHT_SAFE_MUTE"),
        ("SB_MAX_PRESENT_HZ", "SUNBRIGHT_SAFE_MAX_PRESENT_HZ"),
        ("SBR_FASTBOOT", "SUNBRIGHT_SAFE_FASTBOOT"),
        ("SBR_STAGE", "SUNBRIGHT_SAFE_STAGE"),
        ("SBR_SCENARIO", "SUNBRIGHT_SAFE_SCENARIO"),
    ):
        if source in environment:
            environment[destination] = environment[source]
    stage_name = "SB_STAGE" if runner == "decomp" else "SBR_STAGE"
    scenario_name = "SB_SCENARIO" if runner == "decomp" else "SBR_SCENARIO"
    if stage_name in environment:
        environment["SUNBRIGHT_SAFE_STAGE"] = environment[stage_name]
    if scenario_name in environment:
        environment["SUNBRIGHT_SAFE_SCENARIO"] = environment[scenario_name]


def _environment_override_text(values: dict[str, str]) -> str:
    """Serialize validated launcher assignments for reapplication after `.env`."""
    lines = []
    for name, value in sorted(values.items()):
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is None:
            raise ValueError(f"invalid protected environment name {name!r}")
        lines.append(f"export {name}={shlex.quote(value)}")
    return "\n".join(lines) + ("\n" if lines else "")


def _write_environment_override(values: dict[str, str]) -> Path | None:
    if not values:
        return None
    SCRATCH.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=".run-env.", dir=SCRATCH, text=True)
    path = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(_environment_override_text(values))
    except BaseException:
        _clean_scratch_file(path)
        raise
    return path


def _run_guarded(invocation: Invocation, output_log: Path | None = None) -> int:
    command = [str(RUNNERS[invocation.runner]), *invocation.runner_args]
    if platform.system() != "Linux":
        print(
            "[run] live kernel GPU monitoring is unavailable on this platform; "
            "the in-process GPU flight recorder remains armed.",
            file=sys.stderr,
        )
        return subprocess.run(command, check=False).returncode
    result = run_guarded(command, invocation.timeout_secs, output_log=output_log)
    print(f"[run] guarded game exit={result.returncode}")
    return result.returncode


def _print_controls(invocation: Invocation) -> None:
    if invocation.diagnostic:
        return
    print(
        """────────────────────────────────────────────────────────────────────────────────
 Sunbright — Super Mario Sunshine

 Keyboard            Move WASD  ·  Camera IJKL  ·  A Space  ·  B LCtrl
                     X E  ·  Y Q  ·  Z C  ·  L F  ·  R (FLUDD) LShift
                     START Enter  ·  D-pad arrows
 Gamepad             plugged in and mapped as a GameCube pad

 Quit                close the window, or Ctrl-C
 Settings            Escape
────────────────────────────────────────────────────────────────────────────────"""
    )
    if invocation.environment.get("SBR_60FPS") == "1":
        print(" 60 FPS interpolation: ON — game logic remains at 30 Hz.")


def run(invocation: Invocation) -> int:
    os.environ.clear()
    os.environ.update(invocation.environment)
    _arm_child_contract(os.environ, invocation.runner, invocation.isolated_environment)
    override_path: Path | None = None
    try:
        if invocation.renderer_was_forced:
            print("[run] SBR_RENDERER was overridden; the default product uses Aurora.", file=sys.stderr)
        _print_controls(invocation)
        mode = "diagnostic" if invocation.diagnostic else "interactive"
        duration = f"{invocation.timeout_secs:g}s cap" if invocation.timeout_secs else "no time cap"
        print(f"[run] {mode} {invocation.runner} launch; live GPU watcher; {duration}.")
        if radv_hang_enabled(os.environ):
            print(
                "[run] RADV hang diagnostics ENABLED: driver synchronization may mask the timing defect."
            )

        dump_path = os.environ.get("SB_DUMP_FRAME")
        if not dump_path:
            override_path = _write_environment_override(invocation.explicit_environment)
            if override_path is not None:
                os.environ["SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE"] = str(override_path)
            return _run_guarded(invocation)

        os.environ["SBR_LUCENT_DEBUG"] = _texture_capture_channels(os.environ)
        protected_environment = dict(invocation.explicit_environment)
        protected_environment["SBR_LUCENT_DEBUG"] = os.environ["SBR_LUCENT_DEBUG"]
        override_path = _write_environment_override(protected_environment)
        if override_path is not None:
            os.environ["SUNBRIGHT_SAFE_EXPLICIT_ENV_FILE"] = str(override_path)
        SCRATCH.mkdir(parents=True, exist_ok=True)
        descriptor, output_name = tempfile.mkstemp(prefix=".run-out.", dir=SCRATCH)
        os.close(descriptor)
        output_log = Path(output_name)
        try:
            returncode = _run_guarded(invocation, output_log)
            manifest = Path(f"{dump_path}.textures.txt")
            lines = _texture_manifest_lines(output_log.read_text(errors="replace"))
            manifest.parent.mkdir(parents=True, exist_ok=True)
            manifest.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
            if lines:
                print(f"[run] texture manifest: {manifest} ({len(lines)} texture(s))")
            else:
                print(
                    "[run] no texture manifest was captured; the framebuffer dump is not comparable.",
                    file=sys.stderr,
                )
            return returncode
        finally:
            _clean_scratch_file(output_log)
    finally:
        if override_path is not None:
            _clean_scratch_file(override_path)


def selftest() -> int:
    interactive = parse_invocation([], {})
    assert interactive.runner == "recomp"
    assert interactive.timeout_secs is None
    assert not interactive.diagnostic
    assert not interactive.isolated_environment
    assert interactive.environment["SBR_RENDERER"] == "aurora"
    assert "SB_HEADLESS" not in interactive.environment

    diagnostic = parse_invocation(["--diagnostic"], {})
    assert diagnostic.timeout_secs == 240
    assert diagnostic.environment["SB_HEADLESS"] == "1"
    assert diagnostic.environment["SBR_MUTE"] == "1"
    assert diagnostic.environment["SB_MAX_PRESENT_HZ"] == "60"
    assert diagnostic.environment["SBR_QUIT_AFTER"] == "400"
    assert diagnostic.explicit_environment["SB_TURBO"] == "1"
    assert diagnostic.explicit_environment["SBR_QUIT_AFTER"] == "400"

    selected = parse_invocation(
        [
            "--diagnostic",
            "--runtime",
            "decomp",
            "--stage",
            "9",
            "--scenario",
            "2",
            "--run-secs",
            "30",
            "--",
            "SB_DRAW_STATS=1",
        ],
        {},
    )
    assert selected.runner == "decomp"
    assert not selected.isolated_environment
    assert selected.timeout_secs == 30
    assert selected.environment["SBR_STAGE"] == "9"
    assert selected.environment["SB_STAGE"] == "9"
    assert selected.environment["SBR_SCENARIO"] == "2"
    assert selected.environment["SB_SCENARIO"] == "2"
    assert selected.environment["SB_DRAW_STATS"] == "1"
    assert selected.explicit_environment["SB_DRAW_STATS"] == "1"
    assert selected.explicit_environment["SBR_STAGE"] == "9"

    isolated = parse_invocation(["--diagnostic", "--isolated-environment"], {})
    assert isolated.runner == "recomp"
    assert isolated.isolated_environment

    for invalid in (
        ["--runtime", "decomp"],
        ["--isolated-environment"],
        ["--diagnostic", "--isolated-environment", "--runtime", "decomp"],
        ["--diagnostic", "--max-present-hz", "0"],
        ["--run-secs", "nan"],
        ["--quit-after", "10001"],
        ["--", "not-an-assignment"],
    ):
        try:
            parse_invocation(invalid, {})
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid launch was accepted: {invalid}")

    environment = dict(diagnostic.environment)
    _arm_child_contract(environment, diagnostic.runner)
    assert environment["SUNBRIGHT_SAFE_RENDERER"] == "aurora"
    assert environment["SUNBRIGHT_SAFE_HEADLESS"] == "1"
    first_texture = (
        "[2026-08-27T20:44:33.822Z] [texresolve] "
        "static 256x128 mips=4 fmt=1 data=0x7f22ed11f9e0"
    )
    same_texture_other_process = (
        "[2026-08-27T20:45:01.100Z] [texresolve] "
        "static 256x128 mips=4 fmt=1 data=0x123456789abc"
    )
    changed_mips = same_texture_other_process.replace("mips=4", "mips=1")
    expected_texture = ["[texresolve] static 256x128 mips=4 fmt=1"]
    assert _texture_manifest_lines(first_texture) == expected_texture
    assert _texture_manifest_lines(same_texture_other_process) == expected_texture
    assert _texture_manifest_lines(changed_mips) != expected_texture
    assert _texture_manifest_lines(
        first_texture.replace("[texresolve]", "[other-channel]")
    ) == []
    assert _texture_manifest_lines("known-negative\n") == []
    protected = _environment_override_text(
        {"SBR_PAD_SCRIPT": "400:CSTICK=110/0", "SBR_NOTE": "a value with spaces"}
    )
    assert "export SBR_PAD_SCRIPT=400:CSTICK=110/0\n" in protected
    assert "export SBR_NOTE='a value with spaces'\n" in protected
    dump_invocation = parse_invocation(
        ["--diagnostic", "--", "SB_DUMP_FRAME=scratch/frames/test.rgba"],
        {"SBR_LUCENT_DEBUG": "interp,frame"},
    )
    assert _texture_capture_channels(dump_invocation.environment) == "interp,frame,texresolve"
    assert _texture_capture_channels({"SBR_LUCENT_DEBUG": "all"}) == "all"
    assert "LUCENT_DEBUG" not in dump_invocation.environment
    print("default launch policy selftest PASS")
    print("  interactive launch is guarded and unlimited; diagnostic defaults remain bounded")
    print("  runtime selection, environment separator, and invalid-limit controls pass")
    return 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if sys.argv[1:] in (["-h"], ["--help"]):
        print(usage(), end="")
        return 0
    try:
        invocation = parse_invocation(sys.argv[1:], dict(os.environ))
    except ValueError as exc:
        print(f"[run] REFUSING: {exc}", file=sys.stderr)
        return 4
    return run(invocation)


if __name__ == "__main__":
    raise SystemExit(main())
