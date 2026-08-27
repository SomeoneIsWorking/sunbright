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
GRID_W = 16
GRID_H = 12
REDUCED_CELL = 8
STATIC_FLOOR = 0.5

MODE_CONFIG = {
    "native": ("native-60", "cadence_native60.rgba"),
    "native-control": ("native-60", "cadence_native60_control.rgba"),
    "lerp": ("interpolated-60", "cadence_lerp60.rgba"),
}

MANIFEST_SCHEMA = 1


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def manifest_for(prefix: Path) -> Path:
    return Path(str(prefix) + ".manifest.json")


def capture_config(args: argparse.Namespace) -> dict[str, object]:
    return {
        "after": args.after,
        "count": args.count,
        "stage": args.stage,
        "scenario": args.scenario,
        "width": args.width,
        "pad": args.pad,
    }


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
    for field in ("binary_sha256", "comparator_sha256", "config"):
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
    return errors


def repeatability_errors(first: dict[str, object], second: dict[str, object]) -> list[str]:
    errors = pair_manifest_errors(first, second, "native-control")
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
    log_text: str, short_mode: str, args: argparse.Namespace
) -> list[str]:
    expected_frame_rate = (
        "Native 60 FPS" if short_mode in ("native", "native-control") else "Interpolated 60 FPS"
    )
    required = (
        f"[settings] renderer=Aurora framerate={expected_frame_rate}",
        "[rt] guest time base: deterministic virtual",
        f"[pad] scripted input source: {args.pad}",
        "[pad] scripted input is exclusive; live PAD state ignored",
    )
    return [f"runtime did not confirm {text!r}" for text in required if text not in log_text]


def capture(short_mode: str, args: argparse.Namespace) -> None:
    frame_rate, _ = MODE_CONFIG[short_mode]
    prefix = prefix_for(short_mode)
    log = LOGS / f"cadence_{short_mode}.log"
    clean_capture(short_mode)
    quit_after = args.after + args.count + 8
    command = [
        str(REPO / "run.sh"),
        "--diagnostic",
        "--",
        f"SBR_FRAME_RATE={frame_rate}",
        "SBR_DETERMINISTIC=1",
        f"SBR_STAGE={args.stage}",
        f"SBR_SCENARIO={args.scenario}",
        f"SBR_PAD_SCRIPT={args.pad}",
        "SBR_PAD_SCRIPT_ONLY=1",
        "SBR_SMOOTH=1",
        "LUCENT_DEBUG=interp,smooth,frame",
        f"SB_DUMP_FRAME={prefix}",
        f"SB_DUMP_FRAME_AFTER={args.after}",
        "SB_DUMP_FRAME_EVERY=1",
        f"SB_DUMP_FRAME_COUNT={args.count}",
        f"SBR_QUIT_AFTER={quit_after}",
        f"SB_RUN_SECS={args.timeout}",
    ]
    print(f"capture {short_mode}: {' '.join(command)}")
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run(command, cwd=REPO, stdout=output, stderr=subprocess.STDOUT,
                                check=False)
    entries = series_entries(prefix)
    print(f"capture {short_mode}: exit={result.returncode}, dumps={len(entries)}, log={log}")
    if result.returncode != 0:
        raise SystemExit(f"{short_mode} run failed with exit {result.returncode}; inspect {log}")
    if len(entries) != args.count:
        raise SystemExit(
            f"{short_mode} capture produced {len(entries)} of {args.count} requested presents; "
            "the series is incomplete"
        )
    structure_errors = series_structure_errors(entries)
    if structure_errors:
        raise SystemExit(f"{short_mode} capture REFUSES: " + "; ".join(structure_errors))
    log_text = log.read_text(encoding="utf-8", errors="replace")
    contract_errors = runtime_contract_errors(log_text, short_mode, args)
    if contract_errors:
        raise SystemExit(
            f"{short_mode} capture REFUSES: " + "; ".join(contract_errors) + f"; inspect {log}"
        )
    gpu_clean = "GPU clean: no NEW ring timeout, reset or fault during this run" in log_text
    if not gpu_clean:
        raise SystemExit(
            f"{short_mode} capture has no clean GPU-guard verdict; inspect {log}. "
            "No manifest was published."
        )
    if not BINARY.is_file():
        raise SystemExit(f"REFUSES: launched recomp binary is missing at {BINARY}")
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
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "mode": short_mode,
        "frame_rate": frame_rate,
        "completed": True,
        "gpu_guard": "clean",
        "binary_sha256": sha256_file(BINARY),
        "comparator_sha256": sha256_file(Path(__file__)),
        "config": capture_config(args),
        "frames": frames,
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
    current_comparator = sha256_file(Path(__file__))
    if manifest.get("comparator_sha256") != current_comparator:
        raise SystemExit(
            f"REFUSES: {short_mode} was captured by a different comparator revision"
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


def load_metrics(entries: list[tuple[Path, int, int, str]], width: int) -> tuple[list[float], list[np.ndarray], list[int]]:
    if len(entries) < 5:
        raise SystemExit(f"REFUSES: capture has only {len(entries)} presents; need at least 5")
    steps: list[float] = []
    reduced: list[np.ndarray] = []
    ticks: list[int] = []
    previous: np.ndarray | None = None
    for path, _, tick, _ in entries:
        image = cadence.load(path, width)
        reduced.append(reduce_luma(image))
        ticks.append(tick)
        if previous is not None:
            steps.append(float(np.abs(image - previous).mean()))
        previous = image
    return steps, reduced, ticks


def cell_step_energy(frames: list[np.ndarray]) -> np.ndarray:
    out = []
    for previous, current in zip(frames, frames[1:]):
        delta = np.abs(current - previous)
        out.append(
            delta.reshape(GRID_H, REDUCED_CELL, GRID_W, REDUCED_CELL).mean(axis=(1, 3))
        )
    return np.asarray(out)


def spatial_alternation(frames: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray]:
    energy = cell_step_energy(frames)
    scores = np.full((GRID_H, GRID_W), np.nan, dtype=np.float64)
    opinions = np.percentile(energy, 90, axis=0) >= STATIC_FLOOR
    for y in range(GRID_H):
        for x in range(GRID_W):
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
    base_manifest = {
        "schema": MANIFEST_SCHEMA,
        "mode": "native",
        "completed": True,
        "gpu_guard": "clean",
        "binary_sha256": "binary",
        "comparator_sha256": "tool",
        "config": {"count": 9, "stage": 1},
        "frames": [{"tick": index, "role": "main"} for index in range(9)],
    }
    lerp_manifest = dict(base_manifest)
    lerp_manifest["mode"] = "lerp"
    lerp_manifest["frames"] = [
        {"tick": index - (index % 2), "role": "main" if index % 2 == 0 else "sub"}
        for index in range(9)
    ]
    checks["matching successful manifests are accepted"] = not pair_manifest_errors(
        base_manifest, lerp_manifest
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
    partial = dict(base_manifest)
    partial["frames"] = [
        {"tick": index + 1, "role": "main"} for index in range(9)
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
    wrong_roles["frames"] = [{"tick": index, "role": "main"} for index in range(9)]
    checks["all-main Lerp60 series is refused"] = bool(
        pair_manifest_errors(base_manifest, wrong_roles)
    )
    contract_args = argparse.Namespace(pad="10:CSTICK=1/2")
    contract_log = "\n".join(
        (
            "[settings] renderer=Aurora framerate=Native 60 FPS",
            "[rt] guest time base: deterministic virtual",
            "[pad] scripted input source: 10:CSTICK=1/2",
            "[pad] scripted input is exclusive; live PAD state ignored",
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
    native_control = dict(base_manifest)
    native_control["mode"] = "native-control"
    checks["identical Native60 repeat is accepted"] = not repeatability_errors(
        base_manifest, native_control
    )
    changed_repeat = dict(native_control)
    changed_repeat["frames"] = [dict(frame) for frame in native_control["frames"]]
    changed_repeat["frames"][4]["sha256"] = "different"
    checks["changed Native60 repeat is refused"] = bool(
        repeatability_errors(base_manifest, changed_repeat)
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
    if not isinstance(recorded_config, dict) or recorded_config.get("width") != args.width:
        raise SystemExit(
            f"REFUSES: --width {args.width} does not match the capture manifest "
            f"({recorded_config.get('width') if isinstance(recorded_config, dict) else 'malformed'})"
        )
    native_steps, native_frames, native_ticks = load_metrics(native_entries, args.width)
    lerp_steps, lerp_frames, lerp_ticks = load_metrics(lerp_entries, args.width)
    overlap_lo = min(native_ticks)
    overlap_hi = max(native_ticks)

    print("\n=== GLOBAL CONSECUTIVE-PRESENT CADENCE ===")
    native_global = cadence.score(native_steps, "native-60", native_ticks)
    lerp_global = cadence.score(lerp_steps, "interpolated-60", lerp_ticks)
    if native_global is None or lerp_global is None:
        raise SystemExit("REFUSES: one cadence series could not be scored")

    native_score, _ = spatial_alternation(native_frames)
    lerp_score, _ = spatial_alternation(lerp_frames)
    forced_frames = duplicate_every_other(native_frames)
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

    shared = np.isfinite(native_score) & np.isfinite(lerp_score)
    if not np.any(shared):
        raise SystemExit("REFUSES: native and lerp captures have no shared moving screen cells")
    excess = np.full_like(native_score, np.nan)
    excess[shared] = lerp_score[shared] - native_score[shared]
    print("\nlerp excess over Native 60 by cell (positive means more fast/slow alternation)")
    ranked = []
    for y, x in zip(*np.where(shared)):
        ranked.append((float(excess[y, x]), x, y))
    for value, x, y in sorted(ranked, reverse=True)[:16]:
        print(
            f"  cell ({x:02d},{y:02d}) frame box "
            f"[{x / GRID_W:.3f},{y / GRID_H:.3f}.."
            f"{(x + 1) / GRID_W:.3f},{(y + 1) / GRID_H:.3f}] "
            f"native={native_score[y, x]:.3f} lerp={lerp_score[y, x]:.3f} excess={value:+.3f}"
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
    parser.add_argument("--after", type=int, default=1600)
    parser.add_argument("--count", type=int, default=33)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--stage", type=int, default=1)
    parser.add_argument("--scenario", type=int, default=0)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--pad", default="400:STICK=0/100+CSTICK=110/0")
    args = parser.parse_args()
    if not args.selftest and args.action is None:
        parser.error(
            "choose capture-native, capture-native-control, capture-lerp, analyze, or --selftest"
        )
    if args.after < 1 or args.count < 5 or args.timeout < 1:
        parser.error("--after >=1, --count >=5, and --timeout >=1 are required")
    if args.count % 2 == 0:
        parser.error("--count must be odd so Native60 and Lerp60 cover the same guest-time span")
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
