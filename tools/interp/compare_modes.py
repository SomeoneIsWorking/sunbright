#!/usr/bin/env python3
"""Capture and compare Native 60 against interpolated 60 at the presented-image boundary.

The comparison is temporal, not a cross-run pixel diff. Each run emits consecutive RGBA presents
stamped with the guest retrace counter. Native 60 is the scene-content control; interpolated 60 is
scored for excess fast/slow alternation in the same screen cells. A real Native 60 capture with
every other frame duplicated in memory is the positive control, so the spatial statistic must
visibly produce the other answer before lerp60 is interpreted. Capture manifests independently
prove discovery order, completeness, same-binary provenance, guest-time span, and GPU health.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import numpy as np

import cadence


REPO = Path(__file__).resolve().parents[2]
FRAMES = REPO / "scratch" / "frames"
LOGS = REPO / "scratch" / "logs"
BINARY = REPO / "build-sms-recomp" / "sms-recomp"
EMPTY_CARD = REPO / "scratch" / "bin" / "lerp_compare_no_card.raw"
GRID_W = 16
GRID_H = 12
REDUCED_CELL = 8
STATIC_FLOOR = 0.5
PAD_SCRIPT_CLOCK = "guest-retrace"
CAPTURE_POSE_LINE = re.compile(
    r"^\[[^]\r\n]+\] \[capturepose\] tick=(?P<tick>[0-9]+) "
    r"view=(?P<view>[0-9a-f]{96})$"
)
CAPTURE_POSE_INVALID_LINE = re.compile(
    r"^\[[^]\r\n]+\] \[capturepose\] invalid tick=(?P<tick>[0-9]+) reason=(?P<reason>\S+)$"
)

MODE_CONFIG = {
    "native": ("native-60", "cadence_native60.rgba"),
    "native-control": ("native-60", "cadence_native60_control.rgba"),
    "lerp": ("interpolated-60", "cadence_lerp60.rgba"),
}

MANIFEST_SCHEMA = 5


@dataclass(frozen=True)
class GridRoi:
    x0: int
    y0: int
    x1: int
    y1: int


DEFAULT_ROI = GridRoi(0, 0, GRID_W, 10)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_descriptor_set_sha256(lines: list[str]) -> str:
    canonical = "".join(f"{line}\n" for line in sorted(set(lines)))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def capture_tool_sha256() -> str:
    """Bind capture, analysis, launch, and GPU-guard code into one revision."""
    digest = hashlib.sha256()
    paths = (
        Path(__file__),
        Path(cadence.__file__),
        REPO / "run.sh",
        REPO / "run-recomp.sh",
        REPO / "tools" / "launch" / "run.py",
        REPO / "tools" / "launch" / "arguments.py",
        REPO / "tools" / "render" / "gpu_watch.py",
        REPO / "tools" / "render" / "gpu_events.py",
        REPO / "tools" / "render" / "gpu_preflight.py",
    )
    for path in paths:
        data = path.read_bytes()
        digest.update(str(path.relative_to(REPO)).encode("utf-8"))
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return digest.hexdigest()


def file_provenance(path: Path, label: str) -> dict[str, object]:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise SystemExit(f"REFUSES: {label} is missing at {resolved}")
    stat = resolved.stat()
    return {
        "sha256": sha256_file(resolved),
        "bytes": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
    }


def manifest_for(prefix: Path) -> Path:
    return Path(str(prefix) + ".manifest.json")


def capture_config(args: argparse.Namespace) -> dict[str, object]:
    return {
        "after": args.after,
        "count": args.count,
        "stage": args.stage,
        "scenario": args.scenario,
        "width": args.width,
        "height": args.height,
        "pad": args.pad,
        "pad_script_clock": PAD_SCRIPT_CLOCK,
        "save_state": "forced-empty-card",
        "haze": True,
        "display_hz": 60,
        "camera_pose": "settled-j3dsys-view-v1",
    }


def guard_result_errors(returncode: int) -> list[str]:
    if returncode == 0:
        return []
    return [f"guarded launcher returned {returncode}; GPU-clean provenance is unavailable"]


def parse_grid_roi(value: str) -> GridRoi:
    try:
        fields = tuple(int(field, 10) for field in value.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI must be four integer grid coordinates") from exc
    if len(fields) != 4:
        raise argparse.ArgumentTypeError("ROI must be X0,Y0,X1,Y1")
    roi = GridRoi(*fields)
    if not (0 <= roi.x0 < roi.x1 <= GRID_W and 0 <= roi.y0 < roi.y1 <= GRID_H):
        raise argparse.ArgumentTypeError(
            f"ROI must be a non-empty subset of the {GRID_W}x{GRID_H} analysis grid"
        )
    return roi


def capture_pose_records(log_text: str) -> tuple[dict[int, list[str]], dict[int, list[str]]]:
    records: dict[int, list[str]] = {}
    invalid: dict[int, list[str]] = {}
    for line in log_text.splitlines():
        if "[capturepose]" not in line:
            continue
        match = CAPTURE_POSE_LINE.fullmatch(line)
        if match is not None:
            records.setdefault(int(match.group("tick")), []).append(match.group("view"))
            continue
        invalid_match = CAPTURE_POSE_INVALID_LINE.fullmatch(line)
        if invalid_match is not None:
            invalid.setdefault(int(invalid_match.group("tick")), []).append(
                invalid_match.group("reason")
            )
            continue
        raise ValueError(f"unrecognized capture-pose record: {line!r}")
    return records, invalid


def required_camera_ticks(short_mode: str, frames: list[object]) -> list[int]:
    main_ticks = [
        frame.get("tick")
        for frame in frames
        if isinstance(frame, dict) and frame.get("role") == "main"
    ]
    if not main_ticks or not all(isinstance(tick, int) for tick in main_ticks):
        return []
    step = 1 if short_mode in ("native", "native-control") else 2
    first = min(main_ticks) - 2
    if first < 0:
        return []
    return list(range(first, max(main_ticks) + 1, step))


def camera_anchors_from_log(
    short_mode: str, frames: list[object], log_text: str
) -> tuple[list[dict[str, object]], list[str]]:
    errors = []
    try:
        records, invalid = capture_pose_records(log_text)
    except ValueError as exc:
        return [], [str(exc)]
    required = required_camera_ticks(short_mode, frames)
    if not required:
        return [], [f"{short_mode} frames do not define a valid camera timebase"]
    anchors = []
    for tick in required:
        if tick in invalid:
            errors.append(f"camera telemetry is invalid at tick {tick}: {invalid[tick]}")
        views = records.get(tick, [])
        if len(views) != 1:
            errors.append(
                f"camera telemetry tick {tick} has {len(views)} records; expected exactly one"
            )
            continue
        anchors.append({"tick": tick, "view": views[0]})
    if len({anchor["view"] for anchor in anchors}) < 2:
        errors.append("camera telemetry is constant; the camera-only input did not move the view")
    return anchors, errors


def camera_anchor_errors(
    native: dict[str, object], other: dict[str, object], other_mode: str
) -> list[str]:
    errors = []

    def parse(manifest: dict[str, object], label: str) -> dict[int, str]:
        raw = manifest.get("camera_anchors")
        if not isinstance(raw, list):
            errors.append(f"{label} camera anchors are missing or malformed")
            return {}
        parsed: dict[int, str] = {}
        for index, anchor in enumerate(raw):
            if not isinstance(anchor, dict):
                errors.append(f"{label} camera anchor {index} is not an object")
                continue
            tick = anchor.get("tick")
            view = anchor.get("view")
            if (
                not isinstance(tick, int)
                or not isinstance(view, str)
                or re.fullmatch(r"[0-9a-f]{96}", view) is None
            ):
                errors.append(f"{label} camera anchor {index} has malformed tick/view")
                continue
            if tick in parsed:
                errors.append(f"{label} camera tick {tick} is duplicated")
                continue
            parsed[tick] = view
        expected = required_camera_ticks(
            "native" if label == "native" else other_mode,
            manifest.get("frames", []) if isinstance(manifest.get("frames"), list) else [],
        )
        if sorted(parsed) != expected:
            errors.append(
                f"{label} camera timebase is {sorted(parsed)}, expected exact anchors {expected}"
            )
        return parsed

    native_anchors = parse(native, "native")
    other_anchors = parse(other, other_mode)
    if errors:
        return errors
    for tick, other_view in other_anchors.items():
        native_view = native_anchors.get(tick)
        if native_view is None:
            errors.append(f"native camera timebase has no anchor for {other_mode} tick {tick}")
        elif native_view != other_view:
            errors.append(
                f"camera pose differs at shared guest tick {tick}; nominal tick labels are not "
                "a comparable viewpoint"
            )
    shared_views = [native_anchors[tick] for tick in other_anchors if tick in native_anchors]
    if len(set(shared_views)) < 2:
        errors.append("shared camera anchors are constant; viewpoint comparability is untested")
    return errors


def series_entries(prefix: Path) -> list[tuple[Path, int, int, str]]:
    pattern = re.compile(
        rf"^{re.escape(prefix.name)}\.(?P<index>[0-9]+)\."
        r"(?P<role>[A-Za-z0-9_-]+)-t(?P<tick>[0-9]+)$"
    )
    entries = []
    for path in prefix.parent.glob(prefix.name + ".*"):
        match = pattern.match(path.name)
        if match is None:
            continue
        entries.append(
            (path, int(match.group("index")), int(match.group("tick")), match.group("role"))
        )
    entries.sort(key=lambda entry: entry[1])
    return entries


def series_structure_errors(entries: list[tuple[Path, int, int, str]]) -> list[str]:
    errors = []
    indices = [entry[1] for entry in entries]
    ticks = [entry[2] for entry in entries]
    if indices != list(range(len(entries))):
        errors.append(f"indices are not one contiguous 0..{len(entries) - 1} series: {indices}")
    if any(current < previous for previous, current in zip(ticks, ticks[1:])):
        errors.append(f"guest retrace labels are not monotonic: {ticks}")
    return errors


def role_cadence_errors(short_mode: str, frames: list[object]) -> list[str]:
    errors = []
    parsed = []
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict):
            errors.append(f"frame {index} is not an object")
            continue
        role = frame.get("role")
        tick = frame.get("tick")
        if not isinstance(role, str) or not isinstance(tick, int):
            errors.append(f"frame {index} has malformed role/tick metadata")
            continue
        parsed.append((role, tick))
    if errors:
        return errors
    if short_mode in ("native", "native-control"):
        unexpected = [(index, role) for index, (role, _) in enumerate(parsed) if role != "main"]
        if unexpected:
            errors.append(f"Native60 contains non-main presents: {unexpected}")
        for index in range(1, len(parsed)):
            if parsed[index][1] != parsed[index - 1][1] + 1:
                errors.append(
                    f"Native60 ticks {index - 1}/{index} are not consecutive: "
                    f"{parsed[index - 1][1]}/{parsed[index][1]}"
                )
        return errors
    if short_mode != "lerp":
        return [f"unknown capture mode {short_mode!r}"]
    for index, (role, tick) in enumerate(parsed):
        expected_role = "main" if index % 2 == 0 else "sub"
        if role != expected_role:
            errors.append(
                f"Lerp60 frame {index} role is {role!r}, expected {expected_role!r}"
            )
        if index % 2 == 1 and tick != parsed[index - 1][1]:
            errors.append(
                f"Lerp60 main/sub pair {index - 1}/{index} has different ticks "
                f"{parsed[index - 1][1]}/{tick}"
            )
        if index >= 2 and index % 2 == 0 and tick != parsed[index - 2][1] + 2:
            errors.append(
                f"Lerp60 main ticks {index - 2}/{index} are not two retraces apart: "
                f"{parsed[index - 2][1]}/{tick}"
            )
    return errors


def pair_manifest_errors(
    native: dict[str, object], other: dict[str, object], other_mode: str = "lerp"
) -> list[str]:
    errors = []
    for label, manifest in (("native", native), (other_mode, other)):
        if manifest.get("schema") != MANIFEST_SCHEMA:
            errors.append(f"{label} manifest schema is not {MANIFEST_SCHEMA}")
        if manifest.get("completed") is not True:
            errors.append(f"{label} capture did not complete successfully")
        if manifest.get("gpu_guard") != "clean":
            errors.append(f"{label} capture has no clean GPU-guard verdict")
        config = manifest.get("config")
        if not isinstance(config, dict) or config.get("pad_script_clock") != PAD_SCRIPT_CLOCK:
            errors.append(
                f"{label} capture is not keyed by the {PAD_SCRIPT_CLOCK!r} PAD script clock"
            )
    for field in (
        "binary_sha256",
        "capture_tool_sha256",
        "assets",
        "texture_descriptor_set_sha256",
        "texture_descriptor_count",
        "config",
    ):
        if native.get(field) != other.get(field):
            errors.append(f"capture manifests disagree on {field}")
    native_frames = native.get("frames", [])
    other_frames = other.get("frames", [])
    if not isinstance(native_frames, list) or not isinstance(other_frames, list):
        errors.append("capture manifest frames are malformed")
        return errors
    if len(native_frames) != len(other_frames):
        errors.append(
            f"capture sample counts differ: native={len(native_frames)}, "
            f"{other_mode}={len(other_frames)}"
        )
    errors.extend(role_cadence_errors("native", native_frames))
    errors.extend(role_cadence_errors(other_mode, other_frames))
    native_ticks = [frame.get("tick") for frame in native_frames if isinstance(frame, dict)]
    other_ticks = [frame.get("tick") for frame in other_frames if isinstance(frame, dict)]
    if native_ticks and other_ticks:
        native_span = (min(native_ticks), max(native_ticks))
        other_span = (min(other_ticks), max(other_ticks))
        if native_span != other_span:
            errors.append(
                f"capture guest-time spans differ: native={native_span}, "
                f"{other_mode}={other_span}"
            )
    errors.extend(camera_anchor_errors(native, other, other_mode))
    return errors


def repeatability_errors(first: dict[str, object], second: dict[str, object]) -> list[str]:
    errors = pair_manifest_errors(first, second, "native-control")
    if first.get("texture_manifest_sha256") != second.get("texture_manifest_sha256"):
        errors.append("Native60 repeat texture-resolution event streams differ")
    first_frames = first.get("frames")
    second_frames = second.get("frames")
    if errors or not isinstance(first_frames, list) or not isinstance(second_frames, list):
        return errors
    for index, (left, right) in enumerate(zip(first_frames, second_frames)):
        if not isinstance(left, dict) or not isinstance(right, dict):
            continue
        for field in ("tick", "role", "bytes", "sha256"):
            if left.get(field) != right.get(field):
                errors.append(
                    f"Native60 repeat frame {index} disagrees on {field}: "
                    f"{left.get(field)!r} != {right.get(field)!r}"
                )
    return errors


def prefix_for(short_mode: str) -> Path:
    return FRAMES / MODE_CONFIG[short_mode][1]


def clean_capture(short_mode: str) -> None:
    prefix = prefix_for(short_mode)
    result = subprocess.run(
        [
            sys.executable,
            str(REPO / "tools" / "scratch_clean.py"),
            "--glob",
            prefix.name + "*",
            str(FRAMES),
        ],
        cwd=REPO,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"capture cleanup refused with exit {result.returncode}")


def runtime_contract_errors(
    log_text: str,
    short_mode: str,
    args: argparse.Namespace,
    rom: Path | None = None,
    dol: Path | None = None,
) -> list[str]:
    expected_frame_rate = (
        "Native 60 FPS" if short_mode in ("native", "native-control") else "Interpolated 60 FPS"
    )
    required = (
        f"[settings] renderer=Aurora framerate={expected_frame_rate}",
        "[rt] guest time base: deterministic virtual",
        f"[pad] scripted input source: {args.pad}",
        f"[pad] scripted input clock: {PAD_SCRIPT_CLOCK}",
        "[pad] scripted input is exclusive; live PAD state ignored",
        f"[card] cannot open {EMPTY_CARD} — slot A is empty",
    )
    errors = [f"runtime did not confirm {text!r}" for text in required if text not in log_text]
    if rom is not None and dol is not None:
        launch_line = f'[run-recomp] sms-recomp  "{rom}"  "{dol}"'
        if launch_line not in log_text:
            errors.append(
                "runtime did not confirm the exact explicit ROM/DOL paths after .env processing"
            )
    return errors


def capture_command(
    short_mode: str,
    args: argparse.Namespace,
    rom: Path,
    dol: Path,
    prefix: Path,
) -> list[str]:
    frame_rate, _ = MODE_CONFIG[short_mode]
    quit_after = args.after + args.count + 8
    return [
        str(REPO / "run.sh"),
        "--diagnostic",
        "--isolated-environment",
        "--rom",
        str(rom),
        "--size",
        f"{args.width}x{args.height}",
        "--",
        f"SUNBRIGHT_DOL={dol}",
        f"SBR_CARD_A={EMPTY_CARD}",
        f"SBR_FRAME_RATE={frame_rate}",
        "SBR_DISPLAY_HZ=60",
        "SBR_HAZE=1",
        "SBR_DETERMINISTIC=1",
        "SBR_CAPTURE_POSE=1",
        f"SBR_STAGE={args.stage}",
        f"SBR_SCENARIO={args.scenario}",
        f"SBR_PAD_SCRIPT={args.pad}",
        f"SBR_PAD_SCRIPT_CLOCK={PAD_SCRIPT_CLOCK}",
        "SBR_PAD_SCRIPT_ONLY=1",
        "SBR_LUCENT_DEBUG=interp,frame",
        f"SB_DUMP_FRAME={prefix}",
        f"SB_DUMP_FRAME_AFTER={args.after}",
        "SB_DUMP_FRAME_EVERY=1",
        f"SB_DUMP_FRAME_COUNT={args.count}",
        f"SBR_QUIT_AFTER={quit_after}",
        f"SB_RUN_SECS={args.timeout}",
    ]


def capture(short_mode: str, args: argparse.Namespace) -> None:
    frame_rate, _ = MODE_CONFIG[short_mode]
    prefix = prefix_for(short_mode)
    log = LOGS / f"cadence_{short_mode}.log"
    clean_capture(short_mode)
    if EMPTY_CARD.exists():
        raise SystemExit(
            f"REFUSES: deterministic empty-card sentinel unexpectedly exists at {EMPTY_CARD}"
        )
    assert args.rom is not None
    rom = args.rom.expanduser().resolve()
    dol = args.dol.expanduser().resolve()
    asset_provenance = {
        "rom": file_provenance(rom, "ROM"),
        "dol": file_provenance(dol, "DOL"),
    }
    tool_revision = capture_tool_sha256()
    command = capture_command(short_mode, args, rom, dol, prefix)
    print(f"capture {short_mode}: {' '.join(command)}")
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(command, cwd=REPO, stdout=output, stderr=subprocess.STDOUT,
                                check=False)
    entries = series_entries(prefix)
    print(f"capture {short_mode}: exit={result.returncode}, dumps={len(entries)}, log={log}")
    guard_errors = guard_result_errors(result.returncode)
    if guard_errors:
        raise SystemExit(f"{short_mode} capture REFUSES: " + "; ".join(guard_errors))
    if len(entries) != args.count:
        raise SystemExit(
            f"{short_mode} capture produced {len(entries)} of {args.count} requested presents; "
            "the series is incomplete"
        )
    structure_errors = series_structure_errors(entries)
    if structure_errors:
        raise SystemExit(f"{short_mode} capture REFUSES: " + "; ".join(structure_errors))
    log_text = log.read_text(encoding="utf-8", errors="replace")
    contract_errors = runtime_contract_errors(log_text, short_mode, args, rom, dol)
    if contract_errors:
        raise SystemExit(
            f"{short_mode} capture REFUSES: " + "; ".join(contract_errors) + f"; inspect {log}"
        )
    after_provenance = {
        "rom": file_provenance(rom, "ROM"),
        "dol": file_provenance(dol, "DOL"),
    }
    if after_provenance != asset_provenance:
        raise SystemExit(
            f"{short_mode} capture REFUSES: ROM or DOL changed while the game was running"
        )
    if capture_tool_sha256() != tool_revision:
        raise SystemExit(
            f"{short_mode} capture REFUSES: capture, analysis, launcher, or GPU-guard code "
            "changed while the game was running"
        )
    if not BINARY.is_file():
        raise SystemExit(f"REFUSES: launched recomp binary is missing at {BINARY}")
    expected_bytes = args.width * args.height * 4
    wrong_sizes = [
        f"{path.name}={path.stat().st_size}"
        for path, _, _, _ in entries
        if path.stat().st_size != expected_bytes
    ]
    if wrong_sizes:
        raise SystemExit(
            f"{short_mode} capture REFUSES: expected exact {args.width}x{args.height} RGBA8 "
            f"({expected_bytes} bytes), got " + ", ".join(wrong_sizes)
        )
    texture_manifest = Path(str(prefix) + ".textures.txt")
    if not texture_manifest.is_file() or texture_manifest.stat().st_size == 0:
        raise SystemExit(
            f"{short_mode} capture REFUSES: launcher published no non-empty texture manifest"
        )
    texture_descriptors = texture_manifest.read_text(encoding="utf-8").splitlines()
    ticks = [entry[2] for entry in entries]
    frames = [
        {
            "name": path.name,
            "index": index,
            "tick": tick,
            "role": role,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        for path, index, tick, role in entries
    ]
    camera_anchors, camera_errors = camera_anchors_from_log(short_mode, frames, log_text)
    if camera_errors:
        raise SystemExit(
            f"{short_mode} capture REFUSES: " + "; ".join(camera_errors)
        )
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "mode": short_mode,
        "frame_rate": frame_rate,
        "completed": True,
        "gpu_guard": "clean",
        "binary_sha256": sha256_file(BINARY),
        "capture_tool_sha256": tool_revision,
        "assets": asset_provenance,
        "texture_manifest_sha256": sha256_file(texture_manifest),
        "texture_descriptor_set_sha256": canonical_descriptor_set_sha256(
            texture_descriptors
        ),
        "texture_descriptor_count": len(set(texture_descriptors)),
        "config": capture_config(args),
        "frames": frames,
        "camera_anchors": camera_anchors,
    }
    manifest_path = manifest_for(prefix)
    temporary = Path(str(manifest_path) + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, manifest_path)
    print(f"capture {short_mode}: guest retrace labels {min(ticks)}..{max(ticks)}")


def load_verified_capture(short_mode: str) -> tuple[dict[str, object], list[tuple[Path, int, int, str]]]:
    prefix = prefix_for(short_mode)
    manifest_path = manifest_for(prefix)
    if not manifest_path.is_file():
        raise SystemExit(
            f"REFUSES: {short_mode} has no successful capture manifest at {manifest_path}. "
            "Old, failed, or partial frame files are not evidence."
        )
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"REFUSES: cannot read {short_mode} manifest: {exc}") from exc
    if manifest.get("mode") != short_mode:
        raise SystemExit(
            f"REFUSES: {short_mode} manifest identifies itself as {manifest.get('mode')!r}"
        )
    expected_frame_rate = MODE_CONFIG[short_mode][0]
    if manifest.get("frame_rate") != expected_frame_rate:
        raise SystemExit(
            f"REFUSES: {short_mode} manifest frame-rate mode is "
            f"{manifest.get('frame_rate')!r}, expected {expected_frame_rate!r}"
        )
    current_tool = capture_tool_sha256()
    if manifest.get("capture_tool_sha256") != current_tool:
        raise SystemExit(
            f"REFUSES: {short_mode} was captured by a different capture/analysis revision"
        )
    entries = series_entries(prefix)
    structure_errors = series_structure_errors(entries)
    if structure_errors:
        raise SystemExit(f"REFUSES: {short_mode} series: " + "; ".join(structure_errors))
    recorded = manifest.get("frames")
    if not isinstance(recorded, list) or len(recorded) != len(entries):
        raise SystemExit(
            f"REFUSES: {short_mode} manifest/frame count mismatch: "
            f"manifest={len(recorded) if isinstance(recorded, list) else 'malformed'}, "
            f"files={len(entries)}"
        )
    for entry, expected in zip(entries, recorded):
        path, index, tick, role = entry
        observed = {
            "name": path.name,
            "index": index,
            "tick": tick,
            "role": role,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        if observed != expected:
            raise SystemExit(
                f"REFUSES: {short_mode} frame {path.name} does not match its successful manifest"
            )
    return manifest, entries


def reduce_luma(image: np.ndarray) -> np.ndarray:
    height, width, _ = image.shape
    reduced_h = GRID_H * REDUCED_CELL
    reduced_w = GRID_W * REDUCED_CELL
    if height % reduced_h != 0 or width % reduced_w != 0:
        raise SystemExit(
            f"REFUSES: {width}x{height} cannot be box-reduced exactly to "
            f"{reduced_w}x{reduced_h}; an aliased resample would corrupt the control"
        )
    luma = 0.299 * image[:, :, 0] + 0.587 * image[:, :, 1] + 0.114 * image[:, :, 2]
    block_h = height // reduced_h
    block_w = width // reduced_w
    return luma.reshape(reduced_h, block_h, reduced_w, block_w).mean(axis=(1, 3))


def load_metrics(
    entries: list[tuple[Path, int, int, str]], width: int, height: int
) -> tuple[list[float], list[np.ndarray], list[int]]:
    if len(entries) < 5:
        raise SystemExit(f"REFUSES: capture has only {len(entries)} presents; need at least 5")
    steps: list[float] = []
    reduced: list[np.ndarray] = []
    ticks: list[int] = []
    previous: np.ndarray | None = None
    for path, _, tick, _ in entries:
        image = cadence.load(path, width)
        if image.shape[:2] != (height, width):
            raise SystemExit(
                f"REFUSES: {path} decoded as {image.shape[1]}x{image.shape[0]}, "
                f"expected exact {width}x{height}"
            )
        reduced.append(reduce_luma(image))
        ticks.append(tick)
        if previous is not None:
            steps.append(float(np.abs(image - previous).mean()))
        previous = image
    return steps, reduced, ticks


def crop_to_roi(frames: list[np.ndarray], roi: GridRoi) -> list[np.ndarray]:
    expected_shape = (GRID_H * REDUCED_CELL, GRID_W * REDUCED_CELL)
    cropped = []
    for index, frame in enumerate(frames):
        if frame.shape != expected_shape:
            raise ValueError(
                f"frame {index} reduced shape is {frame.shape}, expected exact {expected_shape}"
            )
        region = frame[
            roi.y0 * REDUCED_CELL:roi.y1 * REDUCED_CELL,
            roi.x0 * REDUCED_CELL:roi.x1 * REDUCED_CELL,
        ]
        expected_region = (
            (roi.y1 - roi.y0) * REDUCED_CELL,
            (roi.x1 - roi.x0) * REDUCED_CELL,
        )
        if region.shape != expected_region:
            raise ValueError(
                f"ROI is absent from frame {index}: got {region.shape}, expected {expected_region}"
            )
        cropped.append(region)
    if not cropped:
        raise ValueError("ROI has no frames on this side of the comparison")
    return cropped


def roi_presence_errors(
    native_frames: list[np.ndarray], lerp_frames: list[np.ndarray], roi: GridRoi
) -> list[str]:
    errors = []
    sides = []
    for label, frames in (("native", native_frames), ("lerp", lerp_frames)):
        try:
            cropped = crop_to_roi(frames, roi)
        except ValueError as exc:
            errors.append(f"{label} {exc}")
            continue
        energy = cell_step_energy(cropped)
        if energy.size == 0 or not np.any(np.percentile(energy, 90, axis=0) >= STATIC_FLOOR):
            errors.append(
                f"{label} ROI has no measurable moving cell; its presence is not demonstrated"
            )
        sides.append(cropped)
    if len(sides) == 2 and len(sides[0]) != len(sides[1]):
        errors.append(
            f"ROI frame counts differ: native={len(sides[0])}, lerp={len(sides[1])}"
        )
    return errors


def cell_step_energy(frames: list[np.ndarray]) -> np.ndarray:
    if not frames:
        return np.empty((0, 0, 0), dtype=np.float64)
    height, width = frames[0].shape
    if height % REDUCED_CELL != 0 or width % REDUCED_CELL != 0:
        raise ValueError(f"ROI shape {width}x{height} does not align to analysis cells")
    grid_h = height // REDUCED_CELL
    grid_w = width // REDUCED_CELL
    out = []
    for previous, current in zip(frames, frames[1:]):
        if current.shape != frames[0].shape:
            raise ValueError("ROI frame shapes differ within one capture")
        delta = np.abs(current - previous)
        out.append(
            delta.reshape(grid_h, REDUCED_CELL, grid_w, REDUCED_CELL).mean(axis=(1, 3))
        )
    return np.asarray(out)


def spatial_alternation(frames: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    energy = cell_step_energy(frames)
    if energy.shape[0] == 0:
        raise ValueError("ROI has fewer than two frames")
    grid_h, grid_w = energy.shape[1:]
    scores = np.full((grid_h, grid_w), np.nan, dtype=np.float64)
    opinions = np.percentile(energy, 90, axis=0) >= STATIC_FLOOR
    for y in range(grid_h):
        for x in range(grid_w):
            if not opinions[y, x]:
                continue
            values = energy[:, y, x]
            numer = 0.0
            count = 0
            for first, second in zip(values, values[1:]):
                total = float(first + second)
                if total < 2.0 * STATIC_FLOOR:
                    continue
                numer += abs(float(first - second)) / total
                count += 1
            if count:
                scores[y, x] = numer / count
    return scores, energy


def duplicate_every_other(frames: list[np.ndarray]) -> list[np.ndarray]:
    duplicated = [frame.copy() for frame in frames]
    for index in range(1, len(duplicated), 2):
        duplicated[index] = duplicated[index - 1].copy()
    return duplicated


def mean_finite(values: np.ndarray) -> float:
    finite = values[np.isfinite(values)]
    return float(finite.mean()) if finite.size else float("nan")


def map_rows(label: str, scores: np.ndarray) -> None:
    print(f"\n{label} spatial alternation (0 even, 9 near every-other-present snap, . no opinion)")
    for row in scores:
        text = ""
        for value in row:
            text += "." if not np.isfinite(value) else str(min(9, int(value * 10.0)))
        print(f"  |{text}|")


def selftest() -> int:
    frames_native = []
    frames_lerp = []
    for index in range(8):
        native = np.full((GRID_H * REDUCED_CELL, GRID_W * REDUCED_CELL), index * 2.0)
        lerp = native.copy()
        lerp[3 * REDUCED_CELL:6 * REDUCED_CELL,
             4 * REDUCED_CELL:8 * REDUCED_CELL] = (index // 2) * 4.0
        frames_native.append(native)
        frames_lerp.append(lerp)
    native_score, _ = spatial_alternation(frames_native)
    lerp_score, _ = spatial_alternation(frames_lerp)
    forced_score, forced_energy = spatial_alternation(duplicate_every_other(frames_native))
    region = lerp_score[3:6, 4:8]
    outside = lerp_score.copy()
    outside[3:6, 4:8] = np.nan
    checks = {
        "even native series scores zero": np.nanmax(native_score) < 1e-9,
        "known snapping rectangle scores near one": np.nanmin(region) > 0.9,
        "outside the snapping rectangle stays even": np.nanmax(outside) < 1e-9,
        "real-series duplicate transform creates exact zero steps":
            int(np.count_nonzero(np.all(forced_energy == 0.0, axis=(1, 2)))) == 4,
        "forced duplicate scores above native": mean_finite(forced_score) > mean_finite(native_score),
    }
    base_frames = [{"tick": 100 + index, "role": "main"} for index in range(9)]
    base_camera = [
        {"tick": tick, "view": f"{tick:096x}"} for tick in range(98, 109)
    ]
    base_manifest = {
        "schema": MANIFEST_SCHEMA,
        "mode": "native",
        "completed": True,
        "gpu_guard": "clean",
        "binary_sha256": "binary",
        "capture_tool_sha256": "tool",
        "assets": {"rom": {"sha256": "rom"}, "dol": {"sha256": "dol"}},
        "texture_manifest_sha256": "textures",
        "texture_descriptor_set_sha256": "descriptor-set",
        "texture_descriptor_count": 2,
        "config": {
            "count": 9,
            "stage": 1,
            "width": 1280,
            "height": 960,
            "pad_script_clock": PAD_SCRIPT_CLOCK,
        },
        "frames": base_frames,
        "camera_anchors": base_camera,
    }
    lerp_manifest = dict(base_manifest)
    lerp_manifest["mode"] = "lerp"
    lerp_manifest["texture_manifest_sha256"] = "different-valid-event-order"
    lerp_manifest["frames"] = [
        {"tick": 100 + index - (index % 2), "role": "main" if index % 2 == 0 else "sub"}
        for index in range(9)
    ]
    lerp_manifest["camera_anchors"] = [
        {"tick": tick, "view": f"{tick:096x}"} for tick in range(98, 109, 2)
    ]
    checks["matching successful manifests are accepted"] = not pair_manifest_errors(
        base_manifest, lerp_manifest
    )
    checks["cross-mode duplicate ordering does not replace descriptor-set identity"] = (
        base_manifest["texture_manifest_sha256"]
        != lerp_manifest["texture_manifest_sha256"]
        and not pair_manifest_errors(base_manifest, lerp_manifest)
    )
    unequal = dict(base_manifest)
    unequal["frames"] = base_manifest["frames"][:-1]
    checks["unequal sample counts are refused"] = bool(
        pair_manifest_errors(base_manifest, unequal)
    )
    stale = dict(base_manifest)
    stale["binary_sha256"] = "different"
    checks["different binaries are refused"] = bool(pair_manifest_errors(base_manifest, stale))
    failed = dict(base_manifest)
    failed["completed"] = False
    checks["failed captures are refused"] = bool(pair_manifest_errors(base_manifest, failed))
    checks["zero guarded result permits clean provenance"] = not guard_result_errors(0)
    checks["nonzero guarded result cannot publish clean provenance"] = bool(
        guard_result_errors(86)
    )
    partial = dict(base_manifest)
    partial["frames"] = [
        {"tick": 101 + index, "role": "main"} for index in range(9)
    ]
    partial["camera_anchors"] = [
        {"tick": tick, "view": f"{tick:096x}"} for tick in range(99, 110)
    ]
    checks["different guest-time spans are refused"] = bool(
        pair_manifest_errors(base_manifest, partial)
    )
    fake = Path("frame")
    checks["non-contiguous indices are refused"] = bool(
        series_structure_errors([(fake, 0, 1, "main"), (fake, 2, 2, "main")])
    )
    checks["non-monotonic ticks are refused"] = bool(
        series_structure_errors([(fake, 0, 2, "main"), (fake, 1, 1, "main")])
    )
    wrong_roles = dict(lerp_manifest)
    wrong_roles["frames"] = [{"tick": 100 + index, "role": "main"} for index in range(9)]
    wrong_roles["camera_anchors"] = base_camera
    checks["all-main Lerp60 series is refused"] = bool(
        pair_manifest_errors(base_manifest, wrong_roles)
    )
    contract_args = argparse.Namespace(pad="10:CSTICK=1/2")
    contract_log = "\n".join(
        (
            "[settings] renderer=Aurora framerate=Native 60 FPS",
            "[rt] guest time base: deterministic virtual",
            "[pad] scripted input source: 10:CSTICK=1/2",
            f"[pad] scripted input clock: {PAD_SCRIPT_CLOCK}",
            "[pad] scripted input is exclusive; live PAD state ignored",
            f"[card] cannot open {EMPTY_CARD} — slot A is empty",
        )
    )
    checks["confirmed runtime contract is accepted"] = not runtime_contract_errors(
        contract_log, "native", contract_args
    )
    checks["wrong effective frame-rate mode is refused"] = bool(
        runtime_contract_errors(
            contract_log.replace("Native 60 FPS", "Interpolated 60 FPS"),
            "native",
            contract_args,
        )
    )
    checks["missing guest-retrace input clock confirmation is refused"] = bool(
        runtime_contract_errors(
            contract_log.replace(f"[pad] scripted input clock: {PAD_SCRIPT_CLOCK}\n", ""),
            "native",
            contract_args,
        )
    )
    capture_args = argparse.Namespace(
        after=1600,
        count=33,
        timeout=120,
        stage=1,
        scenario=0,
        width=1280,
        height=960,
        pad="800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0",
    )
    command = capture_command(
        "lerp",
        capture_args,
        Path("/asset/game.rvz"),
        Path("/asset/main.dol"),
        Path("/capture/frame.rgba"),
    )
    checks["capture command does not arm unrelated smoothness frame sink"] = not any(
        argument.startswith("SBR_SMOOTH=") for argument in command
    )
    checks["capture command uses the runtime's prefixed Lucent registry"] = (
        "SBR_LUCENT_DEBUG=interp,frame" in command
        and not any(argument.startswith("LUCENT_DEBUG=") for argument in command)
    )
    checks["capture command keeps exact isolated mode and input contract"] = all(
        required in command
        for required in (
            "--isolated-environment",
            "SBR_FRAME_RATE=interpolated-60",
            "SBR_DETERMINISTIC=1",
            "SBR_CAPTURE_POSE=1",
            "SBR_PAD_SCRIPT=800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0",
            f"SBR_PAD_SCRIPT_CLOCK={PAD_SCRIPT_CLOCK}",
            "SBR_PAD_SCRIPT_ONLY=1",
            "SB_DUMP_FRAME_COUNT=33",
        )
    )
    changed_assets = dict(lerp_manifest)
    changed_assets["assets"] = {
        "rom": {"sha256": "different"},
        "dol": {"sha256": "dol"},
    }
    checks["different guest assets are refused"] = bool(
        pair_manifest_errors(base_manifest, changed_assets)
    )
    changed_dimensions = dict(lerp_manifest)
    changed_dimensions["config"] = dict(lerp_manifest["config"], height=720)
    checks["different capture dimensions are refused"] = bool(
        pair_manifest_errors(base_manifest, changed_dimensions)
    )
    read_count_native = dict(base_manifest)
    read_count_native["config"] = dict(
        base_manifest["config"], pad_script_clock="read-count"
    )
    read_count_lerp = dict(lerp_manifest)
    read_count_lerp["config"] = dict(
        lerp_manifest["config"], pad_script_clock="read-count"
    )
    checks["matching read-count input manifests are still refused"] = bool(
        pair_manifest_errors(read_count_native, read_count_lerp)
    )
    changed_pose = dict(lerp_manifest)
    changed_pose["camera_anchors"] = [dict(anchor) for anchor in lerp_manifest["camera_anchors"]]
    changed_pose["camera_anchors"][2]["view"] = "f" * 96
    checks["same guest tick with a different camera pose is refused"] = bool(
        pair_manifest_errors(base_manifest, changed_pose)
    )
    missing_predecessor = dict(lerp_manifest)
    missing_predecessor["camera_anchors"] = lerp_manifest["camera_anchors"][1:]
    checks["missing Lerp predecessor pose breaks the timebase gate"] = bool(
        pair_manifest_errors(base_manifest, missing_predecessor)
    )
    pose_log = "\n".join(
        (
            f"[2026-08-28T00:00:00.000Z] [capturepose] tick=100 view={'1' * 96}",
            f"[2026-08-28T00:00:01.000Z] [other] tick=101 view={'2' * 96}",
        )
    )
    parsed_pose, invalid_pose = capture_pose_records(pose_log)
    checks["current timestamped camera telemetry is parsed by exact channel"] = (
        parsed_pose == {100: ["1" * 96]} and not invalid_pose
    )
    capture_pose_frames = [
        {"tick": 100 + index, "role": "main"} for index in range(5)
    ]
    capture_pose_log = "\n".join(
        f"[2026-08-28T00:00:{tick % 60:02d}.000Z] [capturepose] "
        f"tick={tick} view={tick:096x}"
        for tick in range(98, 105)
    )
    _, capture_pose_errors = camera_anchors_from_log(
        "native", capture_pose_frames, capture_pose_log
    )
    checks["camera telemetry covers the exact predecessor timebase"] = not capture_pose_errors
    descriptors = [
        "[texresolve] static 32x32 mips=1 fmt=3",
        "[texresolve] static 64x64 mips=4 fmt=1",
    ]
    reordered_duplicates = [descriptors[1], descriptors[0], descriptors[1]]
    changed_descriptor = [descriptors[0], descriptors[1].replace("mips=4", "mips=1")]
    checks["descriptor set ignores event reordering and duplicate count"] = (
        canonical_descriptor_set_sha256(descriptors)
        == canonical_descriptor_set_sha256(reordered_duplicates)
    )
    checks["descriptor set detects changed mip semantics"] = (
        canonical_descriptor_set_sha256(descriptors)
        != canonical_descriptor_set_sha256(changed_descriptor)
    )
    native_control = dict(base_manifest)
    native_control["mode"] = "native-control"
    checks["identical Native60 repeat is accepted"] = not repeatability_errors(
        base_manifest, native_control
    )
    reordered_repeat = dict(native_control)
    reordered_repeat["texture_manifest_sha256"] = "different-valid-event-order"
    checks["Native60 repeat requires exact texture event ordering"] = bool(
        repeatability_errors(base_manifest, reordered_repeat)
    )
    changed_repeat = dict(native_control)
    changed_repeat["frames"] = [dict(frame) for frame in native_control["frames"]]
    changed_repeat["frames"][4]["sha256"] = "different"
    checks["changed Native60 repeat is refused"] = bool(
        repeatability_errors(base_manifest, changed_repeat)
    )
    checks["default ROI excludes the bottom two dialogue rows"] = (
        DEFAULT_ROI == GridRoi(0, 0, GRID_W, 10)
        and crop_to_roi(frames_lerp, DEFAULT_ROI)[0].shape
        == (10 * REDUCED_CELL, GRID_W * REDUCED_CELL)
    )
    checks["ROI is demonstrably present and moving on both sides"] = not roi_presence_errors(
        frames_native, frames_lerp, DEFAULT_ROI
    )
    checks["static or absent ROI refuses instead of reporting no excess"] = bool(
        roi_presence_errors(
            [np.zeros_like(frames_native[0]) for _ in frames_native],
            frames_lerp,
            DEFAULT_ROI,
        )
    )
    for name, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'}  {name}")
    return 0 if all(checks.values()) else 1


def analyze(args: argparse.Namespace) -> None:
    native_manifest, native_entries = load_verified_capture("native")
    native_control_manifest, _ = load_verified_capture("native-control")
    lerp_manifest, lerp_entries = load_verified_capture("lerp")
    repeat_errors = repeatability_errors(native_manifest, native_control_manifest)
    if repeat_errors:
        raise SystemExit(
            "REFUSES: Native60 repeatability control failed: " + "; ".join(repeat_errors)
        )
    manifest_errors = pair_manifest_errors(native_manifest, lerp_manifest)
    if manifest_errors:
        raise SystemExit("REFUSES: captures are not one comparable pair: " + "; ".join(manifest_errors))
    recorded_config = native_manifest.get("config")
    if (
        not isinstance(recorded_config, dict)
        or recorded_config.get("width") != args.width
        or recorded_config.get("height") != args.height
    ):
        observed = (
            f"{recorded_config.get('width')}x{recorded_config.get('height')}"
            if isinstance(recorded_config, dict)
            else "malformed"
        )
        raise SystemExit(
            f"REFUSES: requested {args.width}x{args.height} does not match the capture manifest "
            f"({observed})"
        )
    native_steps, native_frames, native_ticks = load_metrics(
        native_entries, args.width, args.height
    )
    lerp_steps, lerp_frames, lerp_ticks = load_metrics(lerp_entries, args.width, args.height)
    overlap_lo = min(native_ticks)
    overlap_hi = max(native_ticks)
    roi_errors = roi_presence_errors(native_frames, lerp_frames, args.roi_cells)
    if roi_errors:
        raise SystemExit("REFUSES: analysis ROI is not present on both sides: " + "; ".join(roi_errors))
    native_roi = crop_to_roi(native_frames, args.roi_cells)
    lerp_roi = crop_to_roi(lerp_frames, args.roi_cells)

    print("\n=== GLOBAL CONSECUTIVE-PRESENT CADENCE ===")
    native_global = cadence.score(native_steps, "native-60", native_ticks)
    lerp_global = cadence.score(lerp_steps, "interpolated-60", lerp_ticks)
    if native_global is None or lerp_global is None:
        raise SystemExit("REFUSES: one cadence series could not be scored")

    native_score, _ = spatial_alternation(native_roi)
    lerp_score, _ = spatial_alternation(lerp_roi)
    forced_frames = duplicate_every_other(native_roi)
    forced_score, forced_energy = spatial_alternation(forced_frames)
    exact_zero_steps = int(np.count_nonzero(np.all(forced_energy == 0.0, axis=(1, 2))))
    expected_zero_steps = len(forced_frames) // 2
    if exact_zero_steps != expected_zero_steps:
        raise SystemExit(
            f"CONTROL FAILED: forced duplicate yielded {exact_zero_steps} exact zero steps, "
            f"expected {expected_zero_steps}"
        )
    if not mean_finite(forced_score) > mean_finite(native_score):
        raise SystemExit(
            "CONTROL FAILED: duplicating every other real Native 60 frame did not raise spatial "
            "alternation; do not interpret lerp60"
        )

    map_rows("native-60", native_score)
    map_rows("forced-snap control", forced_score)
    map_rows("interpolated-60", lerp_score)
    print(
        f"\nROI: grid cells [{args.roi_cells.x0},{args.roi_cells.y0}.."
        f"{args.roi_cells.x1},{args.roi_cells.y1}); exact moving content was present in both "
        "captures. Cells outside it were not scored."
    )

    shared = np.isfinite(native_score) & np.isfinite(lerp_score)
    if not np.any(shared):
        raise SystemExit("REFUSES: native and lerp captures have no shared moving screen cells")
    excess = np.full_like(native_score, np.nan)
    excess[shared] = lerp_score[shared] - native_score[shared]
    print("\nlerp excess over Native 60 by cell (positive means more fast/slow alternation)")
    ranked = []
    for y, x in zip(*np.where(shared)):
        ranked.append(
            (float(excess[y, x]), x + args.roi_cells.x0, y + args.roi_cells.y0)
        )
    for value, x, y in sorted(ranked, reverse=True)[:16]:
        score_x = x - args.roi_cells.x0
        score_y = y - args.roi_cells.y0
        print(
            f"  cell ({x:02d},{y:02d}) frame box "
            f"[{x / GRID_W:.3f},{y / GRID_H:.3f}.."
            f"{(x + 1) / GRID_W:.3f},{(y + 1) / GRID_H:.3f}] "
            f"native={native_score[score_y, score_x]:.3f} "
            f"lerp={lerp_score[score_y, score_x]:.3f} excess={value:+.3f}"
        )
    print(
        f"\nCONTROL: every-other-frame duplicate produced {exact_zero_steps}/"
        f"{len(forced_frames) - 1} exact zero steps and raised mean spatial alternation "
        f"{mean_finite(native_score):.3f} -> {mean_finite(forced_score):.3f}."
    )
    print(
        f"COMPARISON: common guest retrace labels {overlap_lo}..{overlap_hi}; mean spatial "
        f"alternation native={mean_finite(native_score):.3f}, lerp={mean_finite(lerp_score):.3f}."
    )
    print(
        "BLIND SPOTS: this localizes temporal unevenness, not object identity or correctness. "
        "Different content can occupy the same screen cell across runs; inspect the highest-excess "
        "regions and then join them to draw identities in one lerp run."
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "action",
        nargs="?",
        choices=("capture-native", "capture-native-control", "capture-lerp", "analyze"),
    )
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--after", type=int, default=1820)
    parser.add_argument("--count", type=int, default=33)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--stage", type=int, default=1)
    parser.add_argument("--scenario", type=int, default=0)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=960)
    parser.add_argument(
        "--roi-cells",
        type=parse_grid_roi,
        default=DEFAULT_ROI,
        metavar="X0,Y0,X1,Y1",
        help=(
            "exact analysis-grid ROI; default 0,0,16,10 excludes the two bottom dialogue rows "
            "and refuses unless moving content is present on both sides"
        ),
    )
    parser.add_argument(
        "--rom",
        type=Path,
        help="explicit user-provided game image; required for capture provenance",
    )
    parser.add_argument(
        "--dol",
        type=Path,
        default=REPO / "scratch" / "bin" / "sms.dol",
        help="explicit guest executable whose content hash is bound into every capture",
    )
    parser.add_argument(
        "--pad",
        default="800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0",
        help="input steps keyed by guest retrace (the comparator fixes this clock across modes)",
    )
    args = parser.parse_args()
    if not args.selftest and args.action is None:
        parser.error(
            "choose capture-native, capture-native-control, capture-lerp, analyze, or --selftest"
        )
    if (
        args.after < 1
        or args.count < 5
        or args.timeout < 1
        or args.width < 1
        or args.height < 1
    ):
        parser.error(
            "--after >=1, --count >=5, --timeout >=1, and positive dimensions are required"
        )
    if args.count % 2 == 0:
        parser.error("--count must be odd so Native60 and Lerp60 cover the same guest-time span")
    if args.action is not None and args.action.startswith("capture-") and args.rom is None:
        parser.error("capture actions require --rom so the exact game asset is bound to evidence")
    return args


def main() -> int:
    args = parse_args()
    if args.selftest:
        return selftest()
    FRAMES.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    if args.action == "capture-native":
        capture("native", args)
    elif args.action == "capture-native-control":
        capture("native-control", args)
    elif args.action == "capture-lerp":
        capture("lerp", args)
    else:
        analyze(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
